/*
 * irqfd_demo.c -- KVM_IRQFD on real KVM: interrupt a virtual machine's CPU
 *                 with nothing more than write() to a file descriptor.
 *
 * If you read 07-ioeventfd first: that lab was about the guest poking the
 * host.  This one is the opposite direction: the host poking the guest.
 *
 * This file is the HOST side: a tiny VMM.  The program that runs inside the
 * VM is guest.c, and shared.h holds what the two agree on (addresses, ports,
 * the shared mailbox).
 *
 * BACKGROUND: WHAT IS AN INTERRUPT?
 * ---------------------------------
 * A CPU normally runs its instructions one after another.  An interrupt is a
 * hardware signal that says "stop what you're doing and run this other piece
 * of code first".  That code is the interrupt handler.  When the handler
 * finishes, the CPU carries on exactly where it left off.
 *
 * Devices use interrupts to get the CPU's attention.  A network card says
 * "a packet arrived", a disk says "your read finished".  Without interrupts,
 * the CPU would have to keep asking every device "anything new?", which is
 * called polling and wastes time.
 *
 * On a PC the path looks like this:
 *
 *   device --(wire "IRQ 5")--> interrupt controller --(vector 0x25)--> CPU
 *
 *   - IRQ ("interrupt request") line: a numbered input wire on the
 *     interrupt controller.  Each device is connected to one.
 *   - Interrupt controller: a chip that collects the device wires.  It decides
 *     which one to deliver first and tells the CPU.  The oldest kind is the
 *     Intel 8259 "PIC" (Programmable Interrupt Controller), with 8 inputs.
 *   - Vector: the number the CPU actually receives (0-255).  The CPU uses it
 *     as an index into a table of handler addresses.  The PIC converts
 *     "pin 5" into "vector base + 5".
 *
 * In a virtual machine, none of this hardware is real.  KVM (the Linux
 * kernel's hypervisor) emulates the interrupt controller in software.  The
 * question this lab answers is how the host software playing "the device"
 * tells KVM to raise that wire.
 *
 * THE PROBLEM
 * -----------
 * Say a virtual device has finished some I/O and must interrupt the guest.
 * The classic way is ioctl(vm_fd, KVM_IRQ_LINE, ...).  An ioctl is a
 * "do something special" syscall on a file descriptor.  The catch is that
 * you need the VM's file descriptor, which only the VMM holds.  (The VMM,
 * or virtual machine monitor, is the userspace program that created the VM;
 * QEMU is one, and this file is a tiny one.)
 *
 * Some devices are emulated inside the kernel instead, for speed.  vhost-net,
 * the kernel's virtio network backend, is one.  It has no VM fd.  Without
 * another option, every "packet arrived" would have to bounce up to the VMM
 * process, which would then do the ioctl.  That extra trip is slow.
 *
 * THE FIX: ioctl(vm, KVM_IRQFD, {fd, gsi})
 * ----------------------------------------
 * We register the fd once, and it means: "whenever this eventfd is signalled,
 * raise interrupt line `gsi` in the guest."
 *
 * An eventfd is the simplest file descriptor Linux has.  It holds a 64-bit
 * counter.  write() adds to the counter, and anyone waiting on the fd gets
 * woken up.  After KVM_IRQFD, KVM itself is one of those waiters.  So:
 *   - any userspace thread can write() to the eventfd, and
 *   - kernel code (like vhost) can call eventfd_signal() on it, which does
 *     the same thing without a syscall.
 * Either way KVM wakes up and injects the interrupt directly.  The VMM isn't
 * involved at all, and the vCPU is disturbed only by the interrupt itself.
 * The fd has become a doorbell wired straight to the guest's interrupt pin.
 *
 * GSI = "global system interrupt": KVM's own number for an interrupt input.
 * KVM_CREATE_IRQCHIP sets up a default mapping where GSIs 0-15 are wired to
 * the PIC's pins and to the IOAPIC's pins 0-15.  So GSI 5 is just IRQ 5.
 *
 * WHAT THE GUEST DOES
 * -------------------
 * KVM_CREATE_IRQCHIP gives the VM in-kernel emulations of three PC interrupt
 * controllers:
 *   - the 8259 PIC (old, simple, the one we use),
 *   - the IOAPIC (its modern replacement),
 *   - the LAPIC ("local APIC", one per CPU, which receives interrupts for
 *     that CPU).
 * irqfd needs these to exist inside the kernel: it has to inject the
 * interrupt into something.
 *
 * The guest (guest.c, a small freestanding 32-bit C program):
 *   1. prints a hello message on the serial port,
 *   2. installs its handler in the IDT, the CPU's "vector number -> handler
 *      address" table,
 *   3. programs the PIC: "your vectors start at 0x20; only let IRQ 5
 *      through".  IRQ 5 therefore arrives as vector 0x20 + 5 = 0x25,
 *   4. goes to sleep with HLT ("halt until an interrupt arrives").  Because
 *      the interrupt controller is in the kernel, KVM handles HLT in the
 *      kernel too: the vCPU thread just blocks there, and our userspace
 *      never sees it,
 *   5. each time the interrupt fires, the handler adds 1 to a counter in
 *      guest RAM and tells the PIC "done" (EOI).
 *
 * The host can read guest RAM directly, because it's ordinary memory in our
 * process.  So watching that counter tells us "the interrupt arrived", and
 * watching it costs the guest nothing.
 *
 * Why PIC interrupts reach the CPU at all: on a real PC, the 8259 PIC's
 * output wire goes into the CPU's local APIC, on an input called LINT0.
 * Firmware sets LINT0 to "ExtINT" mode, meaning "accept interrupts from the
 * old PIC".  This is known as "virtual wire" mode.  KVM puts the boot vCPU
 * into the same state at reset.  The IOAPIC also has a pin 5, but all its
 * pins start out masked (switched off), so only the PIC route delivers here.
 *
 * EXPERIMENTS
 * -----------
 *  A. PACED: the host does one write(irqfd), then waits until the guest's
 *     counter goes up, N times.  This measures how long an interrupt takes to
 *     get from host to guest (the latency).
 *  B. BURST: write(irqfd) 10 000 times as fast as possible.  The guest
 *     handler runs FAR fewer than 10 000 times.  This is coalescing:
 *       - irqfd is edge-triggered.  It means "the wire went up", not "the
 *         wire is up".  The PIC remembers that as ONE bit per pin: "pin 5
 *         wants service".  Setting a bit that's already set changes nothing,
 *         so while one interrupt is pending, more signals merge into it.
 *       - KVM also merges them earlier.  For a PIC/IOAPIC GSI, the eventfd
 *         wake-up only *schedules* the injection as deferred kernel work.
 *         Signals that arrive before that work runs are absorbed into it.
 *     Real devices are built around this.  The device puts its results in a
 *     shared list (a "ring"), and the interrupt only means "go look at the
 *     ring".  It never means "exactly one new event".
 *  C. Same as A, but the host uses the classic KVM_IRQ_LINE ioctl instead.
 *
 *  Finally we report how many times KVM_RUN returned to userspace during all
 *  of that.  The answer is exactly ONE: the guest's final "I'm done" signal.
 *  Everything else happened inside the kernel.  (Before any of this, the
 *  guest prints a hello message on the serial port.  That costs one exit per
 *  character and is counted separately.  It's a handy contrast: printing a
 *  short string costs dozens of exits, while tens of thousands of interrupts
 *  cost none.)
 *
 * Run order in main() is A, C, B.  B leaves interrupts still in flight, which
 * would confuse a paced measurement, so it goes last.
 *
 * BUILD & RUN:   make && ./irqfd_demo         (needs /dev/kvm access)
 */

