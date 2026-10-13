/*
 * guest.c -- the program that runs INSIDE the virtual machine.
 *
 * This is not a normal Linux program.  There is no operating system in the
 * guest: no libc, no printf, no malloc, no files.  It's just our code on a
 * bare (virtual) x86 CPU with 64 KiB of RAM.  That's called "freestanding"
 * C, and the Makefile builds it with -ffreestanding -nostdlib.
 *
 * How it gets into the VM:
 *   1. gcc -m32 compiles this file to 32-bit x86 code,
 *   2. guest.ld links it to run at address 0x1000, with _start first,
 *   3. objcopy strips the ELF wrapping, leaving guest.bin: raw machine code
 *      that starts with _start's first instruction,
 *   4. irqfd_demo.c pulls guest.bin in with #embed, copies it to guest RAM
 *      at 0x1000, and points the vCPU there.
 *
 * The CPU mode: the host starts the vCPU in 32-bit "protected mode" with
 * flat segments, so every address in this file is a plain physical address
 * from 0 to 4 GiB (we only have 64 KiB of it).  There's no paging, so no
 * virtual memory: pointer value == guest physical address.
 *
 * What it does (see irqfd_demo.c for the why):
 *   1. prints a hello message on the serial port,
 *   2. installs irq5_handler in the interrupt table (IDT),
 *   3. programs the 8259 interrupt controller (PIC) to deliver IRQ 5 as
 *      vector 0x25,
 *   4. tells the host it's ready, then sleeps in HLT until the host sets
 *      `stop`.  Every interrupt wakes it, runs the handler, which counts it,
 *   5. writes to PORT_DONE so the host knows it has finished.
 */
#include "shared.h"
#include <stdint.h>

/* The mailbox shared with the host (layout in shared.h).
 * `volatile`: the host changes `stop` behind the compiler's back, so every
 * read must really go to memory rather than use a remembered value. */
static volatile struct shared *const sh = (volatile struct shared *)SHARED_GPA;

/* ------------------------------------------------------------------------
 * The only assembly in the guest: one-instruction helpers.
 *
 * These CPU instructions have no C equivalent, so each one is wrapped in a
 * tiny inline-asm function.  (The Linux kernel does exactly the same, in
 * arch/x86/include/asm/io.h and irqflags.h.)  After this block the rest of
 * the file is plain C.
 *
 * Reading the syntax, e.g. asm volatile("outb %0, %1" : : "a"(val), "Nd"(port)):
 *   - "outb %0, %1"   the instruction, with %0/%1 standing for the operands
 *   - "a"(val)        put val in register al first (out requires al)
 *   - "Nd"(port)      port as a constant (if < 256) or in register dx
 *   - volatile        never delete or move this: it has side effects
 *   - "memory"        (on some) the compiler must not keep memory values
 *                     in registers across this, nor move memory accesses
 *                     past it
 * ------------------------------------------------------------------------ */

/* Write one byte to an I/O port.  I/O ports are a small, separate address
 * space x86 uses to talk to devices: the PIC is at 0x20/0x21, the serial
 * port at 0x3f8, and so on. */
static inline void outb(uint16_t port, uint8_t val) { asm volatile("outb %0, %1" : : "a"(val), "Nd"(port)); }

/* Turn interrupts off ("clear interrupt flag"). */
static inline void cli(void) { asm volatile("cli" : : : "memory"); }

/* Turn interrupts on AND sleep until one arrives, as one unit.  This must be
 * a single asm statement, see the sleep loop in _start for why. */
static inline void sti_hlt(void) { asm volatile("sti; hlt" : : : "memory"); }

/* Sleep forever (with interrupts off nothing can wake us). */
static inline __attribute__((noreturn)) void halt_forever(void) {
  for (;;)
    asm volatile("cli; hlt");
}

/* ------------------------------------------------------------ serial port */

/* Print a string, one byte at a time, on COM1.  This is how an OS's very
 * first "printf" usually works: there's no screen driver yet, but a byte
 * written to the serial port's I/O port goes out on the wire.  Here, "the
 * wire" is the host's vCPU thread: each outb exits to it, and it prints the
 * byte. */
static void serial_puts(const char *s) {
  for (; *s; s++)
    outb(PORT_SERIAL, (uint8_t)*s);
}

/* ------------------------------------------------ interrupt handler (IDT) */

/* When an interrupt arrives, the CPU looks up its vector number in the IDT
 * (Interrupt Descriptor Table): an array of 256 8-byte "gates", each saying
 * where that vector's handler is.  The layout is fixed by the CPU: */
struct idt_gate {
  uint16_t offset_lo;      /* handler address, low 16 bits                        */
  uint16_t selector;       /* code segment to run it in (SEL_CODE)                */
  uint8_t zero;            /* unused, must be 0                                   */
  uint8_t type;            /* 0x8e = present, kernel privilege, 32-bit interrupt  */
                           /*        gate (the CPU turns interrupts off on entry) */
  uint16_t offset_hi;      /* handler address, high 16 bits                       */
} __attribute__((packed)); /* packed: no padding, exactly the CPU's layout */

/* The IDT itself.  It's in .bss, which the host never copies, so it isn't
 * in guest.bin.  That's fine: guest RAM starts out all zeros (the host
 * mmap()s it), and an all-zero gate means "not present". */
static struct idt_gate idt[256];

