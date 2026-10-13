/*
 * fd_pass.c -- hand an open file descriptor to another process over a Unix
 * socket (SCM_RIGHTS), and prove the receiver gets the SAME open file under a
 * DIFFERENT fd number.
 *
 * Why this matters for VMs: a VMM (QEMU) often wants a separate process to run
 * a device, for example a vhost-user backend such as virtiofsd or a DPDK
 * switch.  The backend needs the VMM's eventfds (the ioeventfd "kick" of lab 07
 * and the irqfd "call" of lab 08) and the guest's memory.  Those are open
 * files inside the VMM process, and a file descriptor is just a number that
 * only means something in the process that owns it.  SCM_RIGHTS is how the
 * VMM hands the files themselves over.
 *
 * The program forks into two processes, named after those roles:
 *   "vmm"      (the parent) creates eventfds and sends them,
 *   "backend"  (the child)  receives them and uses them.
 * They take turns, so the output reads in order.  Four steps:
 *
 *   1. Same number, different eventfds.  Each side calls eventfd() and both
 *      get fd 3, yet they are two unrelated counters.
 *   2. Pass an fd.  The vmm sends its fd 3; the backend receives it as fd 4.
 *      Different number, same eventfd (same eventfd-id).
 *   3. Use it across processes.  The backend sleeps in read(4); the vmm's
 *      write(3, 42) wakes it with 42.
 *   4. The file outlives its sender.  The backend sends an eventfd, closes its
 *      copy and exits.  The vmm receives it afterwards and it still works.
 *
 * Every `die()` means "a syscall failed; print why and stop".
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* Both processes move their end of the socket to this fd number.  That leaves
 * 0, 1, 2 (stdin/out/err) and 10 open, so the first eventfd() in either
 * process returns 3, and the demo's fd numbers come out the same every run. */
#define SOCK_FD 10

static void die(const char *what) {
  perror(what);
  exit(1);
}

/* ---------------------------------------------------------------- turn-taking
 * One-byte messages that say "your turn".  They carry no file descriptors;
 * they only keep the two processes' output in order. */

static void give_turn(void) {
  char b = 'T';
  if (write(SOCK_FD, &b, 1) != 1)
    die("write turn");
}

static void wait_turn(void) {
  char b;
  if (read(SOCK_FD, &b, 1) != 1)
    die("read turn");
}

/* ------------------------------------------------------------ SCM_RIGHTS
 *
 * sendmsg()/recvmsg() are write()/read() with extras.  Besides the normal
 * bytes, a message can carry "ancillary data" (also called control messages,
 * or cmsgs): little typed records in a separate buffer.  One cmsg type,
 * SCM_RIGHTS ("socket-level control message: access rights"), carries file
 * descriptors.
 *
 * In memory, the control buffer holds one or more records like this:
 *
 *   struct cmsghdr { cmsg_len, cmsg_level, cmsg_type }  followed by the payload
 *                    (total)   SOL_SOCKET  SCM_RIGHTS       int fds[]
 *
 * The CMSG_* macros do the size and alignment arithmetic:
 *   CMSG_SPACE(n)   bytes the buffer needs for an n-byte payload (with padding)
 *   CMSG_LEN(n)     the value for cmsg_len (header + payload, no trailing pad)
 *   CMSG_FIRSTHDR   the first record in a msghdr's control buffer
 *   CMSG_NXTHDR     the next record, or NULL
 *   CMSG_DATA       where a record's payload starts
 *
 * Only Unix-domain sockets (AF_UNIX) can carry SCM_RIGHTS: the other end has
 * to be on the same machine, since it receives a reference to a kernel
 * object. */

/* The control buffer, sized for one fd.  The union makes the buffer correctly
 * aligned for a struct cmsghdr; a plain char array might not be. */
union fd_cmsg {
  char buf[CMSG_SPACE(sizeof(int))];
  struct cmsghdr align;
};

/* Send the open file behind `fd` to the process at the other end of the socket.
 *
 * What the kernel does here: it looks `fd` up in OUR fd table, takes a
 * reference to the `struct file` it finds, and attaches that pointer to the
 * message.  The number itself isn't sent; it would mean nothing to the
 * receiver. */
