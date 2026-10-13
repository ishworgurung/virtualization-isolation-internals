/*
 * mini_uring.h -- a ~150-line io_uring "library" written directly against the
 *                 kernel ABI, no liburing.  Read this file first: it contains
 *                 every fact about how io_uring's shared rings work.
 *
 * WHY io_uring EXISTS
 * -------------------
 * Classic Linux I/O is one system call per operation: read(), write(),
 * send()...  A system call is a switch from user mode into the kernel and back;
 * it costs tens to hundreds of nanoseconds (more with CPU-vulnerability
 * mitigations), and a busy server does millions per second.  io_uring replaces
 * "call the kernel per operation" with "leave requests in shared memory and let
 * the kernel pick them up", so many operations cost one system call -- or none.
 *
 * THE BIG PICTURE
 * ---------------
 * io_uring gives you two lock-free ring buffers that are mmap()ed into BOTH the
 * kernel and your process:
 *
 *      SQ (submission queue)                     CQ (completion queue)
 *      you PRODUCE, kernel CONSUMES              kernel PRODUCES, you CONSUME
 *
 *      user:  write SQE, tail++  ------------>   kernel: reads SQE, does I/O
 *      user:  head++  <----------------------    kernel: writes CQE, tail++
 *
 *   SQE = "submission queue entry": one request (opcode, fd, buffer, length,
 *         offset, flags, and a 64-bit `user_data` cookie).  64 bytes.
 *   CQE = "completion queue entry": one result {user_data, res, flags}.
 *         16 bytes.  `res` is what the equivalent syscall would have returned,
 *         except that errors are a NEGATIVE errno (e.g. -9 = -EBADF).
 *   Completions may arrive in a different order than submissions; the
 *   user_data cookie is how you match them up.
 *
 * Both are single-producer/single-consumer rings using free-running 32-bit
 * head/tail counters (index = counter & mask).  This is the very same design as
 * a virtio virtqueue (01-virtio/) and the SPSC queue in 05-lockfree/.
 * "Free-running" means the counters only ever increase (wrapping at 2^32);
 * tail - head is the number of occupied slots, correct even across the wrap
 * because unsigned subtraction is modular.
 *
 * MEMORY LAYOUT after setup:
 *   ring #1 (mmap offset IORING_OFF_SQ_RING):  sq head, tail, mask, flags,
 * array[] ring #2 (mmap offset IORING_OFF_CQ_RING):  cq head, tail, mask,
 * cqes[] (with IORING_FEAT_SINGLE_MMAP these two are ONE mapping) SQE array
 * (offset IORING_OFF_SQES):        the actual 64-byte request structs The SQ
 * ring itself only holds *indices* into the SQE array (sq_array[]), which lets
 * you submit requests in a different order than you filled them. (Linux >= 6.6
 * can drop that indirection with IORING_SETUP_NO_SQARRAY.) The mmap "offsets"
 * are not file positions; they are magic values that tell the kernel WHICH of
 * the ring's memory regions to map.
 *
 * ORDERING RULES (the contract you must honour):
 *   - Fill the SQE completely, THEN publish it with a RELEASE store to sq tail.
 *   - Read cq tail with ACQUIRE, THEN read the CQEs, THEN RELEASE-store cq
 * head.
 *   - Read sq head with ACQUIRE to learn how much SQ space the kernel freed.
 * (RELEASE store: all my earlier memory writes become visible no later than
 *  this one.  ACQUIRE load: if I see a value, I also see everything written
 *  before its RELEASE store.  05-lockfree explains and tests this.)
 */
#ifndef MINI_URING_H
#define MINI_URING_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <linux/io_uring.h> /* kernel ABI: structs, opcodes, flags */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h> /* __NR_io_uring_* syscall numbers */
#include <unistd.h>

/* Everything we need to drive one ring.  Pointers point INTO the shared
 * mappings (the kernel reads/writes those words concurrently); plain integers
 * are private copies of values that never change after setup. */
struct mu_ring {
  int fd;     /* the ring's file descriptor                */
  int sqpoll; /* 1 if a kernel thread polls the SQ for us  */

  /* --- SQ ring (kernel-shared) --- */
  uint32_t *sq_head;            /* kernel advances: "I consumed up to here"  */
  uint32_t *sq_tail;            /* WE advance:      "I published up to here" */
  uint32_t *sq_flags;           /* kernel sets IORING_SQ_NEED_WAKEUP (SQPOLL)*/
  uint32_t *sq_array;           /* slot -> index into sqes[]                 */
  uint32_t sq_mask, sq_entries; /* mask = entries - 1 (entries is 2^k)    */
  struct io_uring_sqe *sqes;    /* the SQE array (separate mapping)          */

  /* --- CQ ring (kernel-shared) --- */
  uint32_t *cq_head; /* WE advance:      "I consumed up to here"  */
  uint32_t *cq_tail; /* kernel advances: "completed up to here"   */
  uint32_t cq_mask;
  struct io_uring_cqe *cqes;

