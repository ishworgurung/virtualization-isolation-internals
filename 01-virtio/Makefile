# Build the virtio split-virtqueue demo (plain userspace C, no KVM needed).
#   -O2 -g          optimise, keep debug info
#   -Wall -Wextra   all the useful warnings
#   -std=gnu11      C11 (for <stdatomic.h>) + GNU extensions (__atomic builtins)
#   -pthread        the "device" runs on its own thread
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -pthread

# $@ = target name, $< = first prerequisite (the .c file).
virtio_demo: virtio_demo.c
	$(CC) $(CFLAGS) -o $@ $<

run: virtio_demo
	./virtio_demo

clean:
	rm -f virtio_demo
