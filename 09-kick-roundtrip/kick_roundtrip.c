/*
 * kick_roundtrip.c -- a virtual device's full request/response loop on real
 * KVM, done two ways and raced against each other.
 *
 * This file is the HOST side: a tiny VMM (virtual machine monitor, the
 * program that creates and runs the VM, like QEMU or Firecracker).  The
 * program running inside the VM is guest.c.  shared.h holds what the two
 * agree on.
 *
 * THE STORY
 * ---------
 * A virtual machine's disk or network card is software on the host.  Every
 * request the guest makes goes through the same loop:
 *
 *   1. the guest "kicks" the device: it writes to a special address (the
 *      doorbell) to say "I've got work for you",
 *   2. the device does the work (reads the disk, sends the packet...),
 *   3. the device tells the guest "done" by raising an interrupt.
 *
 * How fast that loop can go decides how fast the VM's I/O is.  This program
 * builds the loop both ways KVM allows and times them:
 *
 *   CLASSIC   kick --(VM exit)--> the vCPU thread does the work itself
 *             --(KVM_IRQ_LINE ioctl)--> interrupt
 *
 *             The kick stops the guest's CPU and returns from KVM_RUN to our
 *             code.  The same thread that runs the guest then runs the
 *             device, and only goes back into the guest when it's finished.
 *             While the device works, the guest is frozen.
 *
 *   FAST      kick --(ioeventfd)--> a separate device thread does the work
 *             --(irqfd)--> interrupt
 *
 *             KVM turns the kick into a wake-up for a separate device thread,
 *             and the guest carries on immediately.  The device thread raises
 *             the interrupt with a plain write() to an eventfd.  The vCPU
 *             thread never leaves the kernel for a kick.  This is how real
 *             virtio devices are wired in QEMU and Firecracker.
 *
 * Each building block has its own lab:
 *   KVM_IOEVENTFD  (07-ioeventfd)  a guest write to the doorbell bumps an
 *                  eventfd inside KVM; the guest doesn't stop.
 *   KVM_IRQFD      (08-irqfd)      write() to an eventfd raises an interrupt
 *                  in the guest.
 *   KVM_IRQ_LINE   (08-irqfd)      the older ioctl way to raise an interrupt.
 *
 * An eventfd is Linux's simplest file descriptor: a counter.  write() adds
 * to it, read() returns the total and resets it to 0, and a read() on a
 * counter of 0 waits until someone writes.
 *
 * The device's "work" is simulated: it keeps a CPU busy for W microseconds
 * per request, like a device that takes W us to handle each one.
 *
 * TWO WORKLOADS
 * -------------
 *  1. PIPELINED   the guest sends all N requests without waiting, then waits
 *                 for them all.  Like a database issuing lots of async I/O.
 *  2. PING-PONG   the guest sends one request, waits for it to finish, then
 *                 sends the next.  Like a program doing one blocking read at a
 *                 time.  Run with W = 0, so it measures only the cost of the
 *                 loop itself.
 *
 * WHAT TO EXPECT, INCLUDING WHERE THE FAST PATH LOSES
 * ---------------------------------------------------
 *  * PIPELINED: the fast path lets the guest finish *sending* all its
 *    requests roughly 10x sooner, because its CPU never runs device code.
 *    The *total* time is about the same both ways (about N x W): there's
 *    still only one device doing the work.  The win is that the guest's CPU
 *    is free to do other things meanwhile.
 *  * PING-PONG with W = 0: the classic path is FASTER.  When there's no work
 *    to do, it just runs a function call in the same thread.  The fast path
 *    hands off between threads twice (wake the device thread, then wake the
 *    guest's CPU), and each hand-off costs scheduler time.  ioeventfd/irqfd
 *    are about throughput and keeping the guest's CPU free (and they let the
 *    device live in the kernel, as vhost does), not about making one request
 *    faster.
 *
 * BUILD & RUN:  make && ./kick_roundtrip [N] [W_us]     (default N=20000 W=10)
 */

#define _GNU_SOURCE
#include "shared.h" /* addresses, ports, struct shared: agreed with guest.c */
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The guest program: the raw machine code of guest.c, as built by the
 * Makefile.  #embed (C23) pastes the file's bytes in here as if we'd typed
 * them out: {0x55, 0xb8, ...}.  Each VM gets a copy at GUEST_LOAD_GPA. */
static const unsigned char guest_bin[] = {
#embed "guest.bin"
};

/* Which way the device is wired (see THE STORY above). */
enum device_path {
  CLASSIC, /* kick = VM exit; the vCPU thread does the work; KVM_IRQ_LINE */
  FAST,    /* kick = ioeventfd; a device thread does the work; irqfd      */
};

/* Print what failed (with the errno text) and quit. */
static void die(const char *w) {
  perror(w);
  exit(1);
}

