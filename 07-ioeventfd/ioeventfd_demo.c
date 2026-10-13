/*
 * ioeventfd_demo.c -- how a guest VM can "ring a doorbell" on the host
 *                     without waking up the host process that manages it.
 *
 * BACKGROUND: what a VM exit is
 * ------------------------------
 * A virtual machine's CPU instructions run for real, directly on your CPU
 * (that's what "hardware virtualization", KVM, is for). But the guest OS
 * doesn't own real hardware -- when it tries to talk to a "disk" or
 * "network card", it's actually just reading/writing to certain memory
 * addresses or I/O ports that the host process (the VMM -- Virtual Machine
 * Monitor, e.g. QEMU) has decided will mean something.
 *
 * When the guest touches one of those addresses, the CPU can't complete the
 * access (there's no real memory or device there), so it stops the guest and
 * hands control back to the host kernel. This is called a VM exit. Normally
 * a VM exit is expensive: the guest freezes, the host's device-emulating
 * thread wakes up, decodes what the guest was trying to do, does it, and
 * resumes the guest. Every one of those steps costs time -- easily
 * microseconds, which is enormous compared to a single CPU instruction.
 *
 * (The precise mechanism on Intel is "EPT" -- Extended Page Tables, a second
 *  layer of address translation the hardware checks. AMD's equivalent is
 *  NPT. If an address was never mapped there, touching it faults, and that
 *  fault becomes the VM exit. Full walkthrough: see 02-hypercalls.)
 *
 * THE PROBLEM THIS LAB SOLVES
 * ---------------------------
 * Some devices don't need the full "decode and emulate" treatment -- they
 * just need a *notification*, a "hey, something happened" doorbell. For
 * example: "I put a new network packet in the queue, please send it." The
 * guest doesn't need an answer, and the host doesn't need to interpret
 * anything about the write except that it happened. Making the host's main
 * thread wake up, run C code, and go back to sleep for every single one of
 * these doorbells is wasteful.
 *
 * THE FIX: ioctl(vm, KVM_IOEVENTFD, ...)
 * ---------------------------------------
 * You tell KVM up front: "if the guest ever writes to this exact address,
 * don't bother my userspace program at all -- just bump this counter for me
 * and let the guest keep running immediately." The counter is an eventfd, a
 * tiny kernel object that is just a 64-bit number plus a way to wait for it
 * to change.
 *
 * The CPU still takes the VM exit (that part is unavoidable -- there's still
 * no real memory there), but KVM handles the *entire* exit inside the
 * kernel: it increments the eventfd and immediately resumes the guest. The
 * host's main VMM thread is never woken up. Some other part of the host
 * (a background thread, or even another piece of kernel code called vhost)
 * can watch that eventfd separately and do the real work whenever it's
 * convenient, on any CPU it likes.
 *
 * (An eventfd is a file descriptor backed by a 64-bit counter. write(fd, ...)
 *  adds a number to the counter and wakes up anyone waiting on it; read(fd,
 *  ...) hands back the counter's value and resets it to 0. Normally *you*
 *  call write(). Here, KVM itself does the "write" from inside the kernel
 *  whenever the guest hits the registered address.)
 *
 * WHAT THIS PROGRAM PROVES
 * -------------------------
 * A tiny 16-bit guest program runs three back-to-back experiments so you can
 * see the difference with your own eyes:
 *
 *  1. FAST PATH: the guest writes to address A, 200,000 times. A has an
 *     ioeventfd registered on it.
 *     Result: the host process sees ZERO of these writes (0 KVM_EXIT_MMIO
 *     events reach it) -- yet the eventfd's counter reads exactly 200000.
 *     KVM was quietly counting every one in the kernel.
 *
 *  2. SLOW PATH: the guest writes to address B, 200,000 times. B has no
 *     ioeventfd.
 *     Result: the host process is woken up 200,000 times, once per write,
 *     and each write takes noticeably longer for the guest, because the
 *     guest has to sit frozen until the host handles it.
 *
 *  3. DATAMATCH -- telling writes apart by their VALUE, not just their
 *     address: two ioeventfds are registered on the SAME address C: one
 *     fires only when the guest writes the value 1, the other only for the
 *     value 2. The guest writes 0, then 1, then 2, then 3 to C.
 *     Result: the write of 1 bumps the first eventfd, the write of 2 bumps
 *     the second, and 0 and 3 (matching neither) fall through to the slow
 *     path, same as experiment 2. This is exactly how a real disk/network
 *     device (virtio) tells "please look at queue 0" apart from "please
 *     look at queue 1" -- the queue number IS the value written.
 *
 * Nobody actually reads the eventfds while the guest is running -- that's
 * fine, because an eventfd is just a counter that keeps accumulating; we
 * read the final totals once the guest is done. (A real VMM would instead
 * have a background thread permanently blocked in read() or epoll(),
 * waiting to react the instant a doorbell rings.)
 *
 * BUILD & RUN:   make && ./ioeventfd_demo        (needs /dev/kvm access)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Turns a #defined number into a string literal at compile time, e.g.
 * STR(200000) becomes "200000", so it can be pasted straight into an
 * assembly instruction below (assembly doesn't know about C's #define). */
