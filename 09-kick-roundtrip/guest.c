/*
 * guest.c -- the "driver" that runs INSIDE the virtual machine.
 *
 * It plays the part of an operating system's device driver: it sends N
 * requests to a device and waits for them to complete.  The device lives on
 * the host (kick_roundtrip.c).  The two talk the way real virtual devices
 * (virtio) do:
 *
 *   - to send a request, the guest writes to the device's DOORBELL address
 *     (a "kick"),
 *   - when the device finishes one, it adds 1 to `completed` in shared memory
 *     and raises an INTERRUPT, which wakes the guest if it was sleeping.
 *
 * (A real virtio driver also puts a description of each request in a shared
 * queue, the "ring", before kicking.  This lab leaves the ring out: every
 * request is identical, so the device only needs to know how many there are.)
 *
 * This isn't a normal Linux program.  There is no operating system in the
 * guest: no libc, no printf, no files.  It's just our code on a bare
 * (virtual) CPU, compiled with -ffreestanding -nostdlib.  The host starts the
 * vCPU in 32-bit mode with flat memory, so a pointer's value is simply the
 * guest physical address it points at.
 *
 * How it gets into the VM (see the Makefile):
 *   guest.c --gcc -m32--> guest.elf --objcopy--> guest.bin --#embed--> kick_roundtrip
 * The host copies guest.bin to address 0x1000 and starts the vCPU there.
 */
#include "shared.h"
#include <stdint.h>

/* The mailbox shared with the host (layout in shared.h).
 * `volatile`: the device changes `completed` behind the compiler's back, so
 * every read must really go to memory rather than use a remembered value. */
static volatile struct shared *const sh = (volatile struct shared *)SHARED_GPA;

/* The doorbell.  A 32-bit store here is a "kick".  volatile, because the
 * store itself is the whole point: the compiler must never drop it or merge
 * two of them. */
static volatile uint32_t *const doorbell = (volatile uint32_t *)MMIO_DOORBELL;

/* ------------------------------------------------------------------------
 * The only assembly in the guest: one-instruction helpers for CPU
 * instructions that have no C equivalent.  (The Linux kernel wraps them the
 * same way.)  How to read one:
 *   asm volatile("outb %0, %1" : : "a"(val), "Nd"(port))
 *     "outb %0, %1"  the instruction; %0 and %1 are the operands below
 *     "a"(val)       put val in register al (outb requires it there)
 *     "Nd"(port)     the port, as a constant or in register dx
 *     volatile       never delete or move this: it has side effects
 *     "memory"       (on some) don't move memory reads/writes across this
 * ------------------------------------------------------------------------ */

/* Write one byte to an I/O port (a small separate address space x86 uses
 * for devices: the interrupt controller is at 0x20/0x21, our markers at
 * 0xe1-0xf0). */
static inline void outb(uint16_t port, uint8_t val) { asm volatile("outb %0, %1" : : "a"(val), "Nd"(port)); }

/* Interrupts off / on. */
static inline void cli(void) { asm volatile("cli" : : : "memory"); }
static inline void sti(void) { asm volatile("sti" : : : "memory"); }

/* Turn interrupts on AND sleep until one arrives, as one unit.  Must be a
 * single asm statement: see wait_for_completion() for why. */
static inline void sti_hlt(void) { asm volatile("sti; hlt" : : : "memory"); }

/* Sleep forever (with interrupts off nothing can wake us). */
static inline __attribute__((noreturn)) void halt_forever(void) {
  for (;;)
    asm volatile("cli; hlt");
}

/* Tell the host "this moment happened" (see the PORT_* markers in shared.h).
 * The value written doesn't matter, only which port. */
static void marker(uint16_t port) { outb(port, 0); }

/* ------------------------------------------------ interrupt handler (IDT) */

/* When an interrupt arrives, the CPU looks its number (the "vector") up in
 * the IDT (Interrupt Descriptor Table), an array of 256 entries saying where
 * each vector's handler is.  The entry layout is fixed by the CPU: */
struct idt_gate {
  uint16_t offset_lo;      /* handler address, low 16 bits                        */
  uint16_t selector;       /* code segment to run it in (SEL_CODE)                */
  uint8_t zero;            /* unused, must be 0                                   */
  uint8_t type;            /* 0x8e = present, kernel privilege, 32-bit interrupt  */
                           /*        gate (the CPU turns interrupts off on entry) */
  uint16_t offset_hi;      /* handler address, high 16 bits                       */
} __attribute__((packed)); /* packed: no padding, exactly the CPU's layout */

/* In .bss, so not part of guest.bin.  Guest RAM starts out all zeros, and an
 * all-zero entry means "no handler". */
static struct idt_gate idt[256];

