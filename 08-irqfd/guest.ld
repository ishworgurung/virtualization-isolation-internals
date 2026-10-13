/* guest.ld -- linker script for the guest program (guest.c).
 *
 * A linker script tells the linker where in memory each part of the program
 * will live, so it can fill in the right addresses.  Normal programs use a
 * default script; ours runs on a bare VM, so we say exactly what we want:
 *
 *   - everything starts at 0x1000 (GUEST_LOAD_GPA in shared.h), which is
 *     where the host copies guest.bin and starts the vCPU,
 *   - _start comes first (.text.start), so "start at 0x1000" means "start
 *     at _start",
 *   - then the rest of the code, read-only data (the hello string), data,
 *     and .bss (zero-filled variables: the IDT).  objcopy leaves .bss out of
 *     guest.bin; guest RAM is already zero, so it doesn't need copying.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x1000;
    .text   : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }

    /* Stack starts at 0x8000 and grows down: the program must end below it. */
    ASSERT(. <= 0x7000, "guest program too big: it would overlap the stack")

    /* Drop sections we don't need (compiler notes, unwind tables). */
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