#define _GNU_SOURCE
#include "shared.h" /* addresses, ports, struct shared: agreed with guest.c */
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h> /* KVM's ioctl numbers and structs            */
#include <pthread.h>   /* threads: the vCPU gets its own             */
#include <stdatomic.h> /* atomic counter shared between threads      */
#include <stdint.h>    /* fixed-size integers: uint8_t, uint64_t ... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h> /* eventfd(): the fd we turn into a doorbell */
#include <sys/ioctl.h>
#include <sys/mman.h> /* mmap(): to allocate guest RAM */
#include <time.h>
#include <unistd.h>

/* The guest program: the raw machine code of guest.c, as built by the
 * Makefile (guest.c -> guest.elf -> guest.bin).  #embed (C23) pastes the
 * file's bytes in here as if we'd typed them out: {0x55, 0xb9, ...}.
 * main() copies them into guest RAM at GUEST_LOAD_GPA. */
static const unsigned char guest_bin[] = {
#embed "guest.bin"
};

/* ---------------------------------------------------------------- host ----
 * Everything below is ordinary Linux C, running on the host. */

/* Print what failed (perror adds the errno text, e.g. "Permission denied")
 * and quit.  Keeps the error checks below short. */
static void die(const char *w) {
  perror(w);
  exit(1);
}

