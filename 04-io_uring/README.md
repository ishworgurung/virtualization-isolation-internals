# 04 — io_uring

**Builds on:** [05-lockfree](../05-lockfree/) (single-producer/single-consumer rings, acquire/release) helps, but isn't required.

## What it is

Async I/O built on **two ring buffers shared between your process and the
kernel**. You write requests (SQEs) into the submission queue, the kernel writes
results (CQEs) into the completion queue. Many operations per syscall — or, with
`SQPOLL`, none.

```
        user space                          kernel
        ──────────                          ──────
  fill SQE ─▶ SQ ring  (you produce) ─────▶ consume, perform I/O
  reap CQE ◀─ CQ ring  (you consume) ◀───── produce completion
              └──── mmap'd, lock-free, free-running head/tail counters ────┘
```

It is the same SPSC ring design as the virtio virtqueue (`01-virtio/`) and
`05-lockfree/`; only the parties differ (guest↔host vs user↔kernel).

## Files

| File | Purpose |
|---|---|
| `mini_uring.h` | ~150-line io_uring library over raw syscalls, **no liburing**. Every barrier and mmap offset is commented. Read this first. |
| `uring_basic.c` | Tour: NOP, linked WRITE→READ in one syscall, error reporting, ring-full behaviour |
| `uring_bench.c` | Counts syscalls: `read()` loop vs batched io_uring vs SQPOLL |
| `uring_seccomp.c` | Why io_uring is hard to sandbox — and two mitigations |
| `uring_cqpoll.c` | Zero-syscall pipeline: SQPOLL + busy-polling the CQ from userspace (uses **liburing**) |

```
make run        # uring_cqpoll needs liburing (pacman -S liburing / apt install liburing-dev)
```

## Results on this machine (kernel 7.1)

`uring_basic`: a linked WRITE→READ pair costs **1** `io_uring_enter`; 8 NOPs cost **1**.

`uring_bench 1000000`:

```
method                   time(ms)        ops/sec     syscalls
read() loop                  70.2       14248662      1000000
io_uring batch=128           57.5       17406033         7813
io_uring SQPOLL              64.7       15466852            2
```

The **syscall counts** are the robust result (1 000 000 → 7 813 → 2). The **time**
differences here are small: reading `/dev/zero` is almost free and syscalls on
this box are cheap (~70 ns), so it under-sells io_uring. The gains show on real
storage/network I/O with deep queues, and grow when mitigations
(KPTI/retpoline) make each syscall expensive. SQPOLL also occupies a core, so it
only pays off on latency-critical paths. Verify counts independently:

```
strace -c -f -e trace=read,io_uring_enter ./uring_bench 200000
```

## SQPOLL in one paragraph

With `IORING_SETUP_SQPOLL` the kernel spawns a thread that spins on the SQ tail.
You publish SQEs with a release-store and never call `io_uring_enter`, *except*
when the thread has gone to sleep after `sq_thread_idle` ms: it sets
`IORING_SQ_NEED_WAKEUP` in the SQ flags and you must wake it. The subtle
part — see `mu_submit()` — is the **full memory barrier between publishing the tail and
reading that flag**; without it, a lost-wakeup race lets both sides think the
other will act.

## The completion side: polling the CQ (`uring_cqpoll.c`)

SQPOLL removes the *submit* syscall, but a program that sleeps waiting for
completions still calls `io_uring_enter(..., IORING_ENTER_GETEVENTS)`. The fix
needs **no setup flag**: the CQ ring is always mmap'd into your process, so you
can just spin on its tail (`io_uring_peek_cqe` / `io_uring_for_each_cqe`)
instead of sleeping. SQPOLL + CQ spin = ~0 syscalls, at the cost of **two busy
CPUs** (the kernel SQ thread and your spinning thread).

> There is **no `IORING_SETUP_CQPOLL`** flag, kernel config or sysctl. (An
> earlier version of this demo `#define`d one as `1U << 3`, which is really
> `IORING_SETUP_CQSIZE` → `EINVAL`.) The only io_uring sysctls are
> `kernel.io_uring_disabled` and `kernel.io_uring_group`, which gate access.

