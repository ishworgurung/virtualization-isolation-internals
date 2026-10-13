/*
 * kvm_exits.c -- a ~300-line VMM that boots a 16-bit guest and shows, live,
 *                every kind of VM exit that matters, plus what each one costs.
 *
 * This is the same skeleton every real VMM (Firecracker, crosvm,
 * cloud-hypervisor, QEMU) is built on:
 *
 *     open /dev/kvm -> KVM_CREATE_VM -> give it memory -> KVM_CREATE_VCPU
 *     -> mmap the shared `struct kvm_run` -> loop { KVM_RUN; switch
 *     (exit_reason) }
 *
 * WHO DOES WHAT
 * -------------
 *   KVM   = a Linux kernel module exposed as /dev/kvm.  It programs the CPU's
 *           virtualisation extensions (Intel VT-x / AMD-V) and runs guest code.
 *   VMM   = "virtual machine monitor" = THIS PROGRAM (in production: QEMU,
 *           Firecracker, ...).  An ordinary userspace process that asks KVM, via
 *           ioctl() calls on file descriptors, to create a VM, gives it memory,
 *           and emulates whatever hardware KVM does not emulate itself.
 *   Every KVM object is a file descriptor: /dev/kvm (the system), the VM fd
 *   returned by KVM_CREATE_VM, and one fd per virtual CPU (vCPU).
 *
 * THE MENTAL MODEL
 * ----------------
 * KVM_RUN switches the CPU into guest mode (VMX non-root on Intel, SVM guest on
 * AMD).  The guest runs at full native speed until it does something the
 * hardware is configured to intercept.  That is a **VM exit**: the CPU saves
 * guest state, loads host state, and lands in KVM in the kernel.
 *
 *   exit is handled INSIDE KVM (CPUID, VMCALL to a known hypercall, MSR access,
 *   EPT fault on RAM, timer interrupt ...):
 *        -> KVM fixes it up and re-enters the guest.  KVM_RUN never returns.
 *        -> cost: typically a few hundred ns to ~1 us, depending on CPU and
 *           security mitigations ("light exit"; ~0.45 us measured on the
 *           machine this was written on)
 *
 *   exit needs USERSPACE (port I/O, MMIO to an emulated device, HLT without an
 *   in-kernel irqchip, a hypercall routed to userspace):
 *        -> KVM_RUN returns to us with exit_reason set; we emulate; call
 *           KVM_RUN again.
 *        -> cost: light-exit cost PLUS a kernel->user->kernel round trip
 *           ("heavy exit"), i.e. noticeably more (~2.5-3x here).
 *
 * The guest program below deliberately triggers each kind in turn, and times
 * 100 000 of each of the three cheapest-to-trigger ones.
 *
 * BUILD & RUN:   make && ./kvm_exits         (needs read/write access to
 * /dev/kvm; on most distros that means being in the `kvm` group)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>      /* KVM ioctls and struct kvm_run                   */
#include <linux/kvm_para.h> /* KVM_HC_* hypercall numbers                      */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---- Iteration counts for the cost benchmark (must match guest asm below).
 * ---- */
/* STR(x) turns a macro's VALUE into a string literal ("two-level" trick: the
 * outer macro expands x first, the inner one stringifies the result), so C
 * constants can be pasted into the assembly text below.  Comments inside the
 * #defines are stripped before macro expansion, so they do not end up in the
 * strings. */
#define STR_(x) #x
#define STR(x) STR_(x)
#define N_OUT                                                                  \
  100000 /* OUT instructions  -> heavy exits (return to userspace) */
#define N_CPUID                                                                \
  100000 /* CPUID             -> light exits (handled in-kernel)   */
#define N_VMCALL                                                               \
  100000 /* VMCALL, unknown nr-> light exits (handled in-kernel)   */

/* ---- The tiny "hardware" our guest talks to: I/O ports the VMM emulates. ----
 */
/* x86 has a separate 64 KiB "I/O port" address space accessed with the IN/OUT
 * instructions (a legacy of the original PC).  Because this VM has NO in-kernel
 * interrupt controller (no KVM_CREATE_IRQCHIP), KVM claims none of these ports
 * and every OUT reaches us.  0x21/0x22 would be a bad choice in 08/09, where
 * the in-kernel 8259 PIC owns ports 0x20/0x21. */