  uint32_t local_tail;   /* our private, not-yet-published tail       */
  unsigned long n_enter; /* how many io_uring_enter() syscalls we made */
};

/* The three io_uring syscalls (glibc has no wrappers).
 *   io_uring_setup(entries, params)  -> creates a ring, returns its fd
 *   io_uring_enter(fd, to_submit, min_complete, flags, sigmask, sigsz)
 *                                    -> submit and/or wait for completions
 *   io_uring_register(fd, opcode, arg, nr_args)
 *                                    -> register buffers/files/restrictions...
 */
static inline int mu_sys_setup(unsigned entries, struct io_uring_params *p) { return syscall(__NR_io_uring_setup, entries, p); }

static inline int mu_sys_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags) { return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, NULL, 0); }

static inline int mu_register(struct mu_ring *r, unsigned opcode, void *arg, unsigned nr) { return syscall(__NR_io_uring_register, r->fd, opcode, arg, nr); }

/*
 * mu_setup -- create a ring and map it.  Returns 0 or -errno.
 *   entries      : requested SQ size (the kernel rounds up to a power of two;
 *                  the CQ gets twice as many entries by default)
 *   flags        : IORING_SETUP_* (e.g. IORING_SETUP_SQPOLL,
 * IORING_SETUP_R_DISABLED) sq_idle_ms   : for SQPOLL, how long the kernel
 * thread spins before sleeping
 *
 * The mappings are never unmapped; this demo relies on process exit for that.
 */