static void send_fd(int fd) {
  /* A message must carry at least one normal byte for the fd to ride along
   * with; the value doesn't matter. */
  char byte = 'F';
  struct iovec iov = {.iov_base = &byte, .iov_len = 1};

  union fd_cmsg ctrl;
  memset(&ctrl, 0, sizeof ctrl);

  struct msghdr msg = {
      .msg_iov = &iov, /* the normal bytes: an array of (pointer, length) pieces */
      .msg_iovlen = 1,
      .msg_control = ctrl.buf, /* the ancillary data */
      .msg_controllen = sizeof ctrl.buf,
  };

  struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &fd, sizeof(int)); /* memcpy: CMSG_DATA may be unaligned for int */

  if (sendmsg(SOCK_FD, &msg, 0) != 1)
    die("sendmsg");
}

/* Receive a file sent with send_fd() and return OUR fd number for it.
 *
 * What the kernel does here: for each file attached to the message, it takes
 * the lowest free slot in OUR fd table (as open() would) and points it at
 * that same `struct file`.  So the number we get depends only on which of our
 * slots are free, never on the sender's number. */
static int recv_fd(void) {
  char byte;
  struct iovec iov = {.iov_base = &byte, .iov_len = 1};

  union fd_cmsg ctrl;
  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = ctrl.buf,
      .msg_controllen = sizeof ctrl.buf,
  };

  /* MSG_CMSG_CLOEXEC: mark the new fd close-on-exec, like O_CLOEXEC on
   * open(), so it doesn't leak into programs we might exec later. */
  ssize_t n = recvmsg(SOCK_FD, &msg, MSG_CMSG_CLOEXEC);
  if (n < 0)
    die("recvmsg");
  if (n == 0) {
    fprintf(stderr, "recvmsg: the other side closed the socket\n");
    exit(1);
  }

  /* MSG_CTRUNC: our control buffer was too small, so the kernel dropped
   * (closed) some of the fds.  Always check it: a silently missing fd is a
   * nasty bug. */
  if (msg.msg_flags & MSG_CTRUNC) {
    fprintf(stderr, "recvmsg: control data truncated\n");
    exit(1);
  }

  for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int fd;
      memcpy(&fd, CMSG_DATA(c), sizeof(int));
      return fd;
    }
  }
  fprintf(stderr, "recvmsg: message carried no file descriptor\n");
  exit(1);
}

/* ------------------------------------------------------------- inspecting fds */

/* Read one field from /proc/self/fdinfo/<fd>, the kernel's description of an
 * open fd.  For an eventfd it includes:
 *   eventfd-count:  the counter, in HEX
 *   eventfd-id:     a number unique to this eventfd, in decimal
 * Returns -1 if the field isn't there. */
static long long fdinfo_field(int fd, const char *key, int base) {
  char path[64], line[256];
  snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
  FILE *f = fopen(path, "r");
  if (!f)
    die(path);

  long long val = -1;
  size_t keylen = strlen(key);
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, key, keylen) == 0) {
      val = strtoll(line + keylen, NULL, base); /* strtoll skips the padding spaces */
      break;
    }
  }
  fclose(f);
  return val;
}

/* Print what fd `fd` refers to in THIS process: the /proc/self/fd link (which
 * for every eventfd reads "anon_inode:[eventfd]"), the eventfd's unique id,
 * and its counter.  Reading fdinfo doesn't change the counter, unlike read(). */
static void show(const char *who, int fd) {
  char path[64], target[64];
  snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
  ssize_t n = readlink(path, target, sizeof target - 1);
  if (n < 0)
    die(path);
  target[n] = '\0';

  printf("  [%-7s] fd %d -> %s  eventfd-id %lld  count %lld\n", who, fd, target, fdinfo_field(fd, "eventfd-id:", 10), fdinfo_field(fd, "eventfd-count:", 16));
}

/* eventfd read/write: always exactly 8 bytes, one 64-bit number.
 * write(v) adds v to the counter.  read() returns the counter and resets it to
 * 0, or sleeps until it's non-zero. */
static void efd_write(int fd, uint64_t v) {
  if (write(fd, &v, sizeof v) != sizeof v)
    die("eventfd write");
}

