/*
 * uring_seccomp.c -- why io_uring is awkward to sandbox, demonstrated.
 *
 * BACKGROUND: WHAT seccomp IS
 * ---------------------------
 * seccomp ("secure computing") lets a process install a filter that the kernel
 * runs on EVERY system call the process (and its future children) makes.  The
 * filter is a small classic-BPF program -- a tiny, verifiable bytecode -- that
 * looks at `struct seccomp_data` (architecture, syscall number, instruction
 * pointer, and the six raw argument registers) and returns a verdict: ALLOW,
 * ERRNO (fail the call with an error), KILL, and a few others.  Container
 * runtimes, browsers and VMMs (Firecracker ships a strict per-thread filter)
 * use it to shrink the kernel attack surface.  Once installed, a filter can
 * never be removed; more filters can only be added.
 *
 * THE PROBLEM
 * -----------
 * seccomp-BPF filters syscalls: it sees the syscall number and 6 raw register
 * arguments.  But io_uring turns *operations* into data in a shared ring.  The
 * only syscall is io_uring_enter(), whose arguments are (fd, to_submit, ...).
 * The fact that some SQE says "IORING_OP_READ on fd 5" or "IORING_OP_OPENAT" or
 * "IORING_OP_CONNECT" never passes through a syscall boundary seccomp can see.
 * (seccomp also cannot follow pointers into memory, so it could not inspect
 * the SQEs even if it wanted to.)
 *
 *   filter says:  read(2)  -> EPERM
 *   attacker:     IORING_OP_READ -> works
 *
 * This program proves it, then shows the two mitigations:
 *
 *   PART 1  seccomp denies read(2)/pread64(2).  read() fails.  io_uring READ
 *           on the same fd succeeds.  The filter is bypassed.
 *
 *   PART 2  Mitigation A -- deny io_uring_* syscalls entirely in the sandbox
 *           (the approach taken by Google for ChromeOS, Android apps and its
 *           production servers, and by recent container-runtime default
 *           seccomp profiles).  Blunt, but sound.
 *
 *   PART 3  Mitigation B -- io_uring's own restriction API
 *           (IORING_SETUP_R_DISABLED + IORING_REGISTER_RESTRICTIONS): create
 *           the ring disabled, whitelist opcodes, then enable.  Anything else
 *           completes with -EACCES.  Use this if the *workload* needs a ring
 *           you do not fully trust.
 *
 * Also relevant, host-wide (Linux >= 6.6): sysctl kernel.io_uring_disabled =
 *   0 (anyone), 1 (only CAP_SYS_ADMIN or members of sysctl kernel.io_uring_group
 *   may create rings), 2 (nobody).
 *
 * The seccomp parts run in forked children so the filter cannot affect the parent.
 * BUILD & RUN:  make && ./uring_seccomp
 */
#include "mini_uring.h"

#include <fcntl.h>
#include <linux/audit.h>   /* AUDIT_ARCH_* constants                       */
#include <linux/filter.h>  /* struct sock_filter, BPF_STMT/BPF_JUMP macros */
#include <linux/seccomp.h> /* SECCOMP_* return values, struct seccomp_data */
#include <stddef.h>        /* offsetof                                     */
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>

/* seccomp_data.arch identifies the calling convention of the syscall. */
#if defined(__x86_64__)
#define THIS_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define THIS_ARCH AUDIT_ARCH_AARCH64
#else
#error "add your architecture"
#endif

/*
 * install_deny_filter -- build and load a classic-BPF seccomp filter that
 * returns EPERM for each syscall in `deny[]` and allows everything else.
 *
 * Classic BPF in one paragraph: an accumulator machine.  BPF_LD|BPF_W|BPF_ABS
 * loads a 32-bit word from offset k of seccomp_data into the accumulator;
 * BPF_JMP|BPF_JEQ|BPF_K compares it with constant k and skips `jt`
 * instructions if equal, `jf` if not (offsets are relative to the NEXT
 * instruction); BPF_RET returns the verdict.  Only forward jumps exist, so
 * every filter terminates.
 *
 * Program layout (n = number of denied syscalls):
 *   0: load arch
 *   1: if arch == THIS_ARCH skip next
 *   2: KILL                      <- wrong ABI (e.g. 32-bit compat syscalls)
 *   3: load syscall number
 *   4..4+n-1: if nr == deny[k] jump to DENY
 *   4+n:   ALLOW
 *   5+n:   DENY (return ERRNO|EPERM)
 * Always check `arch` first: syscall numbers differ between ABIs and an
 * attacker could otherwise switch ABI to dodge the number match.
 * (One more hole a production x86-64 filter closes: the x32 ABI reports
 * AUDIT_ARCH_X86_64 too, with syscall numbers that have bit 30
 * (__X32_SYSCALL_BIT) set, so it also rejects nr >= 0x40000000.  x32 is
 * compiled out of most distro kernels; this demo does not bother.)
 */