static inline int mu_setup(struct mu_ring *r, unsigned entries, unsigned flags, unsigned sq_idle_ms) {
  struct io_uring_params p;
  memset(&p, 0, sizeof p); /* unused fields MUST be zero or setup fails */
  p.flags = flags;
  p.sq_thread_idle = sq_idle_ms;

  memset(r, 0, sizeof *r);
  r->fd = mu_sys_setup(entries, &p);
  if (r->fd < 0) {
    return -errno;
  }
  /* Extract SQPOLL flag and normalize to boolean (0 or 1).
   * flags & IORING_SETUP_SQPOLL returns 0 or the raw bit value; the cast ensures
   * a clean boolean for the bool-typed sqpoll field. */
  r->sqpoll = (bool)(flags & IORING_SETUP_SQPOLL);

  /* The kernel tells us (in p.sq_off / p.cq_off) where each field lives
   * inside the mapping.  Sizes: header fields + the array.  Reading the
   * layout from the kernel instead of hard-coding it is what keeps the ABI
   * extensible.
   *
   * SQ ring holds an array of uint32_t indices, not actual SQEs. The SQEs
   * themselves are allocated separately by the application. The kernel just
   * needs indices to track submissions.
   *
   * CQ ring holds full io_uring_cqe structs with results (return value, user
   * data, flags). The kernel writes completion results here. */
  size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
  size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

  /* IORING_FEAT_SINGLE_MMAP (Linux 5.4+): SQ and CQ rings share one mapping.
   * Supporting older kernels would just mean a second mmap at
   * IORING_OFF_CQ_RING; we skip that to stay short. */
  if (!(p.features & IORING_FEAT_SINGLE_MMAP)) {
    fprintf(stderr, "kernel too old (no SINGLE_MMAP)\n");
    return -ENOSYS;
  }
  /* SINGLE_MMAP: one mapping holds both rings; make it big enough for both. */
  if (cq_sz > sq_sz)
    sq_sz = cq_sz;

  /* MAP_SHARED: we must see the kernel's writes (and it ours).
   * MAP_POPULATE: pre-fault the pages now instead of on first access.
   *
   * Bitwise OR (|) combines multiple flags. Each flag is a single bit:
   * PROT_READ=0x01, PROT_WRITE=0x02 -> 0x01|0x02=0x03 (both bits set).
   * Use | to combine flags, & to test if a flag is set. */
  uint8_t *ring = (uint8_t *)mmap(NULL, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
  if (ring == MAP_FAILED)
    return -errno;

  r->sq_head = (uint32_t *)(ring + p.sq_off.head);
  r->sq_tail = (uint32_t *)(ring + p.sq_off.tail);
  r->sq_flags = (uint32_t *)(ring + p.sq_off.flags);
  r->sq_array = (uint32_t *)(ring + p.sq_off.array);
  r->sq_mask = *(uint32_t *)(ring + p.sq_off.ring_mask);       /* constant */
  r->sq_entries = *(uint32_t *)(ring + p.sq_off.ring_entries); /* constant */

  r->cq_head = (uint32_t *)(ring + p.cq_off.head);
  r->cq_tail = (uint32_t *)(ring + p.cq_off.tail);
  r->cq_mask = *(uint32_t *)(ring + p.cq_off.ring_mask);
  r->cqes = (struct io_uring_cqe *)(ring + p.cq_off.cqes);

  /* The SQE array is a separate mapping. */
  r->sqes = (struct io_uring_sqe *)mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
  if (r->sqes == MAP_FAILED)
    return -errno;

  r->local_tail = *r->sq_tail; /* 0 for a new ring, but don't assume */
  return 0;
}

/*
 * mu_get_sqe -- claim the next free SQE slot, zeroed.  NULL if the SQ is full.
 * The request is NOT visible to the kernel until mu_submit() publishes the
 * tail.
 */
static inline struct io_uring_sqe *mu_get_sqe(struct mu_ring *r) {
  /* ACQUIRE: once we see head advance, the kernel has finished reading the
   * SQEs before it, so we may overwrite those slots. */
  uint32_t head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
  if (r->local_tail - head >= r->sq_entries)
    return NULL; /* ring full: kernel hasn't consumed enough yet */

  uint32_t idx = r->local_tail & r->sq_mask;
  struct io_uring_sqe *sqe = &r->sqes[idx];
  memset(sqe, 0, sizeof *sqe); /* stale fields from an old request would be
                                  misinterpreted as flags/options */
  r->sq_array[idx] = idx;      /* identity mapping slot -> sqe */
  r->local_tail++;
  return sqe;
}

/* Convenience prep helpers (fields per include/uapi/linux/io_uring.h).
 * For READ/WRITE: `addr` = buffer, `len` = byte count, `off` = file offset
 * (like pread/pwrite; -1 means "use and advance the file position"). */
static inline void mu_prep_rw(struct io_uring_sqe *s, int op, int fd, void *buf, unsigned len, uint64_t off, uint64_t user_data) {
  s->opcode = op;
  s->fd = fd;
  s->addr = (uint64_t)(uintptr_t)buf;
  s->len = len;
  s->off = off;
  s->user_data = user_data; /* opaque cookie, returned verbatim in the CQE */
}

/*
 * mu_submit -- publish all SQEs filled since last time, and (if needed) tell
 * the kernel.  `wait_nr` > 0 additionally blocks until that many CQEs are
 * available.
 *
 * Without SQPOLL: one io_uring_enter() submits the WHOLE batch, however large.
 * With    SQPOLL: a kernel thread is already polling our tail, so the ONLY time
 *                 we need a syscall is to wake that thread after it went to
 *                 sleep (flag IORING_SQ_NEED_WAKEUP) or to block for
 * completions.
 *
 * Returns the number of SQEs submitted (as reported by io_uring_enter, or our
 * own count when no syscall was made), or -errno.
 */
static inline int mu_submit(struct mu_ring *r, unsigned wait_nr) {
  /* We are the only writer of sq_tail, so a plain read of it is fine. */
  uint32_t pending = r->local_tail - *r->sq_tail;

  /* Publish: RELEASE guarantees the SQE contents are visible before the tail.
   */
  __atomic_store_n(r->sq_tail, r->local_tail, __ATOMIC_RELEASE);

  /* IORING_ENTER_GETEVENTS: "also wait until min_complete CQEs exist". */
  unsigned flags = wait_nr ? IORING_ENTER_GETEVENTS : 0;

  if (r->sqpoll) {
    /* Full barrier: our tail store MUST be globally visible before we read
     * sq_flags.  Otherwise the kernel thread could decide to sleep having
     * missed our submission, while we also decide "no wakeup needed" --
     * a lost-wakeup race.  (liburing does exactly this.)  RELEASE alone is
     * not enough: it orders earlier accesses before the store, but does not
     * stop the *later* load of sq_flags from being performed first. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(r->sq_flags, __ATOMIC_RELAXED) & IORING_SQ_NEED_WAKEUP) {
      flags |= IORING_ENTER_SQ_WAKEUP;
    }
    if (!flags) {
      return pending; /* ZERO syscalls: this is the whole point of SQPOLL */
    }
  } else if (!pending && !wait_nr) {
    return 0; /* nothing to submit, nothing to wait for: skip the syscall */
  }

  r->n_enter++;
  /* With SQPOLL the kernel ignores `to_submit` (its thread submits); without
   * it, the kernel consumes up to `pending` SQEs synchronously here. */
  int ret = mu_sys_enter(r->fd, pending, wait_nr, flags);
  return ret < 0 ? -errno : ret;
}

/* Look at the next completion without consuming it; NULL if none. */
static inline struct io_uring_cqe *mu_peek_cqe(struct mu_ring *r) {
  uint32_t head = *r->cq_head; /* only we write cq_head: plain read is fine */
  /* ACQUIRE pairs with the kernel's RELEASE of cq tail: if we see the tail
   * move, the CQE contents are visible too. */
  uint32_t tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
  return head == tail ? NULL : &r->cqes[head & r->cq_mask];
}

/* Mark the CQE returned by mu_peek_cqe() as consumed.  RELEASE: our reads of
 * that CQE are complete before the kernel may reuse the slot. */
static inline void mu_cqe_seen(struct mu_ring *r) { __atomic_store_n(r->cq_head, *r->cq_head + 1, __ATOMIC_RELEASE); }

#endif /* MINI_URING_H */