/* The current time in nanoseconds, from the monotonic clock.  "Monotonic"
 * means it only ever goes forward: it isn't changed when someone sets the
 * system clock.  That's what you want for measuring durations. */
static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* State shared between main() and the vCPU thread.  They're globals so both
 * threads can reach them. */
static uint8_t *guest_mem;  /* host pointer to guest RAM: mem[x] is GPA x         */
static int vcpu_fd;         /* fd of the virtual CPU                              */
static struct kvm_run *run; /* page KVM shares with us: after KVM_RUN, says why   */

/* The mailbox in guest RAM (layout in shared.h), seen from the host side:
 * it points at mem + SHARED_GPA, the same bytes the guest sees at SHARED_GPA.
 *
 * `volatile` is essential here.  Normally the compiler assumes memory only
 * changes when *this* code changes it.  In a loop like
 *     while (sh->irq_count == before) ;
 * it could read irq_count once, decide the loop never ends, and never look
 * again.  But the guest changes this memory behind the compiler's back.
 * volatile says "really read it from memory, every single time". */
static volatile struct shared *sh;

/* How often KVM_RUN returned to us.  Written by the vCPU thread and read by
 * main(), so they're atomic: that makes those accesses safe across threads.
 * The serial-port bytes of the hello message are counted separately, so
 * userspace_exits still measures only the interrupt experiments. */
static atomic_ulong userspace_exits;
static atomic_ulong serial_exits;

/* The vCPU thread: KVM_RUN in a loop.  Notice how little it does.
 *
 * ioctl(vcpu_fd, KVM_RUN) means "run the guest now".  The calling thread
 * turns into the virtual CPU and doesn't come back until the guest does
 * something KVM can't handle by itself.  That return is an "exit to
 * userspace".  Here that happens once per serial byte, then once at the end.
 *
 * Because KVM_RUN blocks for as long as the guest runs, it needs its own
 * thread.  Otherwise main() couldn't inject any interrupts: it would be stuck
 * inside KVM_RUN.  That's also the point of the demo.  The thread running the
 * guest and the thread raising interrupts are different, and they never have
 * to coordinate. */
static void *vcpu_thread(void *arg) {
  (void)arg; /* unused; this silences the compiler warning */
  for (;;) {
    if (ioctl(vcpu_fd, KVM_RUN, 0) < 0) {
      /* EINTR = "a signal interrupted the syscall": harmless, run again. */
      if (errno == EINTR)
        continue;
      die("KVM_RUN");
    }
    /* A byte for the serial port: print it and go straight back into the
     * guest.  For an OUT, KVM has copied the data into the kvm_run page at
     * byte offset io.data_offset: `count` items of `size` bytes each (here
     * one item of one byte: a single character). */
    if (run->exit_reason == KVM_EXIT_IO && run->io.port == PORT_SERIAL && run->io.direction == KVM_EXIT_IO_OUT) {
      atomic_fetch_add(&serial_exits, 1);
      fwrite((uint8_t *)run + run->io.data_offset, run->io.size, run->io.count, stdout);
      continue;
    }
    atomic_fetch_add(&userspace_exits, 1);
    /* The expected exit: the guest's final write to PORT_DONE. */
    if (run->exit_reason == KVM_EXIT_IO && run->io.port == PORT_DONE)
      return NULL;
    /* Anything else is a bug.  Report it and stop.  (main() may then wait
     * for a counter that never moves; the alarm(60) watchdog ends that.) */
    fprintf(stderr, "unexpected exit %u\n", run->exit_reason);
    return NULL;
  }
}