static void install_deny_filter(const int *deny, int n) {
  struct sock_filter f[64];
  int i = 0;
  f[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
  f[i++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, THIS_ARCH, 1, 0);
  f[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
  f[i++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
  for (int k = 0; k < n; k++)
    /* jt = n-k: skip the remaining (n-k-1) compares and the ALLOW. */
    f[i++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, deny[k], n - k, 0);
  f[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
  /* The low 16 bits of an ERRNO verdict become the errno the caller sees. */
  f[i++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);

  struct sock_fprog prog = {.len = i, .filter = f};

  /* Required so an unprivileged process may install a filter; also means
   * setuid binaries can't be used to escape it.  (NO_NEW_PRIVS: execve()
   * will never grant more privileges, e.g. via setuid bits or file caps.
   * Without it, only a CAP_SYS_ADMIN process may install a filter.) */
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
    perror("NO_NEW_PRIVS");
    exit(1);
  }
  if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
    perror("SECCOMP");
    exit(1);
  }
}

/* Submit ONE read via io_uring and return cqe->res (bytes or -errno).
 * The read itself is performed inside the kernel by io_uring, which never
 * goes through the read() system-call entry point -- so seccomp never runs. */
static int uring_read_once(struct mu_ring *r, int fd, char *buf, unsigned len) {
  struct io_uring_sqe *sqe = mu_get_sqe(r);
  mu_prep_rw(sqe, IORING_OP_READ, fd, buf, len, 0, 1);
  mu_submit(r, 1); /* submit + wait for 1 completion: one io_uring_enter */
  struct io_uring_cqe *cqe = mu_peek_cqe(r);
  int res = cqe ? cqe->res : -EIO;
  if (cqe)
    mu_cqe_seen(r);
  return res;
}

/* Run `fn` in a forked child and wait; keeps filters away from the parent.
 * fflush before fork: otherwise unflushed stdout data would be duplicated into
 * the child and printed twice.  The child ends with _exit(), which skips
 * atexit handlers and stdio flushing of inherited state. */
static void in_child(void (*fn)(void)) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    fn();
    fflush(stdout);
    _exit(0);
  }
  int st;
  waitpid(pid, &st, 0);
  if (WIFSIGNALED(st))
    printf("  child killed by signal %d\n", WTERMSIG(st));
}

/* ------------------------------------------------------------------ PART 1 */
static void part1(void) {
  printf("PART 1: seccomp denies read(2), but io_uring READ still works\n");
  int fd = open("/proc/version", O_RDONLY); /* opened BEFORE the filter */
  char buf[64];

  /* The ring is created before the filter, like a sandbox that allows
   * io_uring_setup/enter because "they are not read()". */
  struct mu_ring r;
  if (mu_setup(&r, 4, 0, 0) < 0) {
    puts("  io_uring unavailable");
    return;
  }

  /* Every classic way to read from an fd, denied.  (A real sandbox would
   * need to think about many more: mmap, sendfile, splice, ...) */
  const int deny[] = {SYS_read, SYS_pread64, SYS_readv, SYS_preadv, SYS_preadv2};
  install_deny_filter(deny, sizeof deny / sizeof deny[0]);
  puts("  [seccomp filter installed: read/pread64/readv/preadv/preadv2 -> EPERM]");

  ssize_t n = read(fd, buf, sizeof buf - 1);
  printf("  read(2)              -> %zd (%s)\n", n, n < 0 ? strerror(errno) : "ok");
  n = pread(fd, buf, sizeof buf - 1, 0);
  printf("  pread(2)             -> %zd (%s)\n", n, n < 0 ? strerror(errno) : "ok");

  int res = uring_read_once(&r, fd, buf, sizeof buf - 1);
  if (res > 0) {
    buf[res > 20 ? 20 : res] = 0; /* print only the first 20 bytes */
    printf("  IORING_OP_READ       -> %d bytes: \"%s...\"   <== BYPASSED THE FILTER\n", res, buf);
  } else {
    printf("  IORING_OP_READ       -> %d (%s)\n", res, strerror(-res));
  }
}