#define PORT_CONSOLE                                                           \
  0x3f8                   /* byte written here -> we print it                 */
#define PORT_REPORT 0x10  /* guest reports a value (used for results)  */
#define PORT_BENCH_A 0x30 /* target of the OUT benchmark loop */
#define PORT_MARK_BEGIN                                                        \
  0x21 /* "start timing" marker                            */
#define PORT_MARK_END                                                          \
  0x22 /* "stop timing" marker                             */

#define GUEST_LOAD_GPA                                                         \
  0x1000 /* guest code is copied to this guest-physical addr */
#define GUEST_MEM_SIZE                                                         \
  0x10000 /* 64 KiB of RAM at GPA 0.  MMIO test address 0xd0000                \
             is deliberately OUTSIDE this -> not RAM -> MMIO. */
#define MMIO_TEST_GPA 0xd0000 /* documentation only; the asm hard-codes it */

/* ========================================================================== *
 * The guest program (16-bit real mode, AT&T syntax, assembled by GCC).
 *
 * Real mode keeps setup to ~10 lines (no GDT / page tables).  The exit
 * mechanics are identical to a 64-bit guest.
 * Labels are byte offsets from `guest_start`; the code is loaded at GPA 0x1000.
 *
 * How this works mechanically:
 *  - The top-level __asm__ block is assembled into THIS executable's .rodata
 *    section.  `.code16` tells the assembler to emit 16-bit machine code; the
 *    host never executes these bytes, it only copies them into guest RAM.
 *    `.code64` switches the assembler back for the rest of the file.
 *  - `guest_start`/`guest_end` are exported as symbols so C can see the blob as
 *    a byte array and memcpy() it.
 *  - Because the code will run at GPA 0x1000 (not where it sits in the host
 *    binary), any absolute address it needs is computed as
 *    `label - guest_start + GUEST_LOAD_GPA`: a *difference* of labels is a
 *    plain number the assembler can resolve, independent of where the host
 *    binary is loaded.
 *  - Real-mode addresses are segment*16 + offset.  We set DS = SS = 0, so for
 *    data accesses offset == physical address.  With paging off, "physical"
 *    here means guest-physical (GPA).
 *  - Numeric labels (`1:`, `2f` = "label 2 forward", `1b` = "label 1
 *    backward") are local labels.
 *  - In 16-bit code, instructions on 32-bit registers (%eax, %esi) are still
 *    legal on any 386+ CPU; the assembler adds an operand-size prefix (0x66).
 * ========================================================================== */
extern const char guest_start[], guest_end[], guest_hc_insn[], guest_msg[];

