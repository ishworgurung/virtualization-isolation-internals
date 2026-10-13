/*
 * uring_bench.c -- "how many syscalls does it take?"  Three ways to perform N
 * small reads from /dev/zero, counting the syscalls each one really makes.
 *
 *   A. plain read(2), one syscall per operation
 *   B. io_uring, batches of 128 SQEs -> one io_uring_enter per batch
 *   C. io_uring + SQPOLL -> a kernel thread polls the SQ (Submission Queue) for
 *      new work; we eliminate the syscall that normally tells the kernel "check
 *      the SQ". Result: ~0 syscalls (the kernel proactively picks up work)
 *
 * The point is NOT that io_uring is always faster (reading /dev/zero is nearly
 * free, so syscall overhead dominates and exaggerates the gap) -- it is to make
 * the syscall arithmetic visible.  Prove the counts independently with:
 *
 *      strace -c -f -e trace=read,io_uring_enter ./uring_bench
 *
 * The "syscalls" column for A is simply N (one read() each); for B and C it is
 * the number of io_uring_enter() calls mini_uring.h actually made.
 *
 * SQPOLL note: the kernel thread burns a CPU while it is polling (`sq_idle_ms`
 * of idleness before it sleeps).  You trade a core for zero-syscall submission.
 * Unprivileged processes may use SQPOLL since Linux 5.11 (before that it
 * required CAP_SYS_ADMIN); if it is unavailable, part C is skipped.
 *
 * WARNING: SQPOLL is not always faster! Batching (B) often wins over SQPOLL (C)
 * because:
 *   - Well-batched syscalls (128 ops per syscall) amortize the syscall cost well
 *   - SQPOLL's kernel thread adds CPU overhead: polling, context switches, cache
 *     misses, potentially running on a different core
 *   - SQPOLL shines for latency (ultra-low-latency submission), not throughput
 *   - If you're already batching efficiently, SQPOLL's syscall savings may not
 *     outweigh the polling thread overhead
 *
 * Use SQPOLL when: you can't batch (high-frequency individual submissions),
 * latency matters more than throughput, or you're building a real-time system.
 *
 * BUILD & RUN:  make && ./uring_bench [N]
 */
#include "mini_uring.h"

#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>

#define IO_SIZE 64 /* bytes per read: tiny, so per-call overhead dominates */
#define BATCH 128  /* max SQEs we queue before each mu_submit()            */

/* Monotonic clock in milliseconds. */
static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/*
 * Run N reads through `r`, pumping the rings until all complete.
 *
 * Buffers: read i lands in slot (i % BATCH) of `buf`.  Several in-flight reads
 * may target the same slot; that would be a bug in a real program, but here
 * every read returns zeros and we never look at the data, only at cqe->res.
 *
 * The CQ (twice the SQ size) cannot overflow: we reap everything available on
 * every iteration, so it never holds more than the SQ plus one batch.  (Since
 * Linux 5.5, IORING_FEAT_NODROP would buffer overflowing CQEs anyway.)
 */
static void run_uring(struct mu_ring *r, int fd, unsigned long n, uint8_t *buf) {
  unsigned long issued = 0, completed = 0;

  while (completed < n) {
    /* (1) Fill as many SQEs as fit.  This is pure memory writes -- no syscall.
     */
    unsigned int queued = 0;
    struct io_uring_sqe *sqe;
    while (issued < n && queued < BATCH && (sqe = mu_get_sqe(r))) {
      /* offset 0: /dev/zero ignores the offset anyway; cookie = index */
      mu_prep_rw(sqe, IORING_OP_READ, fd, buf + (issued % BATCH) * IO_SIZE, IO_SIZE, 0, issued);
      issued++;
      queued++;
    }

    /* (2) Publish.  Plain ring: 1 syscall for the batch.  SQPOLL: usually 0. */
    mu_submit(r, 0);

    /* (3) Reap whatever finished -- again pure memory reads, no syscall. */
    unsigned reaped = 0;
    struct io_uring_cqe *cqe;
    while ((cqe = mu_peek_cqe(r))) {
      if (cqe->res != IO_SIZE) {
        fprintf(stderr, "bad cqe res=%d\n", cqe->res);
        exit(1);
      }
      mu_cqe_seen(r);
      reaped++;
    }
    completed += reaped;

    /* (4) Nothing to reap and nothing left to issue: block for completions
     *     (only the non-SQPOLL ring needs to; SQPOLL just polls the CQ). */
    if (!reaped && issued == n && !r->sqpoll) {
      mu_submit(r, 1);
    } else if (!reaped) {
      sched_yield(); /* SQPOLL: be polite while the kernel thread works */
    }
  }
}

int main(int argc, char **argv) {
  unsigned long n = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000000;
  int fd = open("/dev/zero", O_RDONLY);
  static uint8_t buf[BATCH * IO_SIZE]; /* static: 8 KiB, off the stack */

  printf("%lu reads of %d bytes from /dev/zero\n\n", n, IO_SIZE);
  printf("%-22s %10s %14s %12s\n", "method", "time(ms)", "ops/sec", "syscalls");

  /* ---- A. classic read(2) ------------------------------------------------ */
  double t0 = now_ms();
  for (unsigned long i = 0; i < n; i++)
    if (read(fd, buf, IO_SIZE) != IO_SIZE) {
      perror("read");
      return 1;
    }
  double dt = now_ms() - t0;
  printf("%-22s %10.1f %14.0f %12lu\n", "read() loop", dt, n / dt * 1e3, n);

  /* ---- B. io_uring, batched --------------------------------------------- */
  struct mu_ring r;
  int rc = mu_setup(&r, 256, 0, 0);
  if (rc < 0) {
    fprintf(stderr, "setup: %s\n", strerror(-rc));
    return 1;
  }
  t0 = now_ms();
  run_uring(&r, fd, n, buf);
  dt = now_ms() - t0;
  printf("%-22s %10.1f %14.0f %12lu\n", "io_uring batch=128", dt, n / dt * 1e3, r.n_enter);
  /* Closing the fd alone does not free the ring while its mmaps exist; fine
   * for a demo that exits soon.  mu_setup() below overwrites `r`. */
  close(r.fd);

  /* ---- C. io_uring + SQPOLL ---------------------------------------------- */
  rc = mu_setup(&r, 256, IORING_SETUP_SQPOLL, 200 /* ms idle before sleeping */);
  if (rc < 0) {
    printf("%-22s unavailable: %s\n", "io_uring SQPOLL", strerror(-rc));
    return 0;
  }
  t0 = now_ms();
  run_uring(&r, fd, n, buf);
  dt = now_ms() - t0;
  printf("%-22s %10.1f %14.0f %12lu\n", "io_uring SQPOLL", dt, n / dt * 1e3, r.n_enter);
  printf("\n(SQPOLL syscalls are only wake-ups of the kernel thread after it "
         "slept.)\n");
  return 0;
}
