/*
 * irqfd_demo.c -- KVM_IRQFD on real KVM: host -> guest interrupts raised by
 *                 nothing more than write(eventfd), from ANY thread.
 *
 * THE PROBLEM
 * -----------
 * A device model finished some I/O and must interrupt the guest.  The classic way
 * is ioctl(vm, KVM_IRQ_LINE, ...) -- which must be issued by a process holding the
 * VM fd, i.e. the VMM.  A kernel component such as vhost-net has no such fd, and
 * every completion would have to bounce through the VMM.
 *
 * THE FIX: ioctl(vm, KVM_IRQFD, {fd, gsi})
 * ----------------------------------------
 * "Whenever this eventfd is signalled, raise interrupt line `gsi` in the guest."
 * KVM parks a waiter on the eventfd's wait queue; a write() to the eventfd (from
 * userspace, or eventfd_signal() from another kernel driver like vhost) wakes it,
 * and it injects the interrupt directly.  No VMM involvement, and the vCPU
 * thread is not disturbed except by the interrupt itself.
 *
 * (GSI = "global system interrupt": KVM's number for an interrupt input.  With
 *  the default routing that KVM_CREATE_IRQCHIP installs, GSIs 0-15 are wired to
 *  both the legacy PIC's pins and the IOAPIC's pins 0-15, so GSI 5 == IRQ 5.)
 *
 * WHAT THE GUEST DOES
 * -------------------
 * KVM_CREATE_IRQCHIP gives the guest an in-kernel 8259 PIC + IOAPIC + LAPIC (irqfd
 * needs an in-kernel irqchip: that's what it injects into).  The 16-bit guest:
 *   - installs a handler in the real-mode interrupt vector table (IVT),
 *   - programs the PIC: vectors based at 0x20, unmasks IRQ5  (GSI 5 -> vector 0x25),
 *   - `sti`, then sleeps in `hlt` (with an in-kernel irqchip HLT is handled in
 *     the kernel too: the vCPU just blocks until an interrupt arrives),
 *   - the handler does `inc [counter]` in guest RAM, sends EOI, `iret`.
 * The host can read that counter directly from guest RAM -- our "did it arrive?"
 * signal that costs no exit either.
 *
 * Why PIC interrupts reach the CPU at all: on a PC the 8259 PIC's output is
 * wired to the local APIC's LINT0 input.  At vCPU reset KVM programs the boot
 * CPU's LINT0 as "ExtINT" (accept PIC interrupts), the same "virtual wire" state
 * firmware leaves a real PC in.  The IOAPIC's redirection entries start out
 * masked, so of the two routes for GSI 5 only the PIC one delivers here.
 *
 * EXPERIMENTS
 * -----------
 *  A. PACED: host write(irqfd) -> spin until the guest's counter ticks; repeat N
 *     times.  Reports the host->guest delivery latency distribution.
 *  B. BURST: write(irqfd) 10 000 times back-to-back.  You get FAR fewer than
 *     10 000 interrupts.  irqfd is EDGE-triggered and an 8259 keeps a single
 *     pending bit per line, so signals arriving while one is pending merge.
 *     (Real devices solve this with a used ring: the interrupt just means
 *     "go look", never "exactly one event".)  KVM also merges earlier: for a
 *     PIC/IOAPIC GSI the irqfd wake-up only *schedules* the injection as
 *     deferred kernel work, and further signals before that work runs are
 *     absorbed by it.
 *  C. Same as A but using KVM_IRQ_LINE (the ioctl path) for comparison.
 *  Finally we report how many times KVM_RUN returned to userspace during all
 *  of that: exactly ONE (the guest's final "I'm done" port write).
 *
 * Run order in main() is A, C, B (B leaves interrupts in flight, so it goes last).
 *
 * BUILD & RUN:   make && ./irqfd_demo         (needs /dev/kvm access)
 */

#define _GNU_SOURCE
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

/* Stringify a macro's value so it can be pasted into the assembly text. */
#define STR_(x) #x
#define STR(x)  STR_(x)

#define GUEST_MEM_SIZE 0x10000
#define GUEST_LOAD_GPA 0x1000