// clang-format off
__asm__(
    ".pushsection .rodata\n"
    ".code16\n"
    ".globl guest_start, guest_end, guest_hc_insn, guest_msg\n"
    "guest_start:\n"
    "    cli\n"                  /* no interrupts: we have no handlers          */
    "    xor  %ax, %ax\n"
    "    mov  %ax, %ds\n"        /* DS = 0: data offsets are physical addrs     */
    "    mov  %ax, %ss\n"        /* SS = 0                                      */
    "    mov  $0x8000, %sp\n"    /* stack grows down from GPA 0x8000 (in RAM)   */

    /* ---- PHASE 1: port I/O.  Each OUT => KVM_EXIT_IO ---------------------- */
    "    mov  $" STR(PORT_CONSOLE) ", %dx\n"
    "    mov  $(guest_msg - guest_start + " STR(GUEST_LOAD_GPA) "), %si\n"
    "1:  lodsb\n"                /* al = *si++                                  */
    "    test %al, %al\n"
    "    jz   2f\n"              /* NUL terminator -> done                      */
    "    out  %al, %dx\n"        /* <- VM exit, reason IO, handled in USERSPACE */
    "    jmp  1b\n"
    "2:\n"

    /* ---- PHASE 2: MMIO.  0xd0000 is not backed by a memslot => KVM_EXIT_MMIO. */
    "    mov  $0xd000, %ax\n"
    "    mov  %ax, %es\n"        /* es:0 == linear 0xd0000                      */
    "    movw $0x1234, %es:0\n"  /* MMIO WRITE: EPT misconfig/violation -> exit */
    "    mov  %es:4, %bx\n"      /* MMIO READ : VMM must supply the value       */
    "    mov  $" STR(PORT_REPORT) ", %dx\n"
    "    mov  %bx, %ax\n"
    "    out  %ax, %dx\n"        /* report what we read back (expect 0xbeef)    */

    /* ---- PHASE 3a: hypercall with an unknown number.  In-kernel, ENOSYS. --- */
    /* ABI (Documentation/virt/kvm/x86/hypercalls.rst): RAX = nr, RBX/RCX/RDX/RSI = args,
     * result in RAX.  The instruction is VMCALL (Intel) / VMMCALL (AMD); the VMM
     * patches the 3 bytes at guest_hc_insn to match the host CPU vendor.
     * (By default KVM would also fix up the wrong-vendor instruction itself when
     * it traps with #UD -- KVM_X86_QUIRK_FIX_HYPERCALL_INSN -- but patching up
     * front avoids relying on that quirk.)  Hypercalls must be issued at CPL 0;
     * real mode always runs at CPL 0.  Outside 64-bit mode KVM uses only the
     * low 32 bits of each register. */
    "    mov  $0x1234, %eax\n"
    "guest_hc_insn:\n"
    "    .byte 0x0f, 0x01, 0xc1\n"   /* VMCALL; patched to VMMCALL (0f 01 d9) on AMD */
    "    mov  $" STR(PORT_REPORT) ", %dx\n"
    "    out  %eax, %dx\n"       /* report RAX: expect -KVM_ENOSYS = 0xfffffc18 */

    /* ---- PHASE 3b: KVM_HC_MAP_GPA_RANGE routed to USERSPACE ---------------- */
    /* KVM validates the arguments before exiting: gpa must be page-aligned and
     * npages non-zero, otherwise the guest just gets -KVM_EINVAL. */
    "    mov  $12, %eax\n"             /* nr = KVM_HC_MAP_GPA_RANGE              */
    "    mov  $0x100000, %ebx\n"       /* a0: gpa                                */
    "    mov  $1, %ecx\n"              /* a1: npages                             */
    "    xor  %edx, %edx\n"            /* a2: attributes (4K page, decrypted)    */
    "guest_hc_insn2:\n"
    "    .byte 0x0f, 0x01, 0xc1\n"     /* VMCALL again (also patched, see below) */
    "    mov  $" STR(PORT_REPORT) ", %dx\n"
    "    out  %eax, %dx\n"             /* report result the VMM chose (expect 0) */

    /* ---- PHASE 4: cost benchmark -------------------------------------------- */
    /* Each benchmark is bracketed by two OUTs to marker ports so the VMM can
     * start/stop a stopwatch.  Those two markers are heavy exits themselves,
     * a negligible 2 out of 100 000 iterations. */
    /* 4A: N x OUT  -> every iteration returns to userspace (heavy exit).       */
    "    mov  $" STR(PORT_MARK_BEGIN) ", %dx\n"
    "    out  %al, %dx\n"
    "    mov  $" STR(PORT_BENCH_A) ", %dx\n"
    "    mov  $" STR(N_OUT) ", %esi\n"
    "3:  out  %al, %dx\n"
    "    dec  %esi\n"
    "    jnz  3b\n"
    "    mov  $" STR(PORT_MARK_END) ", %dx\n"
    "    out  %al, %dx\n"

    /* 4B: N x CPUID -> unconditional exit, but handled entirely in-kernel.     */
    /* (On Intel, CPUID in a guest ALWAYS causes a VM exit; on AMD it can be
     * intercepted optionally and KVM always intercepts it.) */
    "    mov  $" STR(PORT_MARK_BEGIN) ", %dx\n"
    "    out  %al, %dx\n"
    "    mov  $" STR(N_CPUID) ", %esi\n"
    "4:  xor  %eax, %eax\n"
    "    cpuid\n"                       /* clobbers eax/ebx/ecx/edx -> esi is the counter */
    "    dec  %esi\n"
    "    jnz  4b\n"
    "    mov  $" STR(PORT_MARK_END) ", %dx\n"
    "    out  %al, %dx\n"

    /* 4C: N x VMCALL with an unknown hypercall number: in-kernel, returns ENOSYS. */
    "    mov  $" STR(PORT_MARK_BEGIN) ", %dx\n"
    "    out  %al, %dx\n"
    "    mov  $" STR(N_VMCALL) ", %esi\n"
    "5:  mov  $0x1234, %eax\n"
    "guest_hc_insn3:\n"
    "    .byte 0x0f, 0x01, 0xc1\n"
    "    dec  %esi\n"
    "    jnz  5b\n"
    "    mov  $" STR(PORT_MARK_END) ", %dx\n"
    "    out  %al, %dx\n"

    /* ---- PHASE 5: HLT.  With no in-kernel irqchip this exits to userspace. --- */
    "    hlt\n"

    /* Data: the console message, NUL-terminated (.asciz). */
    "guest_msg:\n"
    "    .asciz \"hello from the guest! (via OUT to port 0x3f8)\\n\"\n"
    "guest_end:\n"
    ".code64\n"
    ".popsection\n"
);
// clang-format on

