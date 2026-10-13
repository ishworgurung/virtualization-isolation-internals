# Build the KVM_IOEVENTFD demo (a tiny KVM VMM; needs read/write access to /dev/kvm).
#   -std=gnu11  C11 + GNU extensions (the top-level __asm__ holds the guest code)
#   -no-pie     not required (the guest blob only uses label differences); it
#               just keeps host addresses fixed for easier objdump/gdb reading
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -no-pie

# $@ = target name, $< = first prerequisite (the .c file).
ioeventfd_demo: ioeventfd_demo.c
	$(CC) $(CFLAGS) -o $@ $<

# No strace needed: the demo counts and prints its own exit statistics.
run: ioeventfd_demo
	./ioeventfd_demo

clean:
	rm -f ioeventfd_demo