/* The current time in nanoseconds, from a clock that only moves forward. */
static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Pretend to be a device handling one request: keep this CPU busy for `us`
 * microseconds.  (PAUSE is a CPU hint meaning "I'm just spinning".) */
static void do_device_work(unsigned us) {
  uint64_t end = now_ns() + us * 1000ull;
  while (now_ns() < end) {
    __builtin_ia32_pause();
  }
}

/* Everything one VM needs.  Each run_config() call builds a fresh one. */
struct vmctx {
  int vm_fd, vcpu_fd;         /* KVM file descriptors for the VM and its one vCPU      */
  int kickfd;                 /* FAST only: eventfd KVM bumps on every kick            */
  int irqfd;                  /* FAST only: eventfd whose write() interrupts the guest */
  uint8_t *guest_mem;         /* host pointer to guest RAM: mem[x] is guest address x  */
  struct kvm_run *run;        /* page KVM shares with us: after KVM_RUN, says why      */
  volatile struct shared *sh; /* the mailbox in guest RAM (shared.h)            */
  unsigned work_us;           /* W: simulated work per request                         */
  atomic_int stop;            /* tells the device thread to exit                       */
  uint32_t completed;         /* the device's own count of finished requests           */
};

/* The device finished one request: tell the guest by updating `completed`
 * in shared memory.  Called by whichever thread plays the device (the device
 * thread on the FAST path, the vCPU thread on the CLASSIC path), never both,
 * so there's only ever one writer.
 *
 * This is the lab's version of a virtio "used ring": the device publishes
 * the result in memory FIRST, and only then raises the interrupt.  The
 * RELEASE ordering guarantees that anyone who sees the interrupt's effects
 * also sees this new value, never a stale one.  (On x86 plain stores already
 * behave this way, but the release makes the rule explicit and portable.) */
static void complete_one(struct vmctx *c) {
  __atomic_store_n(&c->sh->completed, ++c->completed, __ATOMIC_RELEASE);
  if (c->completed % 2000 == 0) {
    // printf("completed %u requests\n", c->completed);
  }
}

/* ---------------------------------------------------------- the FAST path
 * The device thread.  It sleeps in read() on the kick eventfd until the guest
 * kicks, does the work, publishes the completion, and raises the interrupt
 * with a write() to the irqfd.  The vCPU thread is never involved. */
static void *device_thread(void *arg) {
  struct vmctx *c = arg;
  for (;;) {
    /* Wait for kicks.  read() returns how many kicks arrived since the last
     * read, and resets the count to 0.  If the guest kicked 5 times while we
     * were busy, we get 5 here, in one wake-up. */
    uint64_t kicks;
    if (read(c->kickfd, &kicks, sizeof kicks) != sizeof kicks)
      break;
    if (atomic_load(&c->stop))
      break;
    /* The eventfd only COUNTS kicks: the number the guest wrote to the
     * doorbell (its request id) never reaches us.  That's fine: a real virtio
     * device finds out what to do by reading the request queue in guest
     * memory, not from the doorbell value. */
    while (kicks--) {
      do_device_work(c->work_us);
      complete_one(c);
      uint64_t one = 1; /* raise the interrupt: just a write() */
      if (write(c->irqfd, &one, sizeof one) != sizeof one)
        break;
    }
  }
  return NULL;
}

/* Put a fresh vCPU straight into 32-bit mode with flat memory, so the guest
 * can be ordinary C compiled with gcc -m32.  (A real PC starts in 16-bit
 * mode and its boot code switches; a VMM can simply set the registers to the
 * "after" state.  08-irqfd/irqfd_demo.c explains each field.) */
static void enter_protected_mode(struct vmctx *c) {
  /* The GDT: a small table in guest memory describing the memory segments.
   * The CPU re-reads it when it enters and leaves interrupt handlers. */
  uint64_t *gdt = (uint64_t *)(c->guest_mem + GDT_GPA);
  gdt[0] = 0;                  /* entry 0: must be empty           */
  gdt[1] = 0x00cf9b000000ffff; /* selector 0x08: flat 32-bit code  */
  gdt[2] = 0x00cf93000000ffff; /* selector 0x10: flat 32-bit data  */

  struct kvm_sregs sr;
  if (ioctl(c->vcpu_fd, KVM_GET_SREGS, &sr) < 0)
    die("KVM_GET_SREGS");
  sr.gdt.base = GDT_GPA;
  sr.gdt.limit = 3 * 8 - 1;
  struct kvm_segment code = {.base = 0, .limit = 0xffffffff, .selector = SEL_CODE, .type = 11, .present = 1, .dpl = 0, .db = 1, .s = 1, .g = 1};
  struct kvm_segment data = code;
  data.selector = SEL_DATA;
  data.type = 3;
  sr.cs = code;
  sr.ds = sr.es = sr.fs = sr.gs = sr.ss = data;
  sr.cr0 |= 1; /* protected mode on (paging stays off) */
  if (ioctl(c->vcpu_fd, KVM_SET_SREGS, &sr) < 0)
    die("KVM_SET_SREGS");

  /* Start at the guest's _start, with a stack, and interrupts off
   * (rflags = 2: only the always-1 reserved bit set). */
  struct kvm_regs regs = {.rip = GUEST_LOAD_GPA, .rsp = GUEST_STACK_TOP, .rflags = 2};
  if (ioctl(c->vcpu_fd, KVM_SET_REGS, &regs) < 0)
    die("KVM_SET_REGS");
}

