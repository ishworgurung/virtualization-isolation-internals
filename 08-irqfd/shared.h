/*
 * shared.h -- the contract between the host (irqfd_demo.c) and the guest
 * (guest.c).  Both files include it, so they agree on addresses, port
 * numbers and the layout of the memory they share.
 *
 * Guest memory map (64 KiB of RAM, guest physical addresses):
 *
 *   0x0500  GDT: 3 segment descriptors, written by the host  (GDT_GPA)
 *   0x1000  the guest program: code, then data               (GUEST_LOAD_GPA)
 *   0x8000  top of the guest's stack; it grows down          (GUEST_STACK_TOP)
 *   0x9000  struct shared: the host<->guest mailbox          (SHARED_GPA)
 *
 * The addresses must match guest.ld, which links the guest to run at 0x1000.
 */
#ifndef SHARED_H
#define SHARED_H

#include <stdint.h>

#define GUEST_MEM_SIZE 0x10000
#define GDT_GPA 0x0500
#define GUEST_LOAD_GPA 0x1000
#define GUEST_STACK_TOP 0x8000
#define SHARED_GPA 0x9000

/* Segment selectors: which GDT entry to use.  Entry n is at byte 8*n of the
 * GDT, so the selector is simply 8*n (entry 0 must be empty by x86 rules). */
#define SEL_CODE 0x08 /* GDT entry 1: flat 4 GiB code segment */
#define SEL_DATA 0x10 /* GDT entry 2: flat 4 GiB data segment */

#define IRQ_GSI 5                   /* ISA IRQ5 == GSI 5 on the default routing  */
#define IRQ_VECTOR (0x20 + IRQ_GSI) /* PIC base 0x20 + pin 5 = vector 0x25       */

#define PORT_DONE 0xf0    /* no in-kernel device claims it, so a write here  */
                          /* makes KVM_RUN return to the host: "I'm done"    */
#define PORT_SERIAL 0x3f8 /* COM1 serial port.  KVM has no in-kernel serial  */
                          /* port, so each byte written here exits to the    */
                          /* host, which prints it: a tiny console           */

/* The mailbox at SHARED_GPA.  The guest sees it at that address; the host
 * sees the same bytes at mem + SHARED_GPA.  Every field is written by one
 * side and read by the other, so both sides access it through a volatile
 * pointer (see the comment on `sh` in irqfd_demo.c). */
struct shared {
  uint32_t irq_count; /* guest's handler adds 1 per interrupt; host reads   */
  uint32_t stop;      /* host sets nonzero -> guest leaves its sleep loop   */
  uint32_t ready;     /* guest sets to 1 once the PIC + IDT are set up      */
};

#endif
