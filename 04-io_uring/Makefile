# Build the io_uring programs.
#
# uring_basic, uring_bench, uring_seccomp: use mini_uring.h (direct kernel ABI).
# uring_cqpoll: uses liburing (convenient high-level API).
#
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
BASIC_BINS = uring_basic uring_bench uring_seccomp
LIBURING_BINS = uring_cqpoll
BINS = $(BASIC_BINS) $(LIBURING_BINS)

all: $(BINS)

# Pattern rule: mini_uring.h-based programs.
# Listing mini_uring.h as a prerequisite makes every program rebuild when it changes.
$(BASIC_BINS): %: %.c mini_uring.h
	$(CC) $(CFLAGS) -o $@ $<

# liburing-based programs. Link with -luring.
$(LIBURING_BINS): %: %.c
	$(CC) $(CFLAGS) -o $@ $< -luring

# `&&` stops at the first program that fails.
run: all
	./uring_basic && ./uring_bench && ./uring_seccomp && ./uring_cqpoll

clean:
	rm -f $(BINS)