#define IRQ_GSI    5                /* ISA IRQ5 == GSI 5 on the default routing        */
#define IRQ_VECTOR (0x20 + IRQ_GSI) /* PIC base 0x20 + pin 5                           */
#define PORT_DONE  0xf0             /* not claimed by any in-kernel device -> exits    */

/* Shared variables in GUEST RAM (host reads/writes them directly via `mem`). */
#define OFF_IRQ_COUNT 0x500 /* guest handler increments; host reads            */
#define OFF_STOP      0x508 /* host sets nonzero -> guest exits its hlt loop    */
#define OFF_READY     0x50c /* guest sets to 1 once PIC is programmed + sti     */

/* ---------------------------------------------------------------- guest ---
 * 16-bit real-mode code, assembled into .rodata and copied into guest RAM
 * (technique explained in 02-hypercalls/kvm_exits.c). */
extern const char guest_start[], guest_end[], guest_handler[];
__asm__(
    ".pushsection .rodata\n"
    ".code16\n"
    ".globl guest_start, guest_end, guest_handler\n"
    "guest_start:\n"
    "    cli\n"
    "    xor %ax, %ax\n"
    "    mov %ax, %ds\n"                      /* ds = 0: absolute addresses below are GPAs */
    "    mov %ax, %ss\n"
    "    mov $0x8000, %sp\n"

    /* Real-mode IVT: entry for vector V is 4 bytes at V*4 = {offset16, segment16}.
     * The IVT lives at physical address 0 (the real-mode default), i.e. at the
     * start of our guest RAM.  Vector 0x25 -> bytes 0x94..0x97. */
    "    movw $(guest_handler - guest_start + " STR(GUEST_LOAD_GPA) "), " STR(IRQ_VECTOR * 4) "\n"
    "    movw $0, " STR(IRQ_VECTOR * 4 + 2) "\n"

    /* Program the master 8259 PIC (in-kernel emulation; these OUTs do NOT exit to
     * userspace).  Port 0x20 = command, 0x21 = data.  Initialisation is a fixed
     * sequence of "Initialisation Command Words" (ICW1-4); afterwards writes to
     * 0x21 set the interrupt mask ("OCW1"). */
    "    mov $0x11, %al\n out %al, $0x20\n"    /* ICW1: init, edge-triggered, ICW4 follows */
    "    mov $0x20, %al\n out %al, $0x21\n"    /* ICW2: vector base = 0x20                 */
    "    mov $0x04, %al\n out %al, $0x21\n"    /* ICW3: slave on IRQ2                      */
    "    mov $0x01, %al\n out %al, $0x21\n"    /* ICW4: 8086 mode                          */
    "    mov $0xdf, %al\n out %al, $0x21\n"    /* OCW1: mask everything except IRQ5        */
                                               /* (0xdf = 1101 1111b: bit 5 clear)         */

    "    sti\n"                                /* accept interrupts from now on          */
    "    movl $1, " STR(OFF_READY) "\n"        /* tell the host we're listening */

    /* Sleep until told to stop.  `cli; test; sti; hlt` closes the lost-wakeup
     * window: STI delays interrupt delivery by one instruction, so an interrupt
     * cannot slip in between the test and the HLT and leave us asleep forever. */
    "1:  cli\n"
    "    cmpl $0, " STR(OFF_STOP) "\n"
    "    jne 2f\n"
    "    sti\n"
    "    hlt\n"                                /* sleep; an interrupt wakes us, runs the  */
    "    jmp 1b\n"                             /* handler, and execution resumes here    */
    "2:  mov $" STR(PORT_DONE) ", %dx\n"
    "    out %al, %dx\n"                       /* the ONLY userspace exit of the run */
    "    hlt\n"

    /* Interrupt handler for vector 0x25.  The CPU has already pushed FLAGS, CS
     * and IP; `iret` pops them (restoring FLAGS, so `incl` altering flags is ok). */
    "guest_handler:\n"
    "    push %ax\n"
    "    incl " STR(OFF_IRQ_COUNT) "\n"        /* count deliveries in guest RAM */
    "    mov $0x20, %al\n out %al, $0x20\n"    /* EOI to the PIC (in-kernel) */
    "    pop %ax\n"
    "    iret\n"
    "guest_end:\n"
    ".code64\n"
    ".popsection\n");

