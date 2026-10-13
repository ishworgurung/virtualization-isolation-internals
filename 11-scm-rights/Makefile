# Build the SCM_RIGHTS fd-passing demo.  Plain Linux, no special access needed.
#   -std=gnu11  C11 + GNU extensions (_GNU_SOURCE for MSG_CMSG_CLOEXEC)
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11

# $@ = target name, $< = first prerequisite (the .c file).
fd_pass: fd_pass.c
	$(CC) $(CFLAGS) -o $@ $<

run: fd_pass
	./fd_pass

clean:
	rm -f fd_pass

.PHONY: run clean