/* GCC generates the handler's entry and exit code for us.  An interrupt can
 * arrive between any two instructions, so the handler must leave every
 * register exactly as it found it and return with `iret` (which restores the
 * interrupted code's EIP, CS and EFLAGS) instead of `ret`.
 * __attribute__((interrupt)) does all of that.  It requires the handler to
 * take a pointer to the CPU-pushed frame, even if we don't use it, and the
 * Makefile's -mgeneral-regs-only, so the compiler never touches
 * floating-point/SSE registers it wouldn't save. */
struct interrupt_frame; /* the CPU-pushed EIP/CS/EFLAGS; we don't look inside */

__attribute__((interrupt)) static void irq5_handler(struct interrupt_frame *frame) {
  (void)frame;
  sh->irq_count++; /* the host watches this to see the interrupt arrive */

  /* EOI = "End Of Interrupt": tell the PIC "handled".  The PIC blocks
   * further interrupts on this pin (and lower-priority ones) until it gets
   * one.  Forget it, and you'd see exactly one interrupt and then silence.
   * (Like every PIC access here, handled inside KVM: no exit.) */
  outb(0x20, 0x20);
}

/* Point IDT entry `vector` at `handler`. */
static void idt_set(uint8_t vector, void (*handler)(struct interrupt_frame *)) {
  uint32_t addr = (uint32_t)(uintptr_t)handler;
  idt[vector] = (struct idt_gate){
      .offset_lo = addr & 0xffff,
      .selector = SEL_CODE,
      .zero = 0,
      .type = 0x8e,
      .offset_hi = addr >> 16,
  };
}

/* Tell the CPU where the IDT is.  `lidt` takes a 6-byte operand: the table's
 * size minus 1, then its address. */
static void idt_load(void) {
  struct {
    uint16_t limit;
    uint32_t base;
  } __attribute__((packed)) idtr = {sizeof idt - 1, (uint32_t)(uintptr_t)idt};
  asm volatile("lidt %0" : : "m"(idtr));
}

/* ------------------------------------------------------------ the 8259 PIC */

/* Program the master 8259 PIC: port 0x20 is its "command" register and 0x21
 * its "data" register.  Initialising it is a fixed ritual of four bytes,
 * called Initialisation Command Words (ICW1-ICW4), which it expects in this
 * exact order.  After that, a byte written to 0x21 is the interrupt mask
 * ("OCW1"): a 1 bit blocks that pin, a 0 bit lets it through. */
static void pic_init(uint8_t vector_base, uint8_t mask) {
  outb(0x20, 0x11);        /* ICW1: start init; edge-triggered; ICW4 will follow */
  outb(0x21, vector_base); /* ICW2: pin N arrives as vector vector_base + N      */
  outb(0x21, 0x04);        /* ICW3: a second (slave) PIC hangs off pin 2.  The   */
                           /*       PC standard; we don't use it                 */
  outb(0x21, 0x01);        /* ICW4: 8086 mode (as opposed to the older 8080)     */
  outb(0x21, mask);        /* OCW1: which pins are blocked                       */
}

/* ------------------------------------------------------------ entry point */

/* The host starts the vCPU here, with the stack pointer already at
 * GUEST_STACK_TOP.  The section attribute puts _start in .text.start, which
 * guest.ld places first, so _start is at exactly 0x1000.  It never returns:
 * there is nothing to return to. */
__attribute__((section(".text.start"), noreturn)) void _start(void) {
  serial_puts("hello from the guest! (via OUT to port 0x3f8)\n");

  idt_set(IRQ_VECTOR, irq5_handler);
  idt_load();
  /* Vectors start at 0x20, so IRQ 5 arrives as 0x25.  Mask every pin except
   * 5: ~(1 << 5) = 1101 1111b = 0xdf. */
  pic_init(0x20, (uint8_t)~(1u << IRQ_GSI));

  /* From here on, interrupts can arrive (inside sti_hlt below). */
  sh->ready = 1;

  /* Sleep until the host sets `stop`.  Each interrupt wakes us, the handler
   * runs, and we loop round and sleep again.
   *
   * Why cli first, and why sti_hlt must be ONE asm statement: it avoids a
   * "lost wake-up".  Imagine a simpler loop, "while (!stop) hlt();", with
   * interrupts always on.  Suppose the host sets stop and sends its wake-up
   * interrupt right after we read stop but just before hlt.  The handler
   * runs, returns, and then we hlt anyway, with nobody left to wake us.  We'd
   * sleep forever.
   *
   * The fix uses an x86 guarantee: `sti` takes effect only after the NEXT
   * instruction.  So:
   *   cli()       interrupts off: nothing can sneak in during the check
   *   sh->stop    check it
   *   sti; hlt    interrupts on, but only once hlt has started.  An
   *               interrupt that arrived during the check was held back
   *               ("pending"), and now wakes us instead of being missed.
   * If sti and hlt were two separate asm statements, the compiler would be
   * free to put other instructions between them, and the guarantee would be
   * gone. */
  for (;;) {
    cli();
    if (sh->stop)
      break;
    sti_hlt();
  }

  serial_puts("goodbye from the guest!\n");
  /* Say goodbye.  No in-kernel device owns PORT_DONE, so KVM has to hand
   * this write to the host: KVM_RUN returns with KVM_EXIT_IO.  Apart from the
   * serial bytes above, this is the only exit of the whole run. */
  outb(PORT_DONE, 0);
  halt_forever(); /* not reached: the host stops running us */
}
