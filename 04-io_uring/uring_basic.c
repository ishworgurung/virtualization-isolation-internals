/*
 * uring_basic.c -- a guided tour of one io_uring, using mini_uring.h.
 *
 *   1. NOP           : the round trip with no I/O -- see SQE in, CQE out.
 *   2. Linked ops    : WRITE then READ of the same file, submitted in ONE
 *                      syscall, ordered by IOSQE_IO_LINK.
 *   3. Error report  : failures come back as negative errno in cqe->res
 *                      (io_uring never sets errno for the operation itself).
 *   4. Batching      : fill the whole SQ, observe "ring full", submit all of
 *                      it with one syscall.
 *
 * The pattern in every section is the same three steps:
 *   get an SQE  ->  fill it in (plain memory writes, no syscall)
 *   mu_submit() ->  publish the tail; at most one io_uring_enter()
 *   drain()     ->  read CQEs from shared memory (no syscall)
 *
 * BUILD & RUN:  make && ./uring_basic
 */
#include "mini_uring.h"

#include <fcntl.h>
#include <stdlib.h>

/* Print and consume every currently-available completion.  Returns how many
 * were consumed.  For errors, res is -errno, so strerror(-res) names it. */
static int drain(struct mu_ring *r, const char *label) {
  int n = 0;
  struct io_uring_cqe *cqe;
  while ((cqe = mu_peek_cqe(r))) {
    printf("  CQE[%s]: user_data=%llu res=%d%s%s\n", label,
           (unsigned long long)cqe->user_data, cqe->res,
           cqe->res < 0 ? "  = -" : "",
           cqe->res < 0 ? strerror(-cqe->res) : "");
    mu_cqe_seen(r);
    n++;
  }
  return n;
}

int main(void) {
  struct mu_ring r;
  int rc = mu_setup(&r, 8, 0, 0); /* 8 SQ entries, no special flags */
  if (rc < 0) {
    fprintf(stderr,
            "io_uring_setup failed: %s (io_uring disabled? see "
            "/proc/sys/kernel/io_uring_disabled)\n",
            strerror(-rc));
    return 1;
  }
  printf("ring fd=%d: SQ has %u slots, mask=0x%x\n\n", r.fd, r.sq_entries,
         r.sq_mask);

  /* ---------------------------------------------------------------- 1. NOP */
  /* IORING_OP_NOP does nothing and completes with res = 0.  It measures and
   * demonstrates the pure mechanics of the rings. */
  printf("1) NOP round trip\n");
  struct io_uring_sqe *sqe = mu_get_sqe(&r);
  sqe->opcode = IORING_OP_NOP;
  sqe->user_data = 42; /* our cookie */
  /* Submit 1 SQE and wait for 1 CQE in a SINGLE syscall. */
  mu_submit(&r, 1);
  drain(&r, "nop");

  /* ------------------------------------------------- 2. linked write + read */
  printf("\n2) WRITE -> READ on the same file, linked, ONE io_uring_enter()\n");
  /* A private temp file: mkstemp creates it, unlink removes its name
   * immediately; the open fd keeps the (now anonymous) file alive. */
  char path[] = "/tmp/uring_basic.XXXXXX";
  int fd = mkstemp(path);
  unlink(path);
  static const char msg[] = "hello from io_uring";
  char back[64] = {0}; /* zeroed, and we read at most 63 bytes, so the
                          result is always NUL-terminated */

  sqe = mu_get_sqe(&r);
  /* write strlen(msg) bytes at file offset 0; cookie 100 */
  mu_prep_rw(sqe, IORING_OP_WRITE, fd, (void *)msg, sizeof msg - 1, 0, 100);
  /* IOSQE_IO_LINK: do not START the next SQE until this one completes OK.
   * If this one fails, the next completes with -ECANCELED.  io_uring treats
   * any unexpected result as failure here, including a short write.
   * Without the link, the two requests could run in either order (or
   * concurrently) and the read might see an empty file. */
  sqe->flags |= IOSQE_IO_LINK;

  sqe = mu_get_sqe(&r);
  /* read up to 63 bytes from offset 0; cookie 101 */
  mu_prep_rw(sqe, IORING_OP_READ, fd, back, sizeof back - 1, 0, 101);

  unsigned long before = r.n_enter;
  mu_submit(&r, 2); /* submit both, wait for both */
  printf("  syscalls used for 2 operations: %lu\n", r.n_enter - before);
  drain(&r, "wr/rd");
  printf("  read back: \"%s\"\n", back);

  /* ------------------------------------------------------- 3. error handling
   */
  printf("\n3) Errors are negative errno in cqe->res\n");
  sqe = mu_get_sqe(&r);
  /* fd 999 is not open: the CQE carries -EBADF (-9).  Note that
   * mu_submit() itself still succeeds -- the SUBMISSION was fine, the
   * OPERATION failed. */
  mu_prep_rw(sqe, IORING_OP_READ, /* bad fd */ 999, back, 8, 0, 200);
  mu_submit(&r, 1);
  drain(&r, "badfd");

  /* ----------------------------------------------------- 4. batching a lot */
  printf("\n4) 8 NOPs (the whole SQ) submitted with one syscall\n");
  for (int i = 0; i < 8; i++) {
    sqe = mu_get_sqe(&r);
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = 1000 + i;
  }
  /* A 9th get_sqe would fail: the ring is full until the kernel consumes. */
  printf("  9th mu_get_sqe() while full -> %s\n",
         mu_get_sqe(&r) ? "got one (?!)" : "NULL (ring full)");
  before = r.n_enter;
  mu_submit(&r, 8);
  printf("  syscalls for 8 ops: %lu\n", r.n_enter - before);
  printf("  reaped %d completions\n", drain(&r, "batch"));

  printf("\nTotal io_uring_enter calls this run: %lu\n", r.n_enter);
  return 0;
}