/* Offsets of the other two VMCALL sites, computed by the assembler.
 * (`__asm__("name")` pins the C identifier to that exact assembler symbol.) */
extern const char guest_hc_insn2[] __asm__("guest_hc_insn2");
extern const char guest_hc_insn3[] __asm__("guest_hc_insn3");

/* -------------------------------------------------------------------------- */

static void die(const char *what) {
  perror(what); /* prints `what: <strerror(errno)>` */
  exit(1);
}

/* Monotonic clock in nanoseconds (never jumps backwards, unlike wall time). */
static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* Human names for the exit reasons we count (from <linux/kvm.h>). */
static const char *exit_name(unsigned r) {
  switch (r) {
  case KVM_EXIT_UNKNOWN:
    return "UNKNOWN";
  case KVM_EXIT_EXCEPTION:
    return "EXCEPTION";
  case KVM_EXIT_IO:
    return "IO";
  case KVM_EXIT_HYPERCALL:
    return "HYPERCALL";
  case KVM_EXIT_DEBUG:
    return "DEBUG";
  case KVM_EXIT_HLT:
    return "HLT";
  case KVM_EXIT_MMIO:
    return "MMIO";
  case KVM_EXIT_SHUTDOWN:
    return "SHUTDOWN";
  case KVM_EXIT_FAIL_ENTRY:
    return "FAIL_ENTRY";
  case KVM_EXIT_INTERNAL_ERROR:
    return "INTERNAL_ERROR";
  default:
    return "OTHER";
  }
}

/* Is the HOST CPU an AMD one?  Used to decide whether the guest's VMCALLs must
 * be rewritten as VMMCALLs.  CPUID leaf 0 returns the 12-byte vendor string in
 * EBX, EDX, ECX ("Auth" "enti" "cAMD" for AMD, "Genu" "ineI" "ntel" for Intel);
 * checking EBX alone is enough to tell the two apart. */
static int host_is_amd(void) {
  unsigned eax, ebx, ecx, edx;
  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "0"(0)); /* input: EAX = 0 (leaf 0) */
  return ebx == 0x68747541; /* "Auth" of "AuthenticAMD" */
}

