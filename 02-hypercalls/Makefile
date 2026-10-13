# Build the minimal KVM VMM.
#   -O2 -g          optimise, keep debug info (so gdb/perf can map addresses)
#   -Wall -Wextra   all the useful warnings
#   -std=gnu11      C11 + GNU extensions (top-level __asm__, typeof, ...)
#   -no-pie         not required for correctness: the embedded guest code only
#                   uses *differences* of labels, which are position-independent.
#                   It just gives the host binary fixed addresses, which makes
#                   objdump/gdb output easier to follow.
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -no-pie

# $@ = the target (kvm_exits), $< = the first prerequisite (kvm_exits.c).
kvm_exits: kvm_exits.c
	$(CC) $(CFLAGS) -o $@ $<

# `make run` builds if needed, then runs. Needs read/write access to /dev/kvm.
run: kvm_exits
	./kvm_exits

clean:
	rm -f kvm_exits
