# Build the kick round-trip benchmark: a tiny KVM VMM (kick_roundtrip, runs on
# the host) plus the "driver" it runs inside the VM (guest.c -> guest.bin).
# Running it needs read/write access to /dev/kvm.
#
# Build order:
#   guest.c --gcc--> guest.elf --objcopy--> guest.bin --#embed--> kick_roundtrip
#
# $@ = target name, $< = first prerequisite.

# Host program.
#   -std=gnu23  for #embed, which pastes guest.bin's bytes into an array
#   -pthread    the emulated device runs on its own thread
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu23 -pthread

# Guest program: bare-metal 32-bit x86, no OS underneath it.
#   -m32                     32-bit code (the host starts the vCPU in 32-bit mode)
#   -ffreestanding           no libc; don't assume a normal program environment
#   -nostdlib                don't link libc or startup files (we have _start)
#   -fno-pie                 fixed addresses; guest.ld says where things go
#   -mgeneral-regs-only      no floating point/SSE: needed by the interrupt
#                            handler (__attribute__((interrupt)))
#   -fno-stack-protector     the stack canary needs runtime support we lack
#   -fcf-protection=none     skip endbr32 instructions (CET), just noise here
#   -fno-asynchronous-unwind-tables   no .eh_frame tables (no exceptions here)
GUEST_CFLAGS = -m32 -Os -Wall -Wextra -std=gnu11 -ffreestanding -nostdlib -fno-pie -no-pie \
               -mgeneral-regs-only -fno-stack-protector -fcf-protection=none \
               -fno-asynchronous-unwind-tables

kick_roundtrip: kick_roundtrip.c shared.h guest.bin
	$(CC) $(CFLAGS) -o $@ $<

# Link with our own script (addresses, _start first), then keep only the raw
# bytes: `objcopy -O binary` writes the program exactly as it should sit in
# guest memory from 0x1000 on, with no ELF headers.
guest.bin: guest.c shared.h guest.ld
	$(CC) $(GUEST_CFLAGS) -T guest.ld -Wl,--build-id=none,--no-warn-rwx-segments -o guest.elf $<
	objcopy -O binary guest.elf $@

# Optional arguments: ./kick_roundtrip <requests> <device-work-us>
run: kick_roundtrip
	./kick_roundtrip

# Disassemble the guest: see what the C compiled to.
disasm: guest.bin
	objdump -d guest.elf

clean:
	rm -f kick_roundtrip guest.elf guest.bin

.PHONY: run disasm clean
