# 03 — userfaultfd (UFFD)

**Builds on:** [02-hypercalls](../02-hypercalls/) for what a VMM is and why it owns guest memory (the demo itself doesn't need KVM).

## What it is

A page fault normally belongs to the kernel. `userfaultfd` lets a **userspace
handler** resolve faults for a memory range it registered:

```
 faulting thread                 kernel                    handler thread
 ───────────────                 ──────                    ──────────────
 load *p  (page missing) ──────▶ fault, no PTE
                                 queue uffd_msg  ─────────▶ read(uffd) -> {address, flags}
                                 faulting thread SLEEPS     fetch page from file/network/...
                                                  ◀──────── ioctl(UFFDIO_COPY, dst, src)
 load completes  ◀────────────── PTE installed, wake up
```

| ioctl | Meaning |
|---|---|
| `UFFDIO_API` | version + feature handshake |
| `UFFDIO_REGISTER` | attach to `[addr,len)`; mode `MISSING` and/or `WP` |
| `UFFDIO_COPY` / `UFFDIO_ZEROPAGE` | resolve a missing-page fault, wake the faulter |
| `UFFDIO_WRITEPROTECT` | set / clear write protection (clearing wakes the faulter) |
| `UFFDIO_CONTINUE`, `MINOR` mode | for shared/hugetlb memory (not used here) |

## Write-protect mode

When you register a memory range with `UFFDIO_REGISTER` using the `WP` flag and then call `UFFDIO_WRITEPROTECT`:

- **Reads** go through normally, no fault
- **Writes** trigger a userfaultfd event
- Your handler is notified of the page address and can decide what to do: allow it, reject it, copy the page, record the change, etc.

This is different from the default `MISSING` mode (which faults on any access). Write-protect faults only on *modification*, making it ideal for:

- **Copy-on-write (CoW)** — Share a page read-only, but split it on first write
- **Dirty tracking** — Know which pages have been modified (the core of live migration: protect everything, run the VM, log which pages got written, repeat)
- **Change detection** — Track mutations for snapshots, backups, or replication
- **Selective faulting** — Allow reads to proceed fast while intercepting writes

After handling a write-protect fault, clear protection on that page with `UFFDIO_WRITEPROTECT` to let future writes proceed without faulting. Part B of `uffd_demo.c` shows this in action: protect 16 pages, let the guest write, trap and log writes on first touch only.

## Why Firecracker-style sandboxes care

**Snapshot restore.** Firecracker can load a snapshot with the guest-memory
backend set to UFFD. The VM resumes *immediately*; a handler process pulls pages
only when the guest touches them — from local disk, another host, or object
storage. A sandbox that touches 5% of its RAM at startup pays for 5%. That is
the mechanism behind sub-200 ms cold starts.

**Live migration / incremental snapshots.** UFFD write-protect mode gives you a
dirty-page log in userspace (Part B), the loop at the heart of pre-copy
migration: *protect everything → run → copy the pages that got dirty → repeat
until the dirty set is small → stop the world for the last round.*

## What `uffd_demo.c` does

### Part A — lazy restore, benchmarked against the alternatives

Creates an N-MiB "snapshot file" (default 64), then simulates a guest that
touches a scattered 5% of pages, using four strategies:

| Strategy | Meaning |
|---|---|
| eager `read()` | load the whole image before resuming (the naive way) |
| `mmap(MAP_PRIVATE)` | page cache + CoW for free; simplest |
| UFFD lazy | handler thread does `pread` + `UFFDIO_COPY` on each fault |
| UFFD + prefetch 8 | on a fault also install the next 8 pages |
| UFFD + 50 µs backend | simulated remote storage — something `mmap` cannot do |

The demo uses **Knuth's multiplicative-hash constant** (2654435761) to scatter page accesses: `idx = (t * 2654435761) % npages`. Because the constant is odd and `npages` is a power of two, this formula is a permutation—it visits each page exactly once before repeating. Result: the guest hits pages in pseudo-random order, simulating a realistic scattered working set, without bias or gaps.

Result from one run (64 MiB, file in page cache, this machine):

```
eager read()        : ready after 11.69 ms
mmap MAP_PRIVATE    : ready after  0.01 ms, done touching after  1.38 ms
UFFD lazy           : ready after  0.06 ms, done touching after  4.38 ms | 819 faults, 5.0% of RAM loaded
UFFD lazy+prefetch8 : ...                          done after 10.71 ms | 41.3% of RAM loaded
UFFD, 50us backend  : ...                          done after 89.87 ms
```

**How to read this honestly:**

* Both lazy strategies resume in microseconds and load 5% of memory; eager pays
  for 100% up front. That is the win.
* **`mmap` beats UFFD here** — the snapshot is in the page cache and the kernel
  resolves faults without a thread wake-up and two ioctls. A UFFD fault costs
  roughly 4–5 µs in this run. *If the backing store is a local file, mmap is
  the right answer*, and the simpler one.
* UFFD earns its keep when you need **control**: remote/compressed/deduplicated
  backends (the 50 µs line), custom policy, or knowing which pages are hot.
* **Prefetch is a bet on locality.** Our synthetic guest touches scattered
  pages, so prefetching 8 neighbours wastes work (41% loaded, slower). A real
  guest's kernel and runtime have strong locality, and production systems
  usually prefetch from a *recorded working set* (pages touched during a
  previous run), not blindly the next N.

### Part B — dirty tracking with write-protect

**The idea:** Live migration needs to know *which pages the guest modified* between snapshots. Write-protect mode lets you build a **dirty bitmap**: arm protection on all pages, let the guest run, trap writes to know which pages changed, then migrate only those pages (the rest are already there). Repeat until the dirty set is small, then stop the world for the final copy.

**Key insight:** it's not just about protecting pages—it's about knowing which ones changed so you don't waste bandwidth migrating untouched memory.

**What the demo does:**

1. Allocate and populate 16 pages (all present, so WP can protect them)
2. Write-protect all 16 pages
3. Simulate guest activity:
   - **Round 1:** guest writes pages 3, 7, 12 and reads page 5
     - First write to page 3 → fault, handler logs "page 3 is dirty", unprotects page 3
     - First write to page 7 → fault, handler logs "page 7 is dirty", unprotects page 7
     - Second write to page 7 → NO fault (page is unprotected, proceeds normally)
     - Read of page 5 → NO fault (reads never fault under WP mode)
     - First write to page 12 → fault, handler logs "page 12 is dirty", unprotects page 12
   - Output: `dirty pages: 3 7 12`
   - Comment: a real migration would now copy only these 3 pages to the destination, then re-arm WP
4. **Round 2:** re-arm write-protection, let the guest run again with a different workset, see which pages are dirty now

This demonstrates the core loop of pre-copy live migration: iteratively shrink the dirty set until it's small enough to migrate in one stop-the-world pause.

```
make run                 # or:  ./uffd_demo 256   for a 256 MiB snapshot
```

## Permissions and gotchas

* `vm.unprivileged_userfaultfd=0` (the default on many distros) blocks
  unprivileged `userfaultfd(2)` *unless* you pass `UFFD_USER_MODE_ONLY`
  (Linux ≥ 5.11), which limits you to faults taken in user mode. That flag is
  why this demo runs unprivileged. Consequence: `read(fd, region, …)` *into* a
  registered region would fault in the kernel and is refused — touch the memory
  from userspace instead. (Faults from a KVM guest are a different path;
  Firecracker's handler has been run with the needed privileges/sysctl.)
* **A handler that stops responding freezes the faulting thread forever.**
  Production handlers need timeouts/health checks; the VM is hostage to them.
* WP mode only tracks pages that are *present*. Populate first (or combine with
  MISSING mode). Newer kernels also have `UFFD_FEATURE_WP_ASYNC` (≥ 6.7), which
  records dirtiness in the page table with no fault-and-handle round trip —
  worth a look for dirty tracking, though this demo uses the classic sync mode.
* `UFFDIO_COPY` returns `EEXIST` if the page is already there (race between a
  prefetch and a fault) — treat as success.
* On Linux ≥ 5.7 anonymous-memory WP works; shmem/hugetlb WP needs newer kernels.
* Also consider `MADV_POPULATE_*`, `MAP_POPULATE` and `userfaultfd` **minor
  faults** for shared memory.

## Try next

* **Async write-protect.** Try `UFFD_FEATURE_WP_ASYNC` (Linux ≥ 6.7) for
  dirty tracking and compare the cost per dirty page with the sync mode used here.
* **A remote page source.** Serve pages over a socket from another process (or
  host) and find where lazy restore starts beating `mmap(MAP_PRIVATE)`.
* **A stalled handler.** Make the handler stop responding, watch the faulting
  thread hang, then add a timeout or health check.
* **Prefetching.** On each fault, also copy in the neighbouring pages, and
  measure faults taken against bytes loaded.

## In short

UFFD lets the VMM own page faults, so a snapshot restore resumes instantly and
pages in only what the guest touches — from any backend, with any prefetch
policy. Plain `mmap(MAP_PRIVATE)` is simpler and faster when the file is local;
UFFD wins when you need control or a remote source. And its write-protect mode
gives you the dirty log that live migration is built on.
