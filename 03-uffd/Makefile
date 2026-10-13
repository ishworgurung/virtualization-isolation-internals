# Build the userfaultfd demo.
#   -std=gnu11  C11 atomics + GNU extensions;  -pthread  the fault handler is a thread.
# Runs unprivileged on Linux >= 5.11 (it uses UFFD_USER_MODE_ONLY).
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11 -pthread

# $@ = target name, $< = first prerequisite (the .c file).
uffd_demo: uffd_demo.c
	$(CC) $(CFLAGS) -o $@ $<

# Optional size argument: ./uffd_demo 256   (snapshot size in MiB)
run: uffd_demo
	./uffd_demo

clean:
	rm -f uffd_demo