#define STR_(x) #x
#define STR(x) STR_(x)

#define N_WRITES 200000 /* how many times the guest writes, per experiment */

/* Map of the guest's tiny "physical" address space, all made up by us:
 *   0x0000 - 0xffff : real, backed by actual host RAM (64 KiB)
 *   0xd0000 and up  : NOT backed by anything -- any guest access there
 *                     is a doorbell/MMIO address, never real memory.
 * Think of these 0xd_xxx numbers as fake "hardware registers" we invented
 * for this demo, the same way a real disk controller has registers at
 * fixed addresses that the OS driver writes to. */
#define GUEST_MEM_SIZE 0x10000 /* 64 KiB of guest RAM, starting at address 0 */
#define GUEST_LOAD_GPA 0x1000  /* where we place the guest's tiny program   */
#define MMIO_A 0xd0000         /* experiment 1: HAS an ioeventfd             */
#define MMIO_B 0xd1000         /* experiment 2: has NO ioeventfd             */
#define MMIO_C 0xd2000         /* experiment 3: two ioeventfds, by value     */

/* A second, separate signalling channel: x86 "port I/O" (the `out`
 * instruction), used here only so the guest can tell the host "start timing
 * now" / "stop timing now" / "I'm completely done". Unlike the MMIO_* writes
 * above, these are NOT the thing being measured -- they always cause a VM
 * exit, on purpose, so the host can act on them.
 * (Ports 0x21/0x22 are ordinarily used by the real interrupt controller
 *  chip; it's safe to reuse them here only because this toy VM never creates
 *  one. A real VM with KVM_CREATE_IRQCHIP -- see labs 08/09 -- would own
 *  port 0x21 itself.) */
#define PORT_BEGIN 0x21
#define PORT_END 0x22
#define PORT_DONE 0xf0

/* ---------------------------------------------------------------- guest ---
 * This is the entire "operating system" running inside our toy VM: a
 * handful of 16-bit x86 instructions, hand-written in assembly. It is NOT
 * compiled for our host CPU's normal 64-bit mode -- real CPUs always start
 * up in an old, simple 16-bit mode (called "real mode") for backwards
 * compatibility, and that's the mode our vCPU boots into as well, so this
 * code has to speak its language.
 *
 * `movl $1, %es:0` means: "write the 4-byte value 1 to memory address
 * (es segment * 16) + offset 0". By pointing the `es` register at different
 * places before each block below, the exact same instruction ends up
 * writing to MMIO_A, then MMIO_B, then MMIO_C.
 *
 * This assembly is baked into the compiled binary as plain data (`.rodata`)
 * -- your host CPU never executes it directly. main() below copies these
 * bytes into the guest's fake RAM, and only the *virtual* CPU (the vCPU)
 * executes them, one instruction at a time, in its own isolated world.
 * For a fully-commented, line-by-line introduction to this technique, see
 * 02-hypercalls/kvm_exits.c.
 */