static void die(const char *w) { perror(w); exit(1); }

/* Monotonic clock in nanoseconds (integer). */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static uint8_t *mem;                 /* host pointer to guest RAM         */
static int vcpu_fd;
static struct kvm_run *run;
static atomic_ulong userspace_exits; /* how often KVM_RUN returned to us  */

/* The vCPU thread: KVM_RUN in a loop.  Note how little it does.
 * It runs on its own thread so that main() can inject interrupts while the
 * guest is running (KVM_RUN blocks for as long as the guest runs). */
static void *vcpu_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (ioctl(vcpu_fd, KVM_RUN, 0) < 0) { if (errno == EINTR) continue; die("KVM_RUN"); }
        atomic_fetch_add(&userspace_exits, 1);
        if (run->exit_reason == KVM_EXIT_IO && run->io.port == PORT_DONE)
            return NULL;
        fprintf(stderr, "unexpected exit %u\n", run->exit_reason);
        return NULL;
    }
}

/* Access a 32-bit variable in guest RAM.  volatile: the guest changes it
 * behind the compiler's back, so every access must really hit memory. */
static volatile uint32_t *guest_u32(unsigned off) { return (volatile uint32_t *)(mem + off); }

/* qsort comparator for uint64_t (ascending). */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Run n paced interrupts using `inject()`, report latency stats.
 * Each sample measures: injection call -> KVM delivers the interrupt -> the
 * halted vCPU wakes -> the guest handler runs and increments the counter ->
 * this thread sees the new value.  (KVM's "halt polling" may keep a halted
 * vCPU spinning briefly instead of sleeping, which lowers this latency.) */
static void paced(const char *name, int n, void (*inject)(void *), void *ctx)
{
    uint64_t *lat = malloc(n * sizeof *lat);
    for (int i = 0; i < n; i++) {
        uint32_t before = *guest_u32(OFF_IRQ_COUNT);
        uint64_t t0 = now_ns();
        inject(ctx);
        /* Spin until the guest handler ran (it bumped the counter in RAM).
         * __builtin_ia32_pause = x86 PAUSE: a spin-wait hint to the CPU. */
        while (*guest_u32(OFF_IRQ_COUNT) == before) __builtin_ia32_pause();
        lat[i] = now_ns() - t0;
    }
    /* Sort to read off percentiles: min, median (p50), 99th percentile. */
    qsort(lat, n, sizeof *lat, cmp_u64);
    printf("   %-34s n=%d  min %5.1f us   p50 %5.1f us   p99 %6.1f us\n", name, n,
           lat[0] / 1e3, lat[n / 2] / 1e3, lat[n * 99 / 100] / 1e3);
    free(lat);
}

/* Injection method 1: signal the irqfd (any thread or kernel code could). */
static void inject_irqfd(void *ctx)
{
    uint64_t one = 1;
    if (write(*(int *)ctx, &one, sizeof one) != sizeof one) die("write irqfd");
}

/* Injection method 2: the classic ioctl on the VM fd. */
static void inject_irqline(void *ctx)
{
    int vm = *(int *)ctx;
    /* An edge on a wire: drive high, then low.  Two ioctls per interrupt. */
    struct kvm_irq_level l = { .irq = IRQ_GSI, .level = 1 };
    if (ioctl(vm, KVM_IRQ_LINE, &l) < 0) die("KVM_IRQ_LINE");
    l.level = 0;
    if (ioctl(vm, KVM_IRQ_LINE, &l) < 0) die("KVM_IRQ_LINE");
}