/* Comparison function for qsort(): sort uint64_t values from small to large.
 * qsort wants negative / 0 / positive for "a<b" / "a==b" / "a>b".
 * (We don't use `x - y`: with unsigned 64-bit numbers it would wrap around,
 * and it doesn't fit in an int anyway.) */
static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

/* Send n interrupts one at a time using `inject()`, waiting for each to be
 * handled before sending the next, then print latency statistics.
 *
 * `inject` is a function pointer, so the same measuring code can test both
 * injection methods (irqfd and KVM_IRQ_LINE).  `ctx` is passed through to it:
 * a pointer to whichever fd that method needs.
 *
 * Each sample times the whole round trip:
 *   our inject call -> KVM raises the interrupt -> the sleeping (halted) vCPU
 *   wakes -> the guest's handler runs and bumps the counter -> this thread
 *   sees the new value.
 * (KVM has a feature called "halt polling": a halted vCPU may spin for a
 * short while before really going to sleep.  An interrupt that arrives
 * during that spin is picked up faster, which lowers these numbers.) */
static void paced(const char *name, int n, void (*inject)(void *), void *ctx) {
  uint64_t *lat = malloc(n * sizeof *lat); /* one latency per interrupt, in ns */
  for (int i = 0; i < n; i++) {
    uint32_t before = sh->irq_count;
    uint64_t t0 = now_ns();
    inject(ctx);
    /* Spin (busy-wait) until the guest's handler has run, i.e. the counter
     * in guest RAM has moved.  We spin instead of sleeping because sleeping
     * and waking would add more delay than we're trying to measure.
     * __builtin_ia32_pause = the x86 PAUSE instruction: a hint to the CPU
     * that "this is a spin loop".  It saves power and plays nicer with the
     * other hyperthread on the same core. */
    while (sh->irq_count == before)
      __builtin_ia32_pause();
    lat[i] = now_ns() - t0;
  }
  /* Sort, then read percentiles straight off the sorted array:
   *   min = the fastest (lat[0]),
   *   p50 = the median: half the samples were faster (lat[n/2]),
   *   p99 = 99% were faster than this (lat[n*99/100]).  It shows the
   *         occasional slow case that an average would hide.
   * The values are in ns; /1e3 prints microseconds (us). */
  qsort(lat, n, sizeof *lat, cmp_u64);
  printf("   %-34s n=%d  min %5.1f us   p50 %5.1f us   p99 %6.1f us\n", name, n, lat[0] / 1e3, lat[n / 2] / 1e3, lat[n * 99 / 100] / 1e3);
  free(lat);
}

/* Injection method 1: signal the irqfd.
 * Writing to an eventfd must be exactly 8 bytes (one uint64_t), which is
 * added to its counter.  The value doesn't matter to KVM: any write wakes it,
 * and it raises GSI 5.  Any thread in any process holding this fd could do
 * this, as could kernel code, with no syscall at all. */
static void inject_irqfd(void *ctx) {
  uint64_t one = 1;
  if (write(*(int *)ctx, &one, sizeof one) != sizeof one)
    die("write irqfd");
}

/* Injection method 2: the classic ioctl on the VM fd.
 * KVM_IRQ_LINE sets the *level* of an interrupt wire: 1 = high, 0 = low.
 * The PIC was programmed edge-triggered, so what it reacts to is the wire
 * going from low to high.  To make one interrupt we must pulse the wire:
 * raise it, then lower it again, ready for the next rising edge.  That's two
 * ioctls (two syscalls) per interrupt.  (irqfd does the same pulse for us
 * inside the kernel.) */