// clang-format off
extern const char guest_start[], guest_end[];
__asm__(
    ".pushsection .rodata\n"
    ".code16\n"
    ".globl guest_start, guest_end\n"
    "guest_start:\n"
    "    cli\n"                      /* disable interrupts -- we don't use any */
    "    xor %ax, %ax\n"
    "    mov %ax, %ds\n"
    "    mov %ax, %ss\n"
    "    mov $0x8000, %sp\n"         /* give ourselves a stack, inside guest RAM */

    /* ---- experiment 1: hammer address A (has an ioeventfd) ---- */
    "    mov $0xd000, %bx\n"
    "    mov %bx, %es\n"             /* es:0 now points at MMIO_A (0xd0000)     */
    "    mov $" STR(PORT_BEGIN) ", %dx\n"
    "    out %al, %dx\n"             /* tell the host: "start the stopwatch"    */
    "    mov $" STR(N_WRITES) ", %esi\n" /* loop counter                       */
    "1:  movl $1, %es:0\n"           /* <-- the actual "doorbell": one write   */
    "    dec %esi\n"
    "    jnz 1b\n"                   /* repeat N_WRITES times                  */
    "    mov $" STR(PORT_END) ", %dx\n"
    "    out %al, %dx\n"             /* tell the host: "stop the stopwatch"     */

    /* ---- experiment 2: same thing, but to address B (no ioeventfd) ---- */
    "    mov $0xd100, %bx\n"
    "    mov %bx, %es\n"             /* es:0 now points at MMIO_B (0xd1000)     */
    "    mov $" STR(PORT_BEGIN) ", %dx\n"
    "    out %al, %dx\n"
    "    mov $" STR(N_WRITES) ", %esi\n"
    "2:  movl $1, %es:0\n"
    "    dec %esi\n"
    "    jnz 2b\n"
    "    mov $" STR(PORT_END) ", %dx\n"
    "    out %al, %dx\n"

    /* ---- experiment 3: write four different VALUES to address C ---- */
    "    mov $0xd200, %bx\n"
    "    mov %bx, %es\n"             /* es:0 now points at MMIO_C (0xd2000)     */
    "    movl $0, %es:0\n"           /* matches nothing  -> falls through       */
    "    movl $1, %es:0\n"           /* matches eventfd #1                      */
    "    movl $2, %es:0\n"           /* matches eventfd #2                      */
    "    movl $3, %es:0\n"           /* matches nothing  -> falls through       */

    "    mov $" STR(PORT_DONE) ", %dx\n"
    "    out %al, %dx\n"             /* tell the host: "all three experiments done" */
    "    hlt\n"                      /* stop the (virtual) CPU                  */
    "guest_end:\n"
    ".code64\n"
    ".popsection\n");
// clang-format on

static void die(const char *w) {
  perror(w);
  exit(1);
}

/* Current time in nanoseconds, from a clock that only ever moves forward
 * (unaffected by someone changing the wall-clock date) -- what you want for
 * measuring "how long did that take". */
static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/*
 * Ask KVM to watch one address and silently count writes to it in an
 * eventfd, without ever bothering the host process.
 *
 *   addr      : the guest address to watch (one of the MMIO_* constants).
 *   len       : how many bytes the guest write must be for this to count
 *               (1, 2, 4 or 8). Must match the width the guest actually
 *               writes (`movl` above writes 4 bytes, so we pass len=4).
 *               len=0 is a special case meaning "any width, don't even
 *               bother decoding what the guest wrote" -- the fastest option,
 *               but it can't be combined with datamatch below.
 *   match     : if non-zero, ONLY a write of exactly `value` triggers this
 *               eventfd (other values fall through to the host, as if no
 *               ioeventfd were registered at all). This is how experiment 3
 *               tells writes of 1 apart from writes of 2 on the same address.
 *   value     : the value to match, when match is set.
 *
 * Returns a new eventfd -- a file descriptor whose 64-bit counter KVM will
 * increment by 1, entirely inside the kernel, every time this address (and,
 * if requested, this exact value) is written by the guest.
 *
 * (There's also KVM_IOEVENTFD_FLAG_PIO, not used here, which does the same
 * thing for x86 I/O ports instead of memory addresses; and
 * KVM_IOEVENTFD_FLAG_DEASSIGN, which un-registers one.)
 */
static int add_ioeventfd(int vm, uint64_t addr, uint32_t len, int match, uint64_t value) {
  int fd = eventfd(0, EFD_NONBLOCK); /* non-blocking: our drain() below must never block */
  if (fd < 0)
    die("eventfd");
  struct kvm_ioeventfd io = {
      .addr = addr,
      .len = len,
      .fd = fd,
      .datamatch = value,
      .flags = match ? KVM_IOEVENTFD_FLAG_DATAMATCH : 0,
  };
  if (ioctl(vm, KVM_IOEVENTFD, &io) < 0)
    die("KVM_IOEVENTFD");
  return fd;
}

/* Read an eventfd's current counter value and reset it to 0 in the same
 * step ("drain" it). If the counter is currently 0, read() would normally
 * block forever waiting for it to become non-zero -- but we made the fd
 * non-blocking above, so it instead fails immediately with EAGAIN, which we
 * just treat as "nothing happened yet, value is 0". */
static uint64_t drain(int efd) {
  uint64_t v = 0;
  if (read(efd, &v, sizeof v) < 0)
    return 0; /* EAGAIN: counter was zero */
  return v;
}