int main(void) {
  /* ---------------------------------------------------------------- setup */
  /* The system-wide KVM handle.  O_CLOEXEC: don't leak it into child
   * processes if we ever exec. */
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0)
    die("open /dev/kvm");
  /* The KVM API version has been 12 since Linux 2.6.22 and is never expected
   * to change; new functionality is discovered via capabilities instead. */
  int api = ioctl(kvm, KVM_GET_API_VERSION, 0);
  if (api != 12) {
    fprintf(stderr, "unexpected KVM API %d\n", api);
    return 1;
  }

  /* Create an empty VM: no memory, no vCPUs yet.  Returns the VM fd. */
  int vm = ioctl(kvm, KVM_CREATE_VM, 0);
  if (vm < 0)
    die("KVM_CREATE_VM");

  /*
   * Ask KVM to forward KVM_HC_MAP_GPA_RANGE to *userspace* instead of
   * handling it (the default is ENOSYS).  This is how confidential-VM VMMs
   * (SEV-SNP/TDX) learn that the guest flipped pages private<->shared.
   * It shows the general point: a hypercall is the guest->hypervisor API, and
   * KVM decides per-number whether the kernel or the VMM answers it.
   * args[0] is a bitmask of hypercall numbers to forward; currently
   * KVM_HC_MAP_GPA_RANGE is the only one KVM accepts here.  Older kernels
   * lack the capability, in which case phase 3b simply reports ENOSYS.
   */
  int hypercall_routed = 0;
  {
    struct kvm_enable_cap cap = {.cap = KVM_CAP_EXIT_HYPERCALL,
                                 .args = {1ull << KVM_HC_MAP_GPA_RANGE}};
    hypercall_routed = ioctl(vm, KVM_ENABLE_CAP, &cap) == 0;
    if (!hypercall_routed)
      perror("KVM_CAP_EXIT_HYPERCALL (continuing)");
  }

  /* Guest RAM: anonymous host memory, registered as memslot 0 at GPA 0.
   * A "memslot" tells KVM: guest-physical range [guest_phys_addr,
   * guest_phys_addr + memory_size) is backed by these host virtual addresses.
   * KVM then builds the second-level page tables (EPT/NPT) lazily as the
   * guest touches pages.  Any GPA outside every memslot is, by definition,
   * MMIO.  MAP_NORESERVE: don't reserve swap for it (irrelevant at 64 KiB,
   * but the usual choice for large guest RAM). */
  uint8_t *mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mem == MAP_FAILED)
    die("mmap guest mem");
  struct kvm_userspace_memory_region region = {
      .slot = 0,
      .guest_phys_addr = 0,
      .memory_size = GUEST_MEM_SIZE,
      .userspace_addr = (uintptr_t)mem,
  };
  if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0)
    die("SET_USER_MEMORY_REGION");

  /* Load the guest image and patch the three VMCALL sites if needed.
   * Writing to `mem` IS writing guest RAM: the VMM and the guest share it. */
  size_t code_len = guest_end - guest_start;
  memcpy(mem + GUEST_LOAD_GPA, guest_start, code_len);
  if (host_is_amd()) {
    const char *sites[] = {guest_hc_insn, guest_hc_insn2, guest_hc_insn3};
    size_t offs[] = {sites[0] - guest_start, sites[1] - guest_start,
                     sites[2] - guest_start};
    for (int i = 0; i < 3; i++)
      memcpy(mem + GUEST_LOAD_GPA + offs[i], "\x0f\x01\xd9", 3); /* VMMCALL */
  }

  /* The vCPU and its shared `kvm_run` page (how KVM tells us why it exited). */
  int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0); /* 0 = vCPU id */
  if (vcpu < 0)
    die("KVM_CREATE_VCPU");
  /* `struct kvm_run` is not passed to KVM_RUN as an argument; instead we
   * mmap() the vCPU fd and KVM fills this shared memory on every exit.  The
   * mapping may be larger than the struct (it can include a page for port-I/O
   * data and a coalesced-MMIO ring), hence asking KVM for the size. */
  long run_sz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  struct kvm_run *run =
      mmap(NULL, run_sz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
  if (run == MAP_FAILED)
    die("mmap kvm_run");

  /*
   * CPUID: tell KVM what the guest's CPUID instruction should report.  We
   * simply pass through everything KVM says it supports, which includes the
   * "KVM" paravirt leaves (0x4000_0000 signature, 0x4000_0001 PV feature
   * bits) a Linux guest uses to discover KVM features such as hypercalls.
   * Note: KVM itself only *enforces* those PV feature bits (refusing a
   * hypercall the guest wasn't told about) if the VMM opts in with
   * KVM_CAP_ENFORCE_PV_FEATURE_CPUID; we don't, so this call is good practice
   * rather than a hard requirement for the demo.
   * 128 entries is enough on current CPUs; KVM fails with E2BIG if not.
   */
  {
    struct kvm_cpuid2 *cpuid =
        calloc(1, sizeof *cpuid + 128 * sizeof(struct kvm_cpuid_entry2));
    cpuid->nent = 128;
    if (ioctl(kvm, KVM_GET_SUPPORTED_CPUID, cpuid) < 0)
      die("GET_SUPPORTED_CPUID");
    if (ioctl(vcpu, KVM_SET_CPUID2, cpuid) < 0)
      die("SET_CPUID2");
    free(cpuid);
  }

  /* Registers: real mode, CS base 0 so RIP == linear address.
   * A freshly created x86 vCPU is in the architectural reset state: real
   * mode, CS base 0xffff0000 and IP 0xfff0 (the BIOS entry point at the top
   * of the 4 GiB space).  We have no BIOS, so we point CS at 0 and RIP at our
   * code.  "sregs" = segment/control registers, "regs" = general-purpose
   * registers plus RIP and RFLAGS. */
  struct kvm_sregs sregs;
  ioctl(vcpu, KVM_GET_SREGS, &sregs);
  sregs.cs.base = 0;
  sregs.cs.selector = 0;
  ioctl(vcpu, KVM_SET_SREGS, &sregs);
  struct kvm_regs regs = {.rip = GUEST_LOAD_GPA,
                          .rflags = 0x2 /* bit 1 is reserved-1 */};
  ioctl(vcpu, KVM_SET_REGS, &regs);

  printf("VMM: guest image %zu bytes @ GPA 0x%x, host=%s, hypercall->userspace "
         "%s\n\n",
         code_len, GUEST_LOAD_GPA,
         host_is_amd() ? "AMD (VMMCALL)" : "Intel (VMCALL)",
         hypercall_routed ? "enabled" : "unavailable");

  /* ------------------------------------------------------------ vCPU loop */
  /*
   * THIS LOOP is the heart of every VMM.  In Firecracker it is the vCPU
   * thread's run loop (`Vcpu::run_emulation()` in recent versions):
   * `match self.fd.run() { Ok(VcpuExit::IoOut(..)) ..` -- Rust wrappers from
   * the rust-vmm kvm-ioctls crate around the same ioctl.
   *
   * ioctl(KVM_RUN) blocks for as long as the guest runs (and for as long as
   * KVM can handle exits on its own), and returns only when userspace must
   * act.  It returns -1/EINTR if a signal interrupted it; real VMMs use a
   * signal to "kick" a vCPU thread out of the guest, e.g. to pause the VM.
   */
  unsigned long exits[64] = {0}; /* userspace-visible exits, by reason */
  double t_begin = 0;
  unsigned bench_no = 0; /* which of the three benchmarks is running */
  const char *bench_name[] = {"OUT  (heavy exit: returns to userspace)",
                              "CPUID (light exit: handled in kernel)   ",
                              "VMCALL(light exit: handled in kernel)   "};
  const unsigned long bench_n[] = {N_OUT, N_CPUID, N_VMCALL};
  int running = 1;

  while (running) {
    if (ioctl(vcpu, KVM_RUN, 0) < 0) {
      if (errno == EINTR)
        continue;
      die("KVM_RUN");
    }
    /* `& 63` keeps the index inside the array even for unexpected, large
     * exit-reason numbers. */
    exits[run->exit_reason & 63]++;

    switch (run->exit_reason) {

    case KVM_EXIT_IO: {
      /* Port I/O.  Data lives inside the kvm_run page at data_offset.
       * run->io tells us: direction (IN/OUT), size (1, 2 or 4 bytes), port,
       * and count (> 1 only for string instructions like `rep outsb`, which
       * this guest does not use).  For an IN, we would write the value into
       * that same data area before the next KVM_RUN. */
      uint8_t *data = (uint8_t *)run + run->io.data_offset;
      uint16_t port = run->io.port;
      if (run->io.direction != KVM_EXIT_IO_OUT)
        break; /* no IN in this guest */

      if (port == PORT_CONSOLE) {
        putchar(data[0]); /* emulated UART                  */
      } else if (port == PORT_REPORT) {
        uint32_t v = 0;
        memcpy(&v, data, run->io.size); /* 1, 2 or 4 bytes, little-endian */
        printf("VMM: guest reported 0x%x (%u-bit)\n", v, run->io.size * 8);
      } else if (port == PORT_MARK_BEGIN) {
        t_begin = now_ns(); /* start the stopwatch            */
      } else if (port == PORT_MARK_END) {
        double dt = now_ns() - t_begin;
        printf("VMM: bench %s  %lu iters  %.0f ns/iter  (%.1f ms total)\n",
               bench_name[bench_no], bench_n[bench_no], dt / bench_n[bench_no],
               dt / 1e6);
        bench_no++;
      }
      /* PORT_BENCH_A: deliberately do nothing -- the *exit itself* is what we
       * time. */
      break;
    }

    case KVM_EXIT_MMIO:
      /* The guest touched a GPA no memslot covers.  KVM can't resolve it,
       * so it hands the access to us: address, length, direction, data.
       * (KVM decoded the faulting instruction to find out the length and the
       * value.  On Intel, the first access to such a GPA is an "EPT
       * violation"; KVM then installs a deliberately invalid entry so later
       * accesses cause a cheaper-to-identify "EPT misconfiguration" exit.) */
      if (run->mmio.is_write) {
        uint32_t v = 0;
        memcpy(&v, run->mmio.data, run->mmio.len);
        printf("VMM: MMIO WRITE gpa=0x%llx len=%u value=0x%x\n",
               (unsigned long long)run->mmio.phys_addr, run->mmio.len, v);
      } else {
        /* Emulate a device register that always reads 0xbeef.  We fill
         * run->mmio.data; KVM completes the guest's load instruction with it
         * when we call KVM_RUN again. */
        uint16_t v = 0xbeef;
        memcpy(run->mmio.data, &v, run->mmio.len);
        printf("VMM: MMIO READ  gpa=0x%llx len=%u -> supplying 0x%x\n",
               (unsigned long long)run->mmio.phys_addr, run->mmio.len, v);
      }
      break;

    case KVM_EXIT_HYPERCALL:
      /* Only reached for hypercall numbers we routed to userspace above. */
      printf(
          "VMM: HYPERCALL nr=%llu args=[0x%llx, %llu, %llu] -> returning 0\n",
          (unsigned long long)run->hypercall.nr,
          (unsigned long long)run->hypercall.args[0],
          (unsigned long long)run->hypercall.args[1],
          (unsigned long long)run->hypercall.args[2]);
      run->hypercall.ret = 0; /* becomes RAX in the guest on the next KVM_RUN */
      break;

    case KVM_EXIT_HLT:
      /* HLT = "halt until the next interrupt".  With no in-kernel interrupt
       * controller KVM cannot know when an interrupt will come, so it hands
       * the decision to us.  We treat it as "guest is finished". */
      printf("VMM: guest executed HLT -> shutting down\n");
      running = 0;
      break;

    case KVM_EXIT_FAIL_ENTRY:
      /* The CPU refused to enter guest mode: invalid guest state. */
      fprintf(
          stderr, "FAIL_ENTRY: hw reason 0x%llx\n",
          (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
      return 1;
    case KVM_EXIT_INTERNAL_ERROR:
      /* KVM could not emulate something (e.g. an instruction it can't decode). */
      fprintf(stderr, "INTERNAL_ERROR suberror=%u\n", run->internal.suberror);
      return 1;
    case KVM_EXIT_SHUTDOWN:
      /* A "triple fault" (fault while handling a fault while handling a
       * fault) -- on real hardware the machine resets. */
      fprintf(stderr, "guest triple-faulted (SHUTDOWN)\n");
      return 1;
    default:
      fprintf(stderr, "unhandled exit %u (%s)\n", run->exit_reason,
              exit_name(run->exit_reason));
      return 1;
    }
  }

  /* --------------------------------------------------------------- report */
  printf("\nExits that reached USERSPACE (KVM_RUN returned), by reason:\n");
  for (unsigned r = 0; r < 64; r++)
    if (exits[r])
      printf("  %-10s %lu\n", exit_name(r), exits[r]);
  printf("(The %d CPUID + %d VMCALL exits do not appear: KVM absorbed them.)\n",
         N_CPUID, N_VMCALL);
  printf(
      "Verify from outside with:  sudo perf kvm stat record -- ./kvm_exits\n");
  return 0;
}
