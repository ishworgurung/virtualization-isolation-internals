/*
 * uring_cqpoll.c -- zero-syscall io_uring: SQPOLL + busy-polling the CQ from
 * userspace.
 *
 * There is NO "CQPOLL" setup flag.  None is needed: the CQ ring is always
 * mmap()ed into our process, so "polling the CQ" just means reading cq->tail
 * in a loop (io_uring_peek_cqe / io_uring_for_each_cqe).  The only reason a
 * normal program makes a syscall for completions is to SLEEP until one
 * arrives (io_uring_enter with IORING_ENTER_GETEVENTS).  Spin instead of
 * sleeping and that syscall disappears.
 *
 *   A. normal ring       submit+wait: one io_uring_enter per batch
 *   B. SQPOLL + wait     kernel thread picks up SQEs; we still sleep in
 *                        io_uring_enter(GETEVENTS) for completions
 *   C. SQPOLL + CQ spin  kernel thread picks up SQEs; we spin on the CQ tail.
 *                        ~0 syscalls (only IORING_SQ_NEED_WAKEUP wake-ups)
 *
 * The price of C: two busy CPUs (the SQPOLL kernel thread and our spinning
 * thread).  You buy latency with cores.
 *
 * Not to be confused with IORING_SETUP_IOPOLL: that makes the KERNEL poll the
 * device for completions instead of waiting for an interrupt.  It only works
 * for O_DIRECT I/O on block devices with poll queues (e.g. NVMe loaded with
 * nvme.poll_queues=N); /dev/zero would fail every request with -EOPNOTSUPP.
 *
 * Verify syscall counts:  strace -c -f -e trace=io_uring_enter ./uring_cqpoll
 *
 * BUILD & RUN:  make && ./uring_cqpoll [N]
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IO_SIZE 64 /* tiny reads: per-call overhead dominates */
#define BATCH 128  /* SQEs queued before each submit */

enum mode { NORMAL, SQPOLL_WAIT, SQPOLL_CQ_SPIN };

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Consume every CQE currently in the ring: pure memory reads, no syscall. */
static unsigned reap(struct io_uring *ring) {
  struct io_uring_cqe *cqe;
  unsigned head, n = 0;
  io_uring_for_each_cqe(ring, head, cqe) {
    if (cqe->res != IO_SIZE) {
      fprintf(stderr, "bad cqe res=%d\n", cqe->res);
      exit(1);
    }
    n++;
  }
  io_uring_cq_advance(ring, n); /* publish new cq head in one store */
  return n;
}

static void run(const char *name, enum mode m, int fd, unsigned long n, char *buf) {
  struct io_uring ring;
  struct io_uring_params p = {0};
  if (m != NORMAL) {
    p.flags = IORING_SETUP_SQPOLL;
    p.sq_thread_idle = 200; /* ms the kernel thread spins before sleeping */
  }
  int ret = io_uring_queue_init_params(256, &ring, &p);
  if (ret < 0) {
    printf("%-22s unavailable: %s\n", name, strerror(-ret));
    return;
  }

  unsigned long issued = 0, done = 0;
  double t0 = now_ms();
  while (done < n) {
    struct io_uring_sqe *sqe;
    unsigned q = 0;
    /* buf is shared by every read: the data is all zeros and never looked at */
    while (issued < n && q < BATCH && (sqe = io_uring_get_sqe(&ring))) {
      io_uring_prep_read(sqe, fd, buf, IO_SIZE, 0);
      issued++;
      q++;
    }

    struct io_uring_cqe *cqe;
    switch (m) {
    case NORMAL: /* one syscall: submit the batch and sleep for >= 1 CQE */
      ret = io_uring_submit_and_wait(&ring, 1);
      break;
    case SQPOLL_WAIT: /* submit is free; waiting still costs a syscall */
      io_uring_submit(&ring);
      ret = io_uring_wait_cqe(&ring, &cqe);
      break;
    case SQPOLL_CQ_SPIN: /* submit is free; reap() below is the "wait" */
      ret = io_uring_submit(&ring);
      break;
    }
    if (ret < 0) {
      fprintf(stderr, "%s: %s\n", name, strerror(-ret));
      exit(1);
    }
    done += reap(&ring);
  }
  double dt = now_ms() - t0;
  printf("%-22s %10.1f %14.0f\n", name, dt, n / dt * 1e3);
  io_uring_queue_exit(&ring);
}

int main(int argc, char **argv) {
  unsigned long n = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000000;
  static char buf[IO_SIZE];
  int fd = open("/dev/zero", O_RDONLY);
  if (fd < 0) {
    perror("open /dev/zero");
    return 1;
  }

  printf("%lu reads of %d bytes from /dev/zero\n\n", n, IO_SIZE);
  printf("%-22s %10s %14s\n", "method", "time(ms)", "ops/sec");
  run("A. normal", NORMAL, fd, n, buf);
  run("B. SQPOLL + wait", SQPOLL_WAIT, fd, n, buf);
  run("C. SQPOLL + CQ spin", SQPOLL_CQ_SPIN, fd, n, buf);
  return 0;
}
