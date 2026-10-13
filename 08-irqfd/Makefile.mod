# Build the KVM_IRQFD demo (a tiny KVM VMM; needs read/write access to /dev/kvm).
#   -std=gnu11  C11 + GNU extensions (the top-level __asm__ holds the guest code)
#   -no-pie     not required (the guest blob only uses label differences); it
#               just keeps host addresses fixed for easier objdump/gdb reading
#   -pthread    the vCPU runs on its own thread while main() injects interrupts
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -no-pie -pthread

# $@ = target name, $< = first prerequisite (the .c file).
irqfd_demo: irqfd_demo.c
	$(CC) $(CFLAGS) -o $@ $<

irqfd_demo_pure_assembly_guest: irqfd_demo_pure_assembly_guest.c
	$(CC) $(CFLAGS) -o $@ $<

run: irqfd_demo
	./irqfd_demo

clean:
	rm -f irqfd_demo irqfd_demo_pure_assembly_guest