static void inject_irqline(void *ctx) {
  int vm = *(int *)ctx;
  struct kvm_irq_level l = {.irq = IRQ_GSI, .level = 1};
  if (ioctl(vm, KVM_IRQ_LINE, &l) < 0) /* wire high */
    die("KVM_IRQ_LINE");
  l.level = 0;
  if (ioctl(vm, KVM_IRQ_LINE, &l) < 0) /* wire low */
    die("KVM_IRQ_LINE");
}

/* Put the vCPU straight into 32-bit protected mode, so the guest can be
 * ordinary C compiled with gcc -m32.
 *
 * A real x86 CPU powers on in 16-bit "real mode" (the 1981 IBM PC's mode),
 * and an OS's boot code switches to 32-bit mode itself, in assembly.  A VMM
 * can skip that: it can set the vCPU's registers to what they'd be *after*
 * the switch.
 *
 * Two things describe the mode:
 *   - CR0 bit 0, "PE" (Protection Enable): 1 = protected mode.
 *   - The segment registers (CS for code; DS, SS, ... for data).  Each one
 *     says where its segment starts, how big it is, and what it's for.  We
 *     make all of them "flat": start at 0, 4 GiB long.  Then segments have
 *     no visible effect and an address is just an address, which is what
 *     C expects.
 *
 * The CPU reads segment descriptions from the GDT (Global Descriptor
 * Table), an array in guest memory, whenever a segment register is loaded,
 * e.g. when entering an interrupt handler or returning with iret.  So we
 * write a 3-entry GDT into guest RAM too, matching the registers exactly.
 *
 * (The magic GDT numbers pack the same fields as the kvm_segment structs
 * below: base 0, limit 0xfffff in 4 KiB units = 4 GiB, "present, kernel
 * privilege, code-or-data", and "32-bit".  0x9b = code, 0x93 = data.) */
static void enter_protected_mode(void) {
  uint64_t *gdt = (uint64_t *)(guest_mem + GDT_GPA);
  gdt[0] = 0;                  /* entry 0: must be empty (the "null descriptor") */
  gdt[1] = 0x00cf9b000000ffff; /* entry 1, selector 0x08: flat 32-bit code        */
  gdt[2] = 0x00cf93000000ffff; /* entry 2, selector 0x10: flat 32-bit data        */

  /* "sregs" = segment and system registers.  Read them, change what we
   * need, write them back. */
  struct kvm_sregs sr;
  if (ioctl(vcpu_fd, KVM_GET_SREGS, &sr) < 0)
    die("KVM_GET_SREGS");
  sr.gdt.base = GDT_GPA;
  sr.gdt.limit = 3 * 8 - 1; /* size in bytes, minus 1 */

  struct kvm_segment code = {
      .base = 0,
      .limit = 0xffffffff, /* 4 GiB                                       */
      .selector = SEL_CODE,
      .type = 11, /* code: execute + read (+ "accessed")          */
      .present = 1,
      .dpl = 0, /* privilege level 0: kernel                    */
      .db = 1,  /* 32-bit (not 16-bit) code                     */
      .s = 1,   /* code/data segment, not a system one          */
      .g = 1,   /* limit counts 4 KiB pages                     */
  };
  struct kvm_segment data = code;
  data.selector = SEL_DATA;
  data.type = 3; /* data: read + write (+ "accessed") */

  sr.cs = code;
  sr.ds = sr.es = sr.fs = sr.gs = sr.ss = data;
  sr.cr0 |= 1; /* PE: protected mode on.  Paging (CR0.PG) stays off. */
  if (ioctl(vcpu_fd, KVM_SET_SREGS, &sr) < 0)
    die("KVM_SET_SREGS");

  /* General registers:
   *   rip    = where to start: _start, at the beginning of guest.bin
   *   rsp    = stack pointer.  C needs a stack from its first instruction;
   *            it grows down from here
   *   rflags = 2: bit 1 of the flags register is reserved and must always
   *            be 1.  All other flags are 0, including IF (the interrupt
   *            flag), so interrupts start off.  The guest turns them on
   *            when it's ready. */
  struct kvm_regs regs = {.rip = GUEST_LOAD_GPA, .rsp = GUEST_STACK_TOP, .rflags = 2};
  if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0)
    die("KVM_SET_REGS");
}