/* ------------------------------------------------------------------ PART 2 */
static void part2(void) {
  printf("PART 2: mitigation A -- deny io_uring_setup/enter/register\n");
  const int deny[] = {SYS_io_uring_setup, SYS_io_uring_enter, SYS_io_uring_register};
  install_deny_filter(deny, 3);
  struct mu_ring r;
  int rc = mu_setup(&r, 4, 0, 0); /* io_uring_setup now fails with EPERM */
  printf("  io_uring_setup()     -> %s\n", rc < 0 ? strerror(-rc) : "succeeded (?!)");
  puts("  (an already-open ring fd inherited from before the filter would still\n"
       "   work if io_uring_enter were allowed, so deny the whole family.)");
}

/* ------------------------------------------------------------------ PART 3 */
static void part3(void) {
  printf("PART 3: mitigation B -- ring-level opcode allow-list (IORING_REGISTER_RESTRICTIONS)\n");
  struct mu_ring r;

  /* Create the ring DISABLED: no SQE is processed until we enable it, and
   * restrictions can only be registered while disabled (and only once). */
  int rc = mu_setup(&r, 4, IORING_SETUP_R_DISABLED, 0);
  if (rc < 0) {
    printf("  setup failed: %s\n", strerror(-rc));
    return;
  }

  /* Whitelist: only NOP and WRITE.  Everything not listed => -EACCES.
   * Other restriction kinds can also limit which io_uring_register()
   * operations and which SQE flags are allowed. */
  struct io_uring_restriction res[2];
  memset(res, 0, sizeof res); /* reserved fields must be zero */
  res[0].opcode = IORING_RESTRICTION_SQE_OP;
  res[0].sqe_op = IORING_OP_NOP;
  res[1].opcode = IORING_RESTRICTION_SQE_OP;
  res[1].sqe_op = IORING_OP_WRITE;
  if (mu_register(&r, IORING_REGISTER_RESTRICTIONS, res, 2) < 0) {
    perror("  register restrictions");
    return;
  }
  /* Restrictions are frozen once the ring is enabled.  The intended use:
   * a trusted parent sets this up, then hands the ring fd to untrusted code. */
  if (mu_register(&r, IORING_REGISTER_ENABLE_RINGS, NULL, 0) < 0) {
    perror("  enable rings");
    return;
  }
  puts("  ring restricted to {NOP, WRITE}");

  int fd = open("/proc/version", O_RDONLY);
  char buf[16];
  /* The READ SQE is accepted by the syscall but fails individually: its CQE
   * carries -EACCES (-13). */
  int r1 = uring_read_once(&r, fd, buf, sizeof buf);
  printf("  IORING_OP_READ       -> %d (%s)\n", r1, r1 < 0 ? strerror(-r1) : "allowed (?!)");

  struct io_uring_sqe *sqe = mu_get_sqe(&r);
  sqe->opcode = IORING_OP_NOP;
  mu_submit(&r, 1);
  struct io_uring_cqe *cqe = mu_peek_cqe(&r);
  printf("  IORING_OP_NOP        -> %d (allowed)\n", cqe ? cqe->res : -1);
  if (cqe)
    mu_cqe_seen(&r);

  struct io_uring_sqe *sqe2 = mu_get_sqe(&r);
  sqe2->opcode = IORING_OP_WRITE;
  mu_submit(&r, 1);
  struct io_uring_cqe *cqe2 = mu_peek_cqe(&r);
  printf("  IORING_OP_WRITE      -> %d (allowed)\n", cqe2 ? cqe2->res : -1);
  if (cqe2)
    mu_cqe_seen(&r);
}

int main(void) {
  in_child(part1);
  puts("");
  in_child(part2);
  puts("");
  in_child(part3);
  return 0;
}