int main(void)
{
    alarm(60); /* watchdog: SIGALRM kills us if anything hangs */
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) die("open /dev/kvm");
    int vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) die("KVM_CREATE_VM");

    /* The in-kernel interrupt controllers.  MUST come before KVM_CREATE_VCPU, and
     * is what makes irqfd possible: there has to be an in-kernel PIC/IOAPIC for
     * KVM to inject into. */
    if (ioctl(vm, KVM_CREATE_IRQCHIP, 0) < 0) die("KVM_CREATE_IRQCHIP");

    /* 64 KiB of guest RAM at GPA 0 (memslot 0). */
    mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    struct kvm_userspace_memory_region rg = { .slot = 0, .memory_size = GUEST_MEM_SIZE,
                                              .userspace_addr = (uintptr_t)mem };
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &rg) < 0) die("memslot");
    memcpy(mem + GUEST_LOAD_GPA, guest_start, guest_end - guest_start);

    /* ---------------------------------------------------- register the irqfd */
    int irqfd = eventfd(0, EFD_NONBLOCK);
    struct kvm_irqfd ifd = { .fd = irqfd, .gsi = IRQ_GSI };
    if (ioctl(vm, KVM_IRQFD, &ifd) < 0) die("KVM_IRQFD");
    printf("KVM_IRQFD: eventfd %d -> GSI %d  (guest vector 0x%x)\n\n", irqfd, IRQ_GSI, IRQ_VECTOR);

    /* vCPU in real mode at our code (see 02-hypercalls for the details). */
    vcpu_fd = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu_fd < 0) die("KVM_CREATE_VCPU");
    long sz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    run = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
    struct kvm_sregs sr;
    ioctl(vcpu_fd, KVM_GET_SREGS, &sr);
    sr.cs.base = 0; sr.cs.selector = 0;
    ioctl(vcpu_fd, KVM_SET_SREGS, &sr);
    struct kvm_regs regs = { .rip = GUEST_LOAD_GPA, .rflags = 2 };
    ioctl(vcpu_fd, KVM_SET_REGS, &regs);

    pthread_t th;
    pthread_create(&th, NULL, vcpu_thread, NULL);

    /* Wait (spinning on a guest-RAM flag; no exits involved) until the guest is ready. */
    uint64_t deadline = now_ns() + 5000000000ull; /* 5 s */
    while (!*guest_u32(OFF_READY)) {
        if (now_ns() > deadline) { fprintf(stderr, "guest never became ready\n"); return 1; }
        usleep(100);
    }
    printf("guest is up: PIC programmed, handler installed, sleeping in HLT\n\n");

    /* ------------------------------------------------------------- A: paced */
    printf("A. PACED delivery latency, host write() -> guest handler has run\n");
    paced("irqfd: write(eventfd)", 20000, inject_irqfd, &irqfd);

    /* ------------------------------------------------------------ C: ioctl */
    printf("\nC. same, via the classic ioctl(KVM_IRQ_LINE) path\n");
    paced("KVM_IRQ_LINE: 2 ioctls", 20000, inject_irqline, &vm);
    printf("   (Both are a syscall from a non-vCPU thread. irqfd's real win is that the *caller\n"
           "    needn't be the VMM*: vhost signals it from the kernel with no syscall at all.)\n");

    /* -------------------------------------------------------------- B: burst */
    printf("\nB. BURST: 10000 back-to-back write(irqfd) with no pacing\n");
    uint32_t before = *guest_u32(OFF_IRQ_COUNT);
    uint64_t one = 1;
    for (int i = 0; i < 10000; i++)
        if (write(irqfd, &one, sizeof one) != sizeof one) die("write");
    usleep(100000); /* let the guest drain */
    uint32_t got = *guest_u32(OFF_IRQ_COUNT) - before;
    printf("   host signalled 10000 times, guest handler ran %u times (%.1f%% coalesced)\n",
           got, 100.0 * (10000 - got) / 10000);
    printf("   => an interrupt means \"look at the ring\", never \"exactly one event\".\n");

    /* ------------------------------------------------------------------ stop */
    *guest_u32(OFF_STOP) = 1;
    inject_irqfd(&irqfd); /* wake the guest out of HLT so it notices */
    pthread_join(th, NULL);

    printf("\nTotals: guest took %u interrupts in this run.\n", *guest_u32(OFF_IRQ_COUNT));
    printf("KVM_RUN returned to userspace %lu time(s)  <- the guest's final PORT_DONE write only.\n",
           atomic_load(&userspace_exits));
    printf("Every interrupt above, and every PIC/HLT operation, was handled entirely in-kernel.\n");
    return atomic_load(&userspace_exits) == 1 ? 0 : 2;
}
