/*
 * uffd_demo.c -- userfaultfd, the mechanism behind lazy snapshot restore.
 *
 * BACKGROUND: WHAT IS A PAGE FAULT, AND WHY HAND IT TO USERSPACE?
 * ---------------------------------------------------------------
 * Memory is managed in pages (4 KiB on x86-64).  mmap() only *reserves* a range
 * of virtual addresses; physical pages are attached lazily.  The first time a
 * program touches a page that has no physical page behind it, the CPU raises a
 * "page fault", the kernel allocates/fills a page (zeroes for anonymous memory,
 * file contents for a file mapping), and the instruction is retried.  The
 * program never notices.
 *
 * userfaultfd ("UFFD") lets a *program* take over that job for a range it
 * chooses: instead of the kernel filling the page, the faulting thread is put
 * to sleep and a message is queued on a file descriptor.  A handler thread (or
 * a different process) reads the message, obtains the page contents from
 * anywhere it likes -- a file, the network, a decompressor -- and asks the
 * kernel to install them, which wakes the faulting thread.
 *
 * Two demos, each modelled on something a microVM platform does:
 *
 *   PART A  "Lazy snapshot restore"
 *           Firecracker's snapshot-load with a UFFD memory backend.  A
 *           snapshot's RAM image is NOT read up front.  We register an empty
 *           region with userfaultfd; the first touch of each page becomes an
 *           event delivered to a handler thread, which fetches just that page
 *           (from a file here; could be S3/NBD/peer VM) and installs it with
 *           UFFDIO_COPY.  The guest (main thread) never knows.
 *           We time it against eager read() and against mmap(MAP_PRIVATE).
 *
 *   PART B  "Dirty page tracking with write-protect mode"
 *           The core of pre-copy live migration: write-protect all of RAM,
 *           catch the first write to each page, record it, unprotect just that
 *           page.  Every round we copy only the pages that got dirty.
 *
 * KEY API (all in <linux/userfaultfd.h>):
 *   syscall(SYS_userfaultfd, flags)   create the fd (glibc has no wrapper)
 *   UFFDIO_API                        handshake / feature negotiation
 *   UFFDIO_REGISTER                   attach the fd to [addr, addr+len)
 *                                     mode MISSING = fault on not-present pages
 *                                     mode WP      = fault on writes to WP
 * pages read(uffd) -> struct uffd_msg     receive a fault event; faulting
 * thread SLEEPS UFFDIO_COPY / UFFDIO_ZEROPAGE     resolve a MISSING fault and
 * wake the thread UFFDIO_WRITEPROTECT               set/clear write protection,
 * wakes on clear
 *
 * BUILD & RUN:  make && ./uffd_demo [snapshot_MiB]       (default 64)
 *
 * PERMISSIONS: we pass UFFD_USER_MODE_ONLY (Linux >= 5.11), which asks only for
 * faults taken in *user mode*, i.e. by our own loads and stores.  Such a uffd
 * is allowed for unprivileged processes even when the sysctl
 * vm.unprivileged_userfaultfd=0 (a common distro default), because it cannot be
 * used to stall the kernel in the middle of a system call -- a trick exploits
 * used to widen race windows.  Cost: a fault taken by the kernel on our behalf
 * (e.g. read(2) *into* the registered region) is not delivered to us and the
 * system call fails instead, so this demo only touches the memory from user
 * code.  A VMM like Firecracker cannot use this flag: guest accesses to
 * not-yet-mapped guest RAM are resolved by KVM from kernel mode, so it needs
 * full userfaultfd access (sysctl vm.unprivileged_userfaultfd=1,
 * CAP_SYS_PTRACE, or access to /dev/userfaultfd on Linux >= 6.1).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* System page size, read at startup (4096 on x86-64; can be 16K/64K on ARM). */
static size_t PAGE;

/* Abort with a readable message if a call returns a negative value.  #expr
 * turns the expression's source text into the message. */