/* What one run measured.
 *   submit_ms   from the guest's first kick until it had sent every request
 *   total_ms    from the first kick until every request was completed
 *   mmio_exits  kicks that stopped the guest and came back to our code
 *   total_exits every return from KVM_RUN (markers included)
 *   guest_irqs  interrupts the guest's handler actually ran */
struct result {
  double submit_ms, total_ms;
  unsigned long mmio_exits, total_exits, guest_irqs;
};

/* Build a FRESH VM, run the guest once, and measure.
 *   path:  CLASSIC or FAST (see enum path)
 *   mode:  PIPELINED or PINGPONG (see enum mode in shared.h)
 *   n:     number of requests; work_us: W */
static struct result run_config(int kvm, enum device_path path, enum mode mode, uint32_t n, unsigned work_us) {
  struct vmctx c = {.work_us = work_us};
  c.vm_fd = ioctl(kvm, KVM_CREATE_VM, 0);
  if (c.vm_fd < 0)
    die("KVM_CREATE_VM");

  /* In-kernel interrupt controller: both paths need it (the guest programs
   * it, and both KVM_IRQ_LINE and irqfd raise lines on it).  Must come before
   * the vCPU is created. */
  if (ioctl(c.vm_fd, KVM_CREATE_IRQCHIP, 0) < 0)
    die("KVM_CREATE_IRQCHIP");

  /* 64 KiB of guest RAM (plain memory in our process, zero-filled), the
   * guest program, and the parameters the guest reads when it starts. */
  c.guest_mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (c.guest_mem == MAP_FAILED)
    die("mmap guest RAM");

  struct kvm_userspace_memory_region rg = {.slot = 0, .memory_size = GUEST_MEM_SIZE, .userspace_addr = (uintptr_t)c.guest_mem};
  if (ioctl(c.vm_fd, KVM_SET_USER_MEMORY_REGION, &rg) < 0)
    die("memslot");

  memcpy(c.guest_mem + GUEST_LOAD_GPA, guest_bin, sizeof guest_bin);

  c.sh = (volatile struct shared *)(c.guest_mem + SHARED_GPA);
  c.sh->mode = mode;
  c.sh->n = n;

  /* FAST path wiring: two eventfds and one device thread.
   *   KVM_IOEVENTFD: "when the guest writes 4 bytes to MMIO_DOORBELL, bump
   *                   kickfd and let the guest continue" (KVM_RUN doesn't
   *                   return to us),
   *   KVM_IRQFD:     "when irqfd is written, raise line IRQ_GSI in the guest".
   * On the CLASSIC path we register neither, so a doorbell write has nowhere
   * to go inside KVM and comes back to us as KVM_EXIT_MMIO. */
  pthread_t dev;
  if (path == FAST) {
    c.kickfd = eventfd(0, 0); /* blocking: the device thread sleeps in read() */
    c.irqfd = eventfd(0, 0);

    struct kvm_ioeventfd io = {.addr = MMIO_DOORBELL, .len = 4, .fd = c.kickfd};
    if (ioctl(c.vm_fd, KVM_IOEVENTFD, &io) < 0)
      die("KVM_IOEVENTFD");

    struct kvm_irqfd ifd = {.fd = c.irqfd, .gsi = IRQ_GSI};
    if (ioctl(c.vm_fd, KVM_IRQFD, &ifd) < 0)
      die("KVM_IRQFD");

    pthread_create(&dev, NULL, device_thread, &c);
  }

  /* The vCPU, and the kvm_run page where KVM tells us why it stopped. */
  c.vcpu_fd = ioctl(c.vm_fd, KVM_CREATE_VCPU, 0);
  if (c.vcpu_fd < 0)
    die("KVM_CREATE_VCPU");