Don't confuse it with **`IORING_SETUP_IOPOLL`**: that makes the *kernel* poll
the *device* for completions instead of waiting for an IRQ. It only works for
`O_DIRECT` I/O on block devices with poll queues (e.g. NVMe with
`nvme.poll_queues=N`); `/dev/zero` would fail every request with `-EOPNOTSUPP`.

`uring_cqpoll 1000000`:

```
method                   time(ms)        ops/sec
A. normal                    62.7       15953634
B. SQPOLL + wait             76.6       13053710
C. SQPOLL + CQ spin          57.6       17369190
```

`strace -c -f -e trace=io_uring_enter ./uring_cqpoll 100000` → 1566 calls
total: ~782 each for A and B (100000 / 128), ~0 for C. Note B is *slower* than
A: SQPOLL saves the submit syscall but the wait syscall remains, and now the
completion has to cross from the kernel thread to a sleeping task. SQPOLL only
wins once you stop sleeping too.

## The security story (`uring_seccomp.c`)

seccomp-BPF filters **syscalls by number and raw register args**. io_uring moves
the *operation* (`READ`, `OPENAT`, `CONNECT`, `SEND`, …) into ring memory, so
seccomp only ever sees `io_uring_enter(fd, n, …)`.

```
PART 1: seccomp denies read(2), but io_uring READ still works
  read(2)              -> -1 (Operation not permitted)
  pread(2)             -> -1 (Operation not permitted)
  IORING_OP_READ       -> 63 bytes: "Linux version 7.1.13..."   <== BYPASSED THE FILTER
```

Add to that:

* io_uring has a large, fast-moving attack surface and has produced a long run of
  local-privilege-escalation CVEs; **Google restricted it** in ChromeOS, Android
  and production servers, and container runtimes have moved to blocking it in
  default seccomp profiles (check your runtime version's profile).
* Cross-cutting features (registered files/buffers, fixed files, SQPOLL kernel
  threads, worker threads that run in the *kernel's* context for the task)
  complicate credential and cgroup accounting.

Mitigations shown:

1. **Deny the whole family** (`io_uring_setup`, `_enter`, `_register`) in the
   sandbox → `EPERM` (Part 2). Or host-wide: `sysctl kernel.io_uring_disabled=1|2`.
2. **Ring-level allow-list**: create the ring with `IORING_SETUP_R_DISABLED`,
   register `IORING_REGISTER_RESTRICTIONS` for the opcodes you accept, then
   `IORING_REGISTER_ENABLE_RINGS`. Disallowed ops complete with `-EACCES`
   (Part 3, `READ` → `-13`).

## Where it fits in a Firecracker world

* Firecracker's **async block engine** uses io_uring for the virtio-block
  backing file (`"io_engine": "Async"`); the default is a synchronous engine. It
  is a *host-side* optimisation, driven by the trusted VMM, and the Firecracker
  process is itself locked down by seccomp — so the ring's operations are only
  ever ones the VMM chose. (Check current docs: the async engine has historically
  been marked developer-preview and requires a recent host kernel.)
* The takeaway: **use it in the trusted host I/O path; do not expose it
  to the untrusted workload.** Sandboxes like these run *untrusted code*; whether that
  code can create rings is a policy decision, and the safe default is "no".

## Try next

* **Real storage.** Point `uring_bench` at a file on NVMe with `O_DIRECT` and a
  deep queue, where the time difference should finally show.
* **`IORING_SETUP_IOPOLL`.** Boot with `nvme.poll_queues=N` and let the kernel
  poll the device instead of waiting for an interrupt.
* **Mitigations on vs off.** Re-run the benchmark both ways (see the
  [tuning guide](../optimizing-linux-for-ultra-performance.md#6-speculative-execution-mitigations))
  and see how syscall cost changes the gap.
* **A block backend.** Use io_uring as the device side of
  [09-kick-roundtrip](../09-kick-roundtrip/).

## In short

io_uring amortises syscalls with two shared lock-free rings, and SQPOLL
removes them entirely at the cost of a spinning core. But operations submitted
through the ring aren't individual syscalls, so seccomp can't filter them — for
untrusted code you block io_uring outright or use ring restrictions, and only
use it on the host's own trusted I/O path.