int main(void) {
  /* Watchdog: if we're still running after 60 s, the kernel sends us
   * SIGALRM, whose default action kills the process.  That turns any hang
   * (e.g. waiting for an interrupt that never arrives) into a clean exit. */
  alarm(60);

  /* Creating a VM takes three kinds of file descriptors:
   *   /dev/kvm  -> the KVM system itself    (create VMs, query sizes)
   *   vm fd     -> one virtual machine      (memory, interrupts, irqfd)
   *   vcpu fd   -> one virtual CPU in it    (registers, KVM_RUN)
   * Every operation is an ioctl on the right fd. */
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0)
    die("open /dev/kvm");
  int vm = ioctl(kvm, KVM_CREATE_VM, 0);
  if (vm < 0)
    die("KVM_CREATE_VM");

  /* Create the in-kernel interrupt controllers (PIC + IOAPIC + a LAPIC per
   * vCPU).  This MUST come before KVM_CREATE_VCPU: each vCPU gets its LAPIC
   * when it's created.  It's also what makes irqfd possible, since there
   * has to be an in-kernel controller for KVM to inject into.  Without it,
   * the VMM would have to emulate the PIC itself in userspace. */
  if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) < 0)
    die("KVM_CREATE_IRQCHIP");

  /* Give the guest 64 KiB of RAM starting at GPA 0 ("GPA" = guest physical
   * address: an address as the guest sees it).
   * Guest RAM is ordinary memory in our own process.  mmap(MAP_ANONYMOUS)
   * hands us a fresh zero-filled chunk.  KVM_SET_USER_MEMORY_REGION then
   * tells KVM "guest addresses 0..0xffff are backed by this host memory".
   * (A "memslot" is one such mapping; we need only slot 0.)
   * From now on, mem[x] in our code and address x in the guest are the
   * same byte.  That's how the host and guest share `struct shared`. */
  guest_mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (guest_mem == MAP_FAILED)
    die("mmap guest RAM");
  struct kvm_userspace_memory_region rg = {.slot = 0, .memory_size = GUEST_MEM_SIZE, .userspace_addr = (uintptr_t)guest_mem};
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &rg) < 0)
    die("memslot");
  /* Copy the guest program to where guest.ld linked it to run. */
  memcpy(guest_mem + GUEST_LOAD_GPA, guest_bin, sizeof guest_bin);
  sh = (volatile struct shared *)(guest_mem + SHARED_GPA);

  /* ---------------------------------------------------- register the irqfd
   * The heart of the lab, and it's only three lines:
   *   1. make an eventfd (counter starts at 0).  EFD_NONBLOCK means
   *      read/write never wait.  We only write, and a write only waits when
   *      the counter is about to overflow, which can't happen here,
   *   2. tell KVM "when this fd is signalled, raise GSI 5 in this VM",
   *   3. done.  From here on, write(irqfd) is an interrupt. */
  int irqfd = eventfd(0, EFD_NONBLOCK);
  struct kvm_irqfd ifd = {.fd = irqfd, .gsi = IRQ_GSI};
  if (ioctl(vm, KVM_IRQFD, &ifd) < 0)
    die("KVM_IRQFD");
  printf("KVM_IRQFD: eventfd %d -> GSI %d  (guest vector 0x%x)\n\n", irqfd, IRQ_GSI, IRQ_VECTOR);

  /* Create the vCPU.  0 = vCPU id: the first (boot) CPU. */
  vcpu_fd = ioctl(vm, KVM_CREATE_VCPU, 0);
  if (vcpu_fd < 0)
    die("KVM_CREATE_VCPU");
  /* Map the vCPU's shared `struct kvm_run` page into our memory.  After
   * each KVM_RUN, we read the exit reason from it. */
  long sz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  run = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
  if (run == MAP_FAILED)
    die("mmap kvm_run");
  enter_protected_mode();

  /* Start the guest on its own thread (see vcpu_thread for why). */
  pthread_t th;
  pthread_create(&th, NULL, vcpu_thread, NULL);

  /* Wait until the guest says it's ready (installed its handler, programmed
   * the PIC).  We just poll the flag in guest RAM every 100 us.  Again, no
   * exits are involved.  If we injected too early, the PIC would still be
   * masked and the interrupt lost.  Give up after 5 s: something must be
   * broken. */
  uint64_t deadline = now_ns() + 5000000000ull; /* 5 s */
  while (!sh->ready) {
    if (now_ns() > deadline) {
      fprintf(stderr, "guest never became ready\n");
      return 1;
    }
    usleep(100);
  }
  printf("guest is up: PIC programmed, handler installed, sleeping in HLT\n\n");

  /* ------------------------------------------------------------- A: paced
   * 20_000 interrupts via irqfd, one at a time, timing each one. */
  printf("A. PACED delivery latency, host write() -> guest handler has run\n");
  paced("irqfd: write(eventfd)", 20000, inject_irqfd, &irqfd);

  /* ------------------------------------------------------------ C: ioctl
   * The same test through the old KVM_IRQ_LINE ioctl.  Expect numbers close
   * to A's: from a userspace thread, both are just syscalls.  Speed isn't
   * the point of irqfd, as the printed note explains. */
  printf("\nC. same, via the classic ioctl(KVM_IRQ_LINE) path\n");
  paced("KVM_IRQ_LINE: 2 ioctls", 20000, inject_irqline, &vm);
  printf("   (Both are a syscall from a non-vCPU thread. irqfd's real win is that the *caller\n"
         "    needn't be the VMM*: vhost signals it from the kernel with no syscall at all.)\n");

  /* -------------------------------------------------------------- B: burst
   * Fire 10 000 signals as fast as we can, without waiting for the guest in
   * between.  Then give the guest 100 ms to finish handling whatever reached
   * it, and count how many handler runs actually happened.  The difference
   * is the number of signals that merged ("coalesced") into an interrupt
   * that was already pending.  See EXPERIMENTS B at the top. */
  printf("\nB. BURST: 10_000 back-to-back write(irqfd) with no pacing\n");
  uint32_t before = sh->irq_count;
  uint64_t one = 1;
  for (int i = 0; i < 10000; i++)
    if (write(irqfd, &one, sizeof one) != sizeof one)
      die("write");
  usleep(100000); /* 100 ms: let the guest drain */
  uint32_t got = sh->irq_count - before;
  printf("   host signalled 10_000 times, guest handler ran %u times (%.1f%% coalesced)\n", got, 100.0 * (10000 - got) / 10000);
  printf("   => an interrupt means \"look at the ring\", never \"exactly one event\".\n");

  /* ------------------------------------------------------------------ stop
   * Shut the guest down politely: set `stop` in guest RAM, then send one
   * more interrupt.  The guest is asleep in HLT and would never check `stop`
   * otherwise.  The interrupt wakes it, its loop sees `stop`, and it writes
   * PORT_DONE.  That makes KVM_RUN return, and the vCPU thread ends. */
  sh->stop = 1;
  inject_irqfd(&irqfd);   /* wake the guest out of HLT so it notices */
  pthread_join(th, NULL); /* wait for the vCPU thread to finish */

  printf("\nTotals: guest took %u interrupts in this run.\n", sh->irq_count);
  printf("Serial port: %lu exit(s), one per character of the hello message (not counted below).\n", atomic_load(&serial_exits));
  printf("KVM_RUN returned to userspace %lu time(s)  <- the guest's final PORT_DONE write only.\n", atomic_load(&userspace_exits));
  printf("Every interrupt above, and every PIC/HLT operation, was handled entirely in-kernel.\n");

  /* The pass/fail check.  ~40 000 interrupts, the PIC setup and EOIs, and
   * all the HLTs should have cost exactly ONE trip back to userspace.
   * Exit code: 0 = claim verified, 2 = more exits than expected,
   * 1 = an error along the way (die() or the READY timeout). */
  return atomic_load(&userspace_exits) == 1 ? 0 : 2;
}