static uint64_t efd_read(int fd) {
  uint64_t v;
  if (read(fd, &v, sizeof v) != sizeof v)
    die("eventfd read");
  return v;
}

/* ----------------------------------------------------------------- the vmm */

static void vmm(pid_t backend_pid) {
  printf("STEP 1: each process calls eventfd(0, 0)\n");
  int efd = eventfd(0, 0);
  if (efd < 0)
    die("eventfd");
  show("vmm", efd);
  give_turn(); /* backend: create its own eventfd, write 5 to it */
  wait_turn();
  show("vmm", efd);
  printf("  => both got fd %d, but the ids differ and the backend's write(5) didn't\n"
         "     touch the vmm's counter: two separate eventfds.\n\n",
         efd);

  printf("STEP 2: the vmm sends its fd %d with SCM_RIGHTS\n", efd);
  send_fd(efd);
  wait_turn(); /* backend: receive it and show it */
  printf("  => a different number in the backend, the same eventfd-id: one eventfd,\n"
         "     now reachable from two fd tables.\n\n");

  printf("STEP 3: signal across processes through the shared eventfd\n");
  give_turn();
  wait_turn(); /* backend is about to sleep in read() */
  usleep(200 * 1000);
  printf("  [vmm    ] 200 ms later: write(%d, 42)\n", efd);
  efd_write(efd, 42);
  wait_turn(); /* backend woke up */
  printf("\n");

  printf("STEP 4: the backend sends an eventfd, closes its copy, and exits\n");
  give_turn();
  if (waitpid(backend_pid, NULL, 0) < 0)
    die("waitpid");
  printf("  [vmm    ] backend process has exited; now calling recvmsg()\n");
  int got = recv_fd();
  show("vmm", got);
  printf("  [vmm    ] read(%d) -> %llu\n", got, (unsigned long long)efd_read(got));
  printf("  => the sender closed it and is gone, yet the eventfd and its count\n"
         "     survived: the message itself held a reference while in flight.\n");

  close(got);
  close(efd);
}

/* ------------------------------------------------------------- the backend */

static void backend(void) {
  wait_turn();
  int own = eventfd(0, 0);
  if (own < 0)
    die("eventfd");
  efd_write(own, 5);
  show("backend", own);
  give_turn();

  int shared = recv_fd();
  show("backend", shared);
  give_turn();

  wait_turn();
  printf("  [backend] read(%d): nothing written yet, so I sleep...\n", shared);
  give_turn();
  uint64_t v = efd_read(shared); /* blocks until the vmm writes */
  printf("  [backend] ...woke up, read() returned %llu\n", (unsigned long long)v);
  give_turn();

  wait_turn();
  int mine = eventfd(0, 0);
  if (mine < 0)
    die("eventfd");
  efd_write(mine, 7);
  show("backend", mine);
  send_fd(mine);
  close(mine);
  printf("  [backend] sent fd %d, closed it, exiting\n", mine);
  /* returning to main() exits the process, closing every fd we still hold */
}

/* ------------------------------------------------------------------- main */

/* Move our socket end to SOCK_FD and close the other end (it belongs to the
 * other process). */
static void place_socket(int keep, int other) {
  close(other);
  if (dup2(keep, SOCK_FD) < 0)
    die("dup2");
  close(keep);
}

int main(void) {
  /* Line-buffer stdout even when it's a pipe (e.g. under `make run`), so each
   * printf goes out at its newline and the two processes don't interleave. */
  setvbuf(stdout, NULL, _IOLBF, 0);

  /* A connected pair of Unix sockets: what goes into one comes out of the
   * other.  The two processes here are related (fork), so socketpair() is the
   * simplest way to connect them.  Unrelated processes do the same thing over
   * a named socket (bind()/connect() to a path), as vhost-user does; the
   * sendmsg/recvmsg code is identical. */
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    die("socketpair");

  /* No eventfd exists yet, so the child can't inherit one: anything the
   * backend ends up sharing with the vmm must arrive through the socket. */
  pid_t pid = fork();
  if (pid < 0)
    die("fork");

  if (pid == 0) { /* Child process */
    place_socket(sv[1], sv[0]);
    backend();
    return 0;
  }
  /* Parent process */
  place_socket(sv[0], sv[1]);
  vmm(pid);
  return 0;
}