  long sz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  c.run = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, c.vcpu_fd, 0);
  if (c.run == MAP_FAILED)
    die("mmap kvm_run");

  enter_protected_mode(&c);

  /* Run the guest on THIS thread.  KVM_RUN runs the guest until it does
   * something KVM can't finish by itself; then it returns here with the
   * reason in c.run->exit_reason, we handle it, and loop to resume the guest. */
  struct result r = {0};
  uint64_t t_start = 0, t_sub = 0, t_done = 0;
  for (int running = 1; running;) {
    if (ioctl(c.vcpu_fd, KVM_RUN, 0) < 0) {
      if (errno == EINTR) /* a signal interrupted it: just resume */
        continue;
      die("KVM_RUN");
    }
    r.total_exits++;
    switch (c.run->exit_reason) {
    case KVM_EXIT_IO:
      /* The guest wrote to one of its marker ports: record the time. */
      switch (c.run->io.port) {
      case PORT_START:
        t_start = now_ns();
        break;
      case PORT_SUBMITTED:
        t_sub = now_ns();
        break;
      case PORT_ALLDONE:
        t_done = now_ns();
        break;
      case PORT_EXIT:
        running = 0;
        break;
      }
      break;

    case KVM_EXIT_MMIO:
      /* ------------------------------------------------ the CLASSIC path
       * The guest wrote to the doorbell and nothing inside KVM took it, so
       * the guest is stopped and we're the device.  (On the FAST path, the
       * ioeventfd takes every doorbell write, so this never happens.) */
      r.mmio_exits++;
      if (c.run->mmio.phys_addr == MMIO_DOORBELL && c.run->mmio.is_write) {
        do_device_work(work_us); /* the guest stays frozen during this */
        complete_one(&c);
        /* Raise the interrupt the classic way: set the line high, then low
         * again (the interrupt controller reacts to the rising edge).  The
         * interrupt is now pending, and the guest takes it as soon as our
         * next KVM_RUN resumes it. */
        struct kvm_irq_level l = {.irq = IRQ_GSI, .level = 1};
        ioctl(c.vm_fd, KVM_IRQ_LINE, &l);
        l.level = 0;
        ioctl(c.vm_fd, KVM_IRQ_LINE, &l);
      }
      break;

    default:
      fprintf(stderr, "unexpected exit %u\n", c.run->exit_reason);
      exit(1);
    }
  }

  r.submit_ms = (t_sub - t_start) / 1e6;
  r.total_ms = (t_done - t_start) / 1e6;
  r.guest_irqs = c.sh->irq_count;

  if (path == FAST) {
    /* Stop the device thread: set the flag, then wake it from its read(). */
    atomic_store(&c.stop, 1);
    uint64_t one = 1;
    if (write(c.kickfd, &one, sizeof one) < 0) {
      /* nothing useful to do on failure */
    }
    pthread_join(dev, NULL);
    close(c.kickfd);
    close(c.irqfd);
  }
  /* Tear down.  (The kvm_run mapping is left in place.  It keeps the vCPU,
   * and so the VM, alive until the process exits, which is harmless for four
   * short runs.) */
  close(c.vcpu_fd);
  close(c.vm_fd);
  munmap(c.guest_mem, GUEST_MEM_SIZE);
  return r;
}

/* Print one result line.  us/req = total time divided by the number of
 * requests. */
static void row(const char *name, struct result r, uint32_t n) {
  printf("  %-34s submit %8.1f ms | all done %8.1f ms (%6.2f us/req) | KVM_EXIT_MMIO %7lu | guest irqs %6lu\n", name, r.submit_ms, r.total_ms, r.total_ms * 1e3 / n, r.mmio_exits, r.guest_irqs);
}

int main(int argc, char **argv) {
  alarm(120); /* watchdog: kill the process if anything hangs */
  uint32_t n = argc > 1 ? strtoul(argv[1], NULL, 10) : 20000;
  unsigned w = argc > 2 ? strtoul(argv[2], NULL, 10) : 10;

  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0)
    die("open /dev/kvm");

  printf("N=%u requests, device work W=%u us per request\n\n", n, w);

  printf("1) PIPELINED (deep queue): 'submit' = time the guest vCPU spent issuing kicks\n");
  row("classic: exit + KVM_IRQ_LINE", run_config(kvm, CLASSIC, PIPELINED, n, w), n);
  row("fast: ioeventfd + irqfd", run_config(kvm, FAST, PIPELINED, n, w), n);

  printf("\n2) PING-PONG (queue depth 1), W=0 -> pure round-trip latency (N/4 requests)\n");
  row("classic: exit + KVM_IRQ_LINE", run_config(kvm, CLASSIC, PINGPONG, n / 4, 0), n / 4);
  row("fast: ioeventfd + irqfd", run_config(kvm, FAST, PINGPONG, n / 4, 0), n / 4);

  printf("\nRead: in (1) 'submit' shows the vCPU is freed from device work; the fast path's\n"
         "KVM_EXIT_MMIO is 0 -- the vCPU thread never left the kernel for a kick.  In (2) the\n"
         "extra thread hand-offs make the fast path SLOWER per round trip: ioeventfd/irqfd buy\n"
         "throughput and CPU isolation (and vhost), not single-request latency.\n");
  return 0;
}