/* The interrupt handler.  It only counts: the real information ("how many
 * requests are done") is in sh->completed, which the device updates before
 * raising the interrupt.  The interrupt just means "go and look".
 *
 * __attribute__((interrupt)) makes GCC generate the special entry/exit code
 * a handler needs: save every register it uses, and return with `iret`.  An
 * interrupt can arrive between any two instructions, so the handler must
 * leave everything exactly as it found it. */
struct interrupt_frame; /* what the CPU pushed on entry; we don't look inside */

__attribute__((interrupt)) static void irq_handler(struct interrupt_frame *frame) {
  (void)frame;
  sh->irq_count++;
  /* EOI ("End Of Interrupt"): tell the interrupt controller we're done.
   * Until it gets this, it won't deliver another interrupt on this line. */
  outb(0x20, 0x20);
}

/* Point IDT entry `vector` at `handler`, then tell the CPU where the IDT is
 * (`lidt` takes the table's size minus 1, then its address). */
static void idt_install(uint8_t vector, void (*handler)(struct interrupt_frame *)) {
  uint32_t addr = (uint32_t)(uintptr_t)handler;
  idt[vector] = (struct idt_gate){
      .offset_lo = addr & 0xffff,
      .selector = SEL_CODE,
      .zero = 0,
      .type = 0x8e,
      .offset_hi = addr >> 16,
  };
  struct {
    uint16_t limit;
    uint32_t base;
  } __attribute__((packed)) idtr = {sizeof idt - 1, (uint32_t)(uintptr_t)idt};
  asm volatile("lidt %0" : : "m"(idtr));
}

/* ------------------------------------------------------------ the 8259 PIC */

/* Program the PC's interrupt controller (the 8259 "PIC", emulated inside
 * KVM).  It takes a fixed sequence of four setup bytes, then a mask byte in
 * which a 1 bit blocks that interrupt line and a 0 bit lets it through.
 * 08-irqfd/guest.c explains each byte. */
static void pic_init(uint8_t vector_base, uint8_t mask) {
  outb(0x20, 0x11);        /* start setup; edge-triggered; 4th byte will follow */
  outb(0x21, vector_base); /* line N arrives as vector vector_base + N          */
  outb(0x21, 0x04);        /* a second PIC hangs off line 2 (PC standard)       */
  outb(0x21, 0x01);        /* 8086 mode                                         */
  outb(0x21, mask);        /* which lines are blocked                           */
}

/* ------------------------------------------------------------- the driver */

/* Sleep until at least `target` requests have completed.
 *
 * The obvious version, "while (sh->completed < target) hlt();", has a bug: a
 * "lost wake-up".  Suppose the device completes the request and sends its
 * interrupt just after we read `completed` but just before we `hlt`.  The
 * interrupt is handled, then we go to sleep anyway, and nothing will ever
 * wake us.
 *
 * The fix uses an x86 rule: `sti` (interrupts on) takes effect only after
 * the NEXT instruction.  So:
 *   cli()          interrupts off: none can sneak in while we check
 *   check          read `completed`
 *   sti; hlt       interrupts on, but only once hlt has started.  An interrupt
 *                  that arrived during the check was held back, and now wakes
 *                  us instead of being missed.
 * If sti and hlt were two separate asm statements, the compiler could put
 * other instructions between them and the guarantee would be gone. */
static void wait_for_completion(uint32_t target) {
  for (;;) {
    cli();
    if (sh->completed >= target) {
      sti();
      return;
    }
    sti_hlt();
  }
}

/* The host starts the vCPU here (guest.ld puts _start at 0x1000), with the
 * stack pointer already set.  It never returns: there's nothing to return to. */
__attribute__((section(".text.start"), noreturn)) void _start(void) {
  /* Set up interrupts: handler for vector 0x25, and the PIC delivering line 5
   * as vector 0x25 with every other line blocked (~(1 << 5) = 0xdf). */
  idt_install(IRQ_VECTOR, irq_handler);
  pic_init(0x20, (uint8_t)~(1u << IRQ_GSI));
  sti();

  uint32_t n = sh->n;

  marker(PORT_START);
  for (uint32_t id = 1; id <= n; id++) {
    /* KICK: "request number `id` is ready".  In the classic path, this store
     * stops the vCPU and returns to the host.  In the fast path, KVM just
     * bumps an eventfd and this loop carries straight on. */
    *doorbell = id;
    if (sh->mode == PINGPONG)
      wait_for_completion(id); /* one at a time: wait for this one first */
  }
  marker(PORT_SUBMITTED);

  wait_for_completion(n); /* wait for the last one */
  marker(PORT_ALLDONE);

  marker(PORT_EXIT);
  halt_forever(); /* not reached: the host stops running us */
}