int main(void) {
  alarm(30); /* safety net: kill ourselves after 30s instead of hanging forever if a bug loops the guest */

  /* --------------------------------------------------- create the VM ---
   * This is the standard three-step KVM setup, explained fully in
   * 02-hypercalls: open the device, ask it for a fresh VM, give that VM
   * some memory. */
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0)
    die("open /dev/kvm");
  int vm = ioctl(kvm, KVM_CREATE_VM, 0);
  if (vm < 0)
    die("KVM_CREATE_VM");

  /* Hand the VM 64 KiB of ordinary host memory to use as its "RAM", starting
   * at guest address 0. Anything the guest touches OUTSIDE this range (like
   * our MMIO_A/B/C addresses) has nothing behind it, which is exactly what
   * makes those addresses act as doorbells instead of real memory. */
  uint8_t *guest_mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  struct kvm_userspace_memory_region rg = {
      .slot = 0,
      .guest_phys_addr = 0,
      .memory_size = GUEST_MEM_SIZE,
      .userspace_addr = (uintptr_t)guest_mem,
  };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &rg) < 0)
    die("memslot");
  /* Copy our tiny guest program (the assembly above) into that RAM. */
  memcpy(guest_mem + GUEST_LOAD_GPA, guest_start, guest_end - guest_start);

  /* ------------------------------------------- set up the three doorbells
   * Register the ioeventfds BEFORE the guest ever runs, so nothing can be
   * missed. Note these belong to the VM as a whole, not to any one virtual
   * CPU: if this VM had several vCPUs, a write from ANY of them to these
   * addresses would land on the same eventfd. */
  int efd_a = add_ioeventfd(vm, MMIO_A, 4, 0, 0);  /* any 4-byte write to A counts        */
  int efd_c1 = add_ioeventfd(vm, MMIO_C, 4, 1, 1); /* only a write of the value 1 to C     */
  int efd_c2 = add_ioeventfd(vm, MMIO_C, 4, 1, 2); /* only a write of the value 2 to C     */
  printf("registered ioeventfd: A=0x%x (any value) | C=0x%x datamatch==1 | C=0x%x datamatch==2\n\n", MMIO_A, MMIO_C, MMIO_C);

  /* ------------------------------------------------- create the vCPU ---
   * A vCPU is the virtual CPU our guest program actually executes on.
   * `run` is a page of shared memory KVM uses to tell us WHY control came
   * back to us every time the guest stops (a VM exit). */
  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  if (vcpu < 0)
    die("KVM_CREATE_VCPU");
  long sz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);

  /* Point the vCPU at real mode with a flat (base 0) code segment, and set
   * its instruction pointer to where we loaded our guest program. See
   * 02-hypercalls for why these particular register values are needed to
   * boot into 16-bit real mode. */
  struct kvm_sregs sr;
  ioctl(vcpu, KVM_GET_SREGS, &sr);
  sr.cs.base = 0;
  sr.cs.selector = 0;
  ioctl(vcpu, KVM_SET_SREGS, &sr);
  struct kvm_regs regs = {.rip = GUEST_LOAD_GPA, .rflags = 2};
  ioctl(vcpu, KVM_SET_REGS, &regs);

  /* --------------------------------------------------------- run the guest
   * KVM_RUN is the main loop of any hypervisor: it lets the guest execute
   * real instructions on the real CPU until something happens that the
   * hardware can't handle by itself (an exit). We then look at
   * run->exit_reason to find out what happened, react, and call KVM_RUN
   * again to let the guest continue.
   *
   * Crucially: writes to A (and matching writes to C) NEVER show up as an
   * exit_reason here at all -- KVM already dealt with them using the
   * ioeventfds before returning to us. Only the port-I/O markers, the
   * unmatched writes to C, and every single write to B make it this far. */
  unsigned long mmio_exits = 0;     /* count of KVM_EXIT_MMIO we actually saw */
  unsigned long mmio_exits_exp2 = 0; /* snapshot of mmio_exits right after experiment 1 (should stay 0) */
  unsigned long kvm_run_calls = 0;   /* how many times KVM_RUN returned to us, total */
  int nunm = 0;                      /* how many "unmatched" writes to C we've seen */
  uint32_t unm_vals[4];              /* their values, for the printout below */
  double t0 = 0, t_exp1 = 0, t_exp2 = 0; /* stopwatch state for experiments 1 and 2 */
  int phase = 1;                     /* which experiment's BEGIN/END markers we're between */

  for (int running = 1; running;) {
    if (ioctl(vcpu, KVM_RUN, 0) < 0) {
      if (errno == EINTR) /* interrupted by a signal -- harmless, just retry */
        continue;
      die("KVM_RUN");
    }
    kvm_run_calls++; /* we only get here at all when something needed OUR attention */
    switch (run->exit_reason) {

    case KVM_EXIT_IO: {
      /* The guest used `out` -- one of our BEGIN/END/DONE markers. */
      uint16_t port = run->io.port;
      if (port == PORT_BEGIN) {
        t0 = now_ns(); /* start timing this experiment */
      } else if (port == PORT_END) {
        double dt = now_ns() - t0;
        if (phase == 1) {
          t_exp1 = dt;
          mmio_exits_exp2 = mmio_exits; /* lock in experiment 1's exit count */
        } else {
          t_exp2 = dt;
        }
        phase++;
      } else if (port == PORT_DONE) {
        running = 0; /* guest signalled it's finished all three experiments */
      }
      break;
    }

    case KVM_EXIT_MMIO: {
      /* We only land here for a write that NO ioeventfd claimed: either a
       * write to B, or an unmatched write (0 or 3) to C. A real device
       * emulator would decode and act on it here; we just count it. */
      mmio_exits++;
      if (run->mmio.phys_addr >= MMIO_C && nunm < 4) {
        memcpy(&unm_vals[nunm++], run->mmio.data, 4);
      }
      break;
    }

    case KVM_EXIT_HLT:
      running = 0; /* guest executed `hlt`, i.e. it's done */
      break;

    default:
      fprintf(stderr, "unexpected exit %u\n", run->exit_reason);
      return 1;
    }
  }

  /* ---------------------------------------------------------------- report
   * Now, after the guest has stopped, we finally look at the doorbell
   * counters. This is safe because an eventfd never loses counts -- it just
   * keeps accumulating until someone reads it. */
  uint64_t a = drain(efd_a), c1 = drain(efd_c1), c2 = drain(efd_c2);

  printf("EXPERIMENT 1: %d guest writes to A (ioeventfd registered)\n", N_WRITES);
  printf("   eventfd counter after the run   : %lu   <- KVM counted every kick in-kernel\n", (unsigned long)a);
  printf("   KVM_EXIT_MMIO seen by userspace : %lu   <- our loop above never even saw these\n", mmio_exits_exp2);
  printf("   guest time per write            : %.0f ns\n\n", t_exp1 / N_WRITES);

  /* Experiment 2's exit count = every MMIO exit we saw in total, minus
   * experiment 1's (which was 0), minus the 2 unmatched writes from
   * experiment 3 (which happens afterwards but got mixed into the same
   * running total). */
  printf("EXPERIMENT 2: %d guest writes to B (NO ioeventfd)\n", N_WRITES);
  printf("   KVM_EXIT_MMIO seen by userspace : %lu   <- every single write woke us up\n", mmio_exits - mmio_exits_exp2 - (unsigned long)nunm);
  printf("   guest time per write            : %.0f ns\n", t_exp2 / N_WRITES);
  printf("   => ioeventfd is %.2fx faster per kick for the vCPU\n\n", t_exp2 / t_exp1);

  printf("EXPERIMENT 3: datamatch on one address (guest wrote 0,1,2,3 to C)\n");
  printf("   eventfd(value==1) counter : %lu\n", (unsigned long)c1);
  printf("   eventfd(value==2) counter : %lu\n", (unsigned long)c2);
  printf("   fell through to userspace as KVM_EXIT_MMIO, values:");
  for (int i = 0; i < nunm; i++)
    printf(" %u", unm_vals[i]);
  printf("   (expect 0 and 3)\n\n");

  /* Sanity check on the total: we expect exactly 5 port-I/O markers
   * (BEGIN, END, BEGIN, END, DONE) + 200000 unavoidable exits from B's
   * writes + 2 unmatched writes from C + 0 exits from A's 200000 writes. */
  printf("KVM_RUN returned to userspace %lu times in total = %d (B) + 2 (unmatched C) + 5 port-I/O markers\n"
         "                                            + 0 from the %d writes to A.\n",
         kvm_run_calls, N_WRITES, N_WRITES);

  /* Exit code 0 only if every expectation above actually held -- lets a
   * script (e.g. `make run`) detect a regression without parsing output. */
  return (a == N_WRITES && mmio_exits_exp2 == 0 && c1 == 1 && c2 == 1) ? 0 : 2;
}
