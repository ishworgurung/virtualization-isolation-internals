/*
 * shared.h -- the contract between the host (kick_roundtrip.c) and the guest
 * (guest.c).  Both files include it, so they agree on addresses, port
 * numbers and the layout of the memory they share.
 *
 * Guest address map (guest physical addresses):
 *
 *   0x00500  GDT: 3 segment descriptors, written by the host  (GDT_GPA)
 *   0x01000  the guest program: code, then data               (GUEST_LOAD_GPA)
 *   0x08000  top of the guest's stack; it grows down          (GUEST_STACK_TOP)
 *   0x09000  struct shared: the host<->guest mailbox          (SHARED_GPA)
 *   0x0ffff  end of guest RAM (64 KiB)
 *   0xd0000  the device's DOORBELL.  Not RAM: nothing is there, so a write to
 *            it is caught by KVM (see MMIO_DOORBELL below)
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

/* The doorbell.  The guest "kicks" the device (says "I have work for you")
 * by storing a 32-bit number at this address.  It lies outside guest RAM, so
 * the store can't land in memory.  Instead, KVM either
 *   - returns from KVM_RUN with KVM_EXIT_MMIO ("the guest wrote to a device
 *     address, you deal with it": the CLASSIC path), or
 *   - just bumps an eventfd, if one was registered for this address with
 *     KVM_IOEVENTFD (the FAST path), and lets the guest carry on.
 * MMIO = "memory-mapped I/O": talking to a device through ordinary memory
 * reads and writes at special addresses. */
#define MMIO_DOORBELL 0xd0000

#define IRQ_GSI 5                   /* the device's interrupt line               */
#define IRQ_VECTOR (0x20 + IRQ_GSI) /* PIC base 0x20 + pin 5 = vector 0x25       */

/* Guest -> host "markers": the guest writes to these I/O ports at key moments.
 * No in-kernel device owns them, so each write makes KVM_RUN return to the host,
 * which records the time.  They cost one exit each, which is fine: they happen
 * three times per run, not once per request.
 * (NOT 0x20/0x21: those are the interrupt controller's ports, handled inside
 * KVM, so a write there would never reach the host.) */
#define PORT_START 0xe1     /* "I'm about to send the first request"    */
#define PORT_SUBMITTED 0xe2 /* "I've sent every request"                */
#define PORT_ALLDONE 0xe3   /* "every request has completed"           */
#define PORT_EXIT 0xf0      /* "I'm finished, stop running me"          */

/* How the guest issues its requests (host -> guest, via struct shared). */
enum mode {
  PIPELINED = 0, /* send all requests without waiting, then wait for all      */
  PINGPONG = 1,  /* send one, wait for it to complete, send the next, ...     */
};

/* The mailbox at SHARED_GPA.  The guest sees it at that address; the host
 * sees the same bytes at mem + SHARED_GPA.  Each field has one writer. */
struct shared {
  uint32_t irq_count; /* guest's interrupt handler adds 1 per interrupt     */
  uint32_t completed; /* device: how many requests are finished so far      */
  uint32_t mode;      /* host -> guest, before start: enum mode             */
  uint32_t n;         /* host -> guest, before start: how many requests     */
};

#endif