#define CHECK(expr)                                                            \
  if ((expr) < 0) {                                                            \
    perror(#expr);                                                             \
    exit(1);                                                                   \
  }

/* Monotonic clock in milliseconds. */
static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Create a userfaultfd and do the API handshake requesting `features`. */
static int uffd_open(uint64_t features) {
  /* O_NONBLOCK so the handler can poll() + read() without ever hanging.
   * O_CLOEXEC so the fd is not inherited across exec(). */
  int fd =
      syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
  if (fd < 0) {
    perror("userfaultfd (is it blocked by seccomp/sysctl?)");
    exit(1);
  }
  /* Handshake: we tell the kernel which API version + optional features we
   * want; it replies with what it supports.  The fd is unusable until this
   * ioctl succeeds, and it fails if we request a feature the kernel lacks. */
  struct uffdio_api api = {.api = UFFD_API, .features = features};
  CHECK(ioctl(fd, UFFDIO_API, &api));
  return fd;
}

/* Attach the uffd to [addr, addr+len).  `mode` selects which faults are
 * reported: UFFDIO_REGISTER_MODE_MISSING and/or UFFDIO_REGISTER_MODE_WP.
 * addr and len must be page-aligned; the range must already be mapped. */
static void uffd_register(int fd, void *addr, size_t len, uint64_t mode) {
  struct uffdio_register reg = {
      .range = {.start = (uintptr_t)addr, .len = len},
      .mode = mode,
  };
  CHECK(ioctl(fd, UFFDIO_REGISTER, &reg));
}

/* ========================================================================== *
 *                    PART A: lazy snapshot restore
 * ========================================================================== */

/* Everything the handler thread needs, shared with the "guest" (main thread).
 */
struct lazy_ctx {
  int uffd;
  int stop_fd;   /* eventfd used to tell the handler to exit          */
  int snap_fd;   /* the "snapshot file" (memory image)                */
  uint8_t *base; /* start of the registered region                    */
  size_t npages;
  unsigned prefetch;   /* extra pages to speculatively install per fault    */
  unsigned latency_us; /* simulated remote-storage latency per fetch        */
  /* stats */
  atomic_uint faults;    /* faults received                                   */
  atomic_uint installed; /* pages installed (faults + prefetch)               */
};

/* Fetch page `idx` from the snapshot and install it.  Returns 0 on success.
 * `buf` is a page-sized scratch buffer owned by the handler thread. */
static int install_page(struct lazy_ctx *c, size_t idx, uint8_t *buf) {
  if (c->latency_us)
    usleep(c->latency_us); /* pretend this is a network read */
  if (pread(c->snap_fd, buf, PAGE, idx * PAGE) != (ssize_t)PAGE)
    return -1;

  /* Why not simply memcpy() into the region?  Because touching the region
   * would itself be a missing-page fault -- on the handler thread, which
   * would then wait for... itself.  UFFDIO_COPY asks the kernel to allocate
   * a fresh page, copy `buf` into it, and map it at `dst` in one atomic step:
   * no thread can ever observe a half-filled page. */
  struct uffdio_copy cp = {
      .dst = (uintptr_t)c->base + idx * PAGE,
      .src = (uintptr_t)buf,
      .len = PAGE,
      .mode = 0, /* 0 => also wake any thread blocked on this page */
  };
  if (ioctl(c->uffd, UFFDIO_COPY, &cp) < 0) {
    /* EEXIST: another fault/prefetch already installed it -- benign. */
    return errno == EEXIST ? 0 : -1;
  }
  /* Successfully installed the page. */
  atomic_fetch_add(&c->installed, 1);
  return 0;
}

/*
 * The handler thread -- in Firecracker's model this is a *separate process*
 * (the "UFFD handler") that receives the uffd over a unix socket, but the loop
 * is the same.  (A production handler must also handle other events, e.g.
 * UFFD_EVENT_REMOVE when the balloon device discards guest pages.)
 *
 * poll() waits on two fds at once: the uffd (a fault is queued) and stop_fd
 * (main() wants us to exit).
 */
static void *lazy_handler(void *arg) {
  struct lazy_ctx *c = arg;
  uint8_t *buf = aligned_alloc(PAGE, PAGE);
  struct pollfd pfd[2] = {
      {.fd = c->uffd, .events = POLLIN},
      {.fd = c->stop_fd, .events = POLLIN},
  };

  for (;;) {
    CHECK(poll(pfd, 2, -1)); /* -1 = no timeout */
    if (pfd[1].revents & POLLIN)
      break; /* shutdown requested */

    struct uffd_msg msg;
    ssize_t n = read(c->uffd, &msg, sizeof msg);
    if (n < 0) {
      /* EAGAIN (fd is non-blocking): nothing queued after all, e.g. a
       * fault that a prefetch resolved -- and woke -- before we read it. */
      if (errno == EAGAIN)
        continue; /* spurious wake */
      perror("read uffd");
      break;
    }
    if (msg.event != UFFD_EVENT_PAGEFAULT) {
      continue;
    }
    /* Count this page fault. */
    atomic_fetch_add(&c->faults, 1);

    /* The kernel reports the page-aligned faulting address (the exact byte
     * address only if UFFD_FEATURE_EXACT_ADDRESS was requested); rounding
     * down to a page boundary is therefore just defensive. */

    /* Round down to the nearest page boundary. */
    uint64_t addr = msg.arg.pagefault.address & ~(uint64_t)(PAGE - 1);
    /* Compute the page index within the guest memory. */
    size_t idx = (addr - (uintptr_t)c->base) / PAGE;

    /* Fault path: this install wakes the sleeping faulter, so do it first. */
    if (install_page(c, idx, buf) < 0) {
      fprintf(stderr, "install failed\n");
      break;
    }

    /* Prefetch: guests are spatially local, so grab the next few pages
     * now, before they fault.  This is the "you know which pages are hot"
     * advantage of UFFD over plain mmap. */
    for (unsigned k = 1; k <= c->prefetch && idx + k < c->npages; k++)
      install_page(c, idx + k, buf);
  }
  free(buf);
  return NULL;
}

/* Build the "snapshot": page i starts with the tag (i+1) so we can verify.
 * The rest of each page is zero.  Returns an fd open for reading/writing. */
static int make_snapshot(const char *path, size_t npages) {
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  CHECK(fd);
  uint8_t *page = calloc(1, PAGE);
  for (size_t i = 0; i < npages; i++) {
    memset(page, 0, PAGE);
    /* `&(uint64_t){ i + 1 }` is a C99 compound literal: a temporary
     * uint64_t we can take the address of. */
    memcpy(page, &(uint64_t){i + 1}, sizeof(uint64_t));
    CHECK(pwrite(fd, page, PAGE, i * PAGE));
  }
  free(page);
  return fd;
}

/* "The guest": touch a sparse set of pages (spread out, ~few percent of RAM).
 * Reads the first 8 bytes (the tag) of each touched page and sums them, so the
 * three strategies can be checked for identical results.
 * 2654435761 is Knuth's multiplicative-hash constant; because it is odd,
 * t -> t*2654435761 mod npages is a permutation whenever npages is a power of
 * two (true for the default 64 MiB), so every touch hits a different page. */
static uint64_t guest_touch(const uint8_t *mem, size_t npages, size_t touches) {
  uint64_t sum = 0;

  for (size_t t = 0; t < touches; t++) {

    size_t idx = (t * 2654435761u) % npages; /* scattered, deterministic */

    /* volatile: force a real load, so the compiler cannot optimise the
     * access (and therefore the page fault) away. */

    asm volatile("" ::: "memory"); /* prevent compiler reordering */

    /* first touch => fault */
    sum += *(const volatile uint64_t *)(mem + idx * PAGE);
  }
  return sum;
}

static void part_a(size_t mib) {
  size_t npages = mib * 1024 * 1024 / PAGE;
  size_t touches = npages / 20; /* the guest only ever uses ~5% of its RAM */
  printf("==== PART A: lazy snapshot restore  (%zu MiB snapshot, %zu pages, "
         "guest touches %zu = %.1f%%) ====\n",
         mib, npages, touches, 100.0 * touches / npages);

  /* mkstemp() creates a uniquely named file; XXXXXX is replaced in place. */
  char snap_path[] = "/tmp/uffd_snapshot.XXXXXX";
  int snap_fd = mkstemp(snap_path);
  CHECK(snap_fd);
  close(snap_fd);
  snap_fd = make_snapshot(snap_path, npages);
  unlink(snap_path); /* file lives on until we close it */
  /* NOTE: we just wrote the file, so it sits in the page cache (RAM).  All
   * "disk" reads below are really memory copies: a best case for eager and
   * mmap.  The 50 us configuration simulates a slow/remote backend. */

  size_t len = npages * PAGE;

  /* --- Strategy 1: EAGER -- read the whole file before "resuming" ------- */
  double t0 = now_ms();
  uint8_t *eager = mmap(NULL, len, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  /* pread may return fewer bytes than asked, so loop until everything is in. */
  for (size_t off = 0; off < len;) {
    ssize_t r = pread(snap_fd, eager + off, len - off, off);
    CHECK(r);
    off += r;
  }
  double t_eager_ready = now_ms() - t0; /* VM cannot run until this completes */
  uint64_t s1 = guest_touch(eager, npages, touches);
  printf("eager read()      : ready after %7.2f ms, done touching after %7.2f "
         "ms\n",
         t_eager_ready, now_ms() - t0);

  /* --- Strategy 2: mmap(MAP_PRIVATE) of the file ------------------------ */
  /* Simple and very good: page cache + CoW for free.  No control over
   * fetching, though, and the backing MUST be a file on this host.
   * MAP_PRIVATE = copy-on-write: reads come straight from the page cache;
   * the first write to a page gives this process a private copy, so the
   * snapshot file is never modified.  Faults here are resolved entirely in
   * the kernel -- no thread hand-off. */
  t0 = now_ms();
  uint8_t *mm =
      mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, snap_fd, 0);
  if (mm == MAP_FAILED) {
    perror("mmap file");
    exit(1);
  }
  double t_mmap_ready = now_ms() - t0;
  uint64_t s2 = guest_touch(mm, npages, touches);
  printf("mmap MAP_PRIVATE  : ready after %7.2f ms, done touching after %7.2f "
         "ms\n",
         t_mmap_ready, now_ms() - t0);

  /* --- Strategy 3: UFFD lazy load, several flavours --------------------- */
  struct config {
    const char *name;    /* Name of the configuration */
    unsigned prefetch;   /* Number of pages to prefetch on a fault */
    unsigned latency_us; /* Simulated latency for handling a page fault in
                            microseconds */
  };

  struct config cfg[] = {
      {"UFFD lazy          ", 0, 0},
      /* 16 pages prefetch */
      {"UFFD lazy+prefetch16", 16, 0},
      /*
      Pretend remote storage with a 200us backend latency; mmap can't do this
       */
      {"UFFD, 200us backend ", 0, 200},
  };
  uint64_t s3 = 0;
  for (size_t k = 0; k < sizeof cfg / sizeof cfg[0]; k++) {
    t0 = now_ms();
    /* An EMPTY anonymous region: no page is present yet, so every first
     * touch is a "missing" fault that userfaultfd will report to us. */
    uint8_t *region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    struct lazy_ctx c = {.snap_fd = snap_fd,
                         .base = region,
                         .npages = npages,
                         .prefetch = cfg[k].prefetch,
                         .latency_us = cfg[k].latency_us};
    c.uffd = uffd_open(0);
    c.stop_fd = eventfd(0, 0);
    uffd_register(c.uffd, region, len, UFFDIO_REGISTER_MODE_MISSING);

    pthread_t th;
    pthread_create(&th, NULL, lazy_handler, &c);
    double t_ready =
        now_ms() - t0; /* guest can resume NOW: no data loaded yet */

    s3 = guest_touch(region, npages, touches); /* faults happen here */
    printf("%s : ready after %7.2f ms, done touching after %7.2f ms | "
           "faults=%u installed=%u (%.1f%% of RAM loaded)\n",
           cfg[k].name, t_ready, now_ms() - t0, atomic_load(&c.faults),
           atomic_load(&c.installed),
           100.0 * atomic_load(&c.installed) / npages);

    /* Tear down: stop the handler, then unmap and close. */
    eventfd_write(c.stop_fd, 1);
    pthread_join(th, NULL);
    munmap(region, len);
    close(c.uffd);
    close(c.stop_fd);
  }

  /* Same pages touched, same tags read => same sums, whichever strategy
   * produced the memory.  (s3 is from the last UFFD configuration.) */
  printf("checksums (must match): eager=%lu mmap=%lu uffd=%lu -> %s\n\n", s1,
         s2, s3, (s1 == s2 && s2 == s3) ? "OK" : "MISMATCH");
  munmap(eager, len);
  munmap(mm, len);
  close(snap_fd);
}

/* ========================================================================== *
 *               PART B: write-protect dirty page tracking
 * ========================================================================== */

#define B_PAGES 16

struct wp_ctx {
  int uffd;
  int stop_fd;
  uint8_t *base;
  atomic_uchar dirty[B_PAGES]; /* the "dirty bitmap" a migration would send */
};

/* Set (wp=1) or clear (wp=0) write protection on a page range. */
static void set_wp(int uffd, void *addr, size_t len, int wp) {
  struct uffdio_writeprotect w = {
      .range = {.start = (uintptr_t)addr, .len = len},
      /* mode 0 = remove protection AND wake threads waiting on it
       * (UFFDIO_WRITEPROTECT_MODE_DONTWAKE would suppress the wake-up). */
      .mode = wp ? UFFDIO_WRITEPROTECT_MODE_WP : 0,
  };
  CHECK(ioctl(uffd, UFFDIO_WRITEPROTECT, &w));
}

/* Handler: every write-protect fault = "this page just became dirty". */
static void *wp_handler(void *arg) {
  struct wp_ctx *c = arg;
  struct pollfd pfd[2] = {
      {.fd = c->uffd, .events = POLLIN},
      {.fd = c->stop_fd, .events = POLLIN},
  };
  for (;;) {
    CHECK(poll(pfd, 2, -1));

    /* Return early if it is a POLLIN on the uffd */
    if (pfd[1].revents & POLLIN) {
      break;
    }

    struct uffd_msg msg;

    if (read(c->uffd, &msg, sizeof msg) < 0) {
      continue;
    }
    /* Return early if it is not a pagefault event */
    if (msg.event != UFFD_EVENT_PAGEFAULT) {
      continue;
    }
    /* The WP flag distinguishes "wrote to a protected page" from a
     * missing-page fault. */
    if (!(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP)) {
      continue;
    }

    uint64_t addr = msg.arg.pagefault.address & ~(uint64_t)(PAGE - 1);
    size_t idx = (addr - (uintptr_t)c->base) / PAGE;

    /* 1. Remember: this page is dirty.
     * 2. Un-protect JUST this page so the write can proceed and later
     *    writes to it cost nothing (we already know it is dirty).
     * Order matters: the dirty flag is set BEFORE the unprotect wakes the
     * writer, so by the time the writer continues the bitmap is updated. */
    atomic_store(&c->dirty[idx], 1);
    set_wp(c->uffd, (void *)(uintptr_t)addr, PAGE, 0);
    puts("WP handler: page marked dirty and unprotected");
  }
  return NULL;
}

static void print_dirty(struct wp_ctx *c) {
  printf("   dirty pages:");
  int n = 0;
  for (int i = 0; i < B_PAGES; i++)
    if (atomic_load(&c->dirty[i])) {
      printf(" %d", i);
      n++;
    }
  printf("   (%d of %d)\n", n, B_PAGES);
}

static void part_b(void) {
  printf("==== PART B: dirty-page tracking via UFFD write-protect "
         "(live-migration building block) ====\n");

  /* Allocate a block of memory for the pages to be tracked - 16 pages
   * (B_PAGES). */
  uint8_t *mem = mmap(NULL, B_PAGES * PAGE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  /* Write-Protect only applies to pages that are PRESENT, so populate them
   * first. (Absent pages are handled by MISSING mode.  Linux >= 6.4 also offers
   * UFFD_FEATURE_WP_UNPOPULATED to protect not-yet-populated anonymous
   * pages; we don't rely on it.) */
  memset(mem, 0xBB, B_PAGES * PAGE);

  struct wp_ctx c = {.base = mem};
  /* Feature negotiation: ask for WP fault flag support. */
  c.uffd = uffd_open(UFFD_FEATURE_PAGEFAULT_FLAG_WP);
  c.stop_fd = eventfd(0, 0);
  uffd_register(c.uffd, mem, B_PAGES * PAGE, UFFDIO_REGISTER_MODE_WP);
  pthread_t th;
  /* Create the write-protect handler thread. */
  pthread_create(&th, NULL, wp_handler, &c);

  /* Two "migration rounds".  Each round: clear the bitmap, protect
   * everything, let the guest run, read the bitmap. */
  for (int round = 1; round <= 2; round++) {
    printf(" round %d: write-protect all %d pages, then the 'guest' runs\n",
           round, B_PAGES);
    memset((void *)c.dirty, 0, sizeof c.dirty);
    /* Arm the write-protect on all pages */
    set_wp(c.uffd, mem, B_PAGES * PAGE, 1);

    /* Guest activity.  Reads never fault; the first write to a page does.
     * Each faulting store blocks until the handler unprotects the page,
     * so when this block finishes, all dirty flags have been recorded. */
    volatile uint8_t *p = mem;
    if (round == 1) {
      p[3 * PAGE] = 1;       /* first write to page 3   -> fault, dirty */
      p[7 * PAGE] = 1;       /* page 7                  -> fault, dirty */
      p[7 * PAGE + 100] = 2; /* page 7 again            -> NO fault     */
      asm volatile("" ::: "memory"); /* prevent compiler reordering */
      /* The asm barrier orders the read after everything before it, and
       * volatile makes the compiler treat it as a side effect that must happen.
       * No reordering, no elision. */
      /* a read of page 5        -> NO fault     */
      *(const volatile uint64_t *)&p[5 * PAGE];
      p[12 * PAGE] = 1; /* page 12                 -> fault, dirty */
    } else if (round == 2) {
      /* round 2: different working set          */
      /* Write to page 0 -> fault, dirty */
      p[0] = 9;
      /* Write to page 7 -> fault, dirty */
      p[7 * PAGE] = 9;
    }
    print_dirty(&c);
    printf("   => a migration round would copy only those pages, then re-arm "
           "WP\n");
  }

  eventfd_write(c.stop_fd, 1);
  pthread_join(th, NULL);
  puts("");
}

int main(int argc, char **argv) {
  PAGE = sysconf(_SC_PAGESIZE);
  /* Optional argument: snapshot size in MiB. */
  size_t mib = argc > 1 ? strtoul(argv[1], NULL, 10) : 64;
  part_a(mib);
  part_b();
  return 0;
}
