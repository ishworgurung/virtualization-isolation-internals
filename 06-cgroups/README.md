# 06 — cgroups v1 vs v2

**Builds on:** nothing; stands alone.

`cglab` is a std-only Rust tool that creates **real cgroups** on your machine and
shows each mechanism working. It runs unprivileged inside the subtree systemd
delegates to your user (`user@UID.service`); set `CGLAB_ROOT=<dir>` to use another
one, or run as root.

```
cargo build --release
./target/release/cglab all         # info + limits + weight + nip + gomaxprocs
./target/release/cglab limits      # or: weight | nip | gomaxprocs | info
./target/release/cglab jail --cpu 50 --mem 64 --pids 32 -- <cmd...>
./target/release/cglab clean       # if a run was interrupted
```

Source layout: `src/cg.rs` = the cgroup filesystem helpers (everything is
`mkdir`/`write`/`read` — the whole API is a file tree); `src/main.rs` = the demos.

## v1 vs v2 in one table

| | v1 | v2 |
|---|---|---|
| Hierarchy | one tree **per controller** (`/sys/fs/cgroup/cpu/…`, `/memory/…`) | **one unified tree**, all controllers |
| A process's membership | can be in different groups per controller | exactly one group |
| Enabling controllers | implicit by mounting | `cgroup.subtree_control` (`+cpu +memory`) per level |
| Processes | any node | **leaves only** when controllers are enabled (no-internal-process rule) |
| CPU cap | `cpu.cfs_quota_us` + `cpu.cfs_period_us` | `cpu.max` (`"quota period"`) |
| CPU share | `cpu.shares` (2–262144, default 1024) | `cpu.weight` (1–10000, default 100) |
| Memory cap | `memory.limit_in_bytes` (hard only) | `memory.max` (hard, OOM) **and** `memory.high` (throttle) |
| Memory protection | `soft_limit_in_bytes` (weak) | `memory.min` (hard guarantee), `memory.low` (best effort) |
| Block I/O | `blkio` — effectively **direct I/O only** | `io` — understands **writeback** (buffered writes) |
| Pressure info | none | **PSI**: `cpu/memory/io.pressure` per cgroup |
| Kill a group | walk `tasks`, race with fork | `cgroup.kill` (5.14+) |
| Delegation to unprivileged users | awkward, unsafe | designed for it (rootless containers) |

## What the lab demonstrates (real output from this machine)

### `cpu.max` — hard bandwidth cap

```
cpu.max = 20000 100000  (0.2 CPU).  Worker: 4 busy threads for 2 s
wall 2.04 s, CPU consumed 0.41 s  => average 0.20 CPUs (asked for 4)
cpu.stat: nr_periods=21 nr_throttled=21 throttled_usec=7688269
```

Four threads wanted 4 CPUs and were throttled in *every* 100 ms period. Note the
difference between the *cap* (`cpu.max`) and the *share* (`cpu.weight`): the cap
applies even when the machine is idle; the weight only matters under contention.

### `cpu.weight` — proportional share

Two groups pinned to CPU 0 (via `taskset`, to force contention), weights 100 and 400:

```
w100 used 0.59 s   w400 used 2.37 s   => ratio 1 : 4.0
```

### `memory.high` vs `memory.max` — the biggest v2 improvement

```
memory.high = 32 MiB:  worker reaches 32 MiB in 59 ms, then crawls.
   memory.events: high 409   oom_kill 0        <- throttled, never killed
memory.max  = 32 MiB:  worker SIGKILLed.
   memory.events: max 37  oom 1  oom_kill 1    memory.peak: 32 MiB
```

* `memory.high` is a *throttle*: above it, the kernel reclaims and delays the
  allocating tasks. It never kills. The right knob for a **soft ceiling** with
  graceful degradation and a good signal (`high` events) to alert on.
* `memory.max` is a *wall*: reclaim first, then the OOM killer — **scoped to that
  cgroup**, so a tenant cannot OOM its neighbours or the host.
* Best practice: set `high` somewhat below `max` so pressure builds gradually
  before the kill; use `memory.min`/`memory.low` to protect critical services.
* In this run swap is off, so anonymous pages cannot be reclaimed and the
  throttled worker made essentially no progress (`memory.pressure` `full` rose to
  ~20–40%). With swap or a file-backed working set the slowdown is much gentler.

### PSI — pressure stall information

Every cgroup exposes `cpu.pressure`, `memory.pressure` (and `io.pressure`):

```
some avg10=8.87 avg60=1.60 avg300=0.33 total=1017585
full avg10=8.87 ...
```

*some* = % of time at least one task was stalled waiting for the resource;
*full* = % of time **all** non-idle tasks were stalled. `total` is cumulative
microseconds, so you can compute rates. **This is the noisy-neighbour signal**:
CPU utilisation tells you "busy"; PSI tells you "*people are waiting*". It is
also pollable (`epoll` on the file with a threshold trigger written into it) so
an agent can react without polling loops, and it is the input to `systemd-oomd`
and Android's `lmkd`, which makes it a natural fit for a multi-tenant sandbox fleet.

### `pids.max`

`pids.max = 8` → the worker started 7 threads and `pids.events max` = 13 rejected
clones. Fork-bomb containment for free. (Threads count as pids.)

### No-internal-processes rule

```
1) group with a process: enable +memory for children  -> EBUSY
2) group with +memory enabled: move a process INTO it -> EBUSY
   ...but its leaf child accepts the process           -> ok
```

A group can either **hold processes** or **distribute controllers to children**,
not both. Otherwise a process in an interior node competes with its own child
groups and the accounting stops being well-defined (v1's problem). Practical
consequences: container runtimes and `systemd` build a `parent/` (interior) +
`leaf/` structure; if you `echo +memory > cgroup.subtree_control` and get `EBUSY`,
move the processes out first. (Exception: the root cgroup; and threaded mode.)

### `cpu.max` → GOMAXPROCS

`cglab gomaxprocs` computes the effective limit from `cpu.max` of the group and
all its ancestors (tightest wins):

```
cpu.max = 50000 100000   effective 0.50  -> derived parallelism 1
cpu.max = 250000 100000  effective 2.50  -> derived parallelism 3
cpu.max = max 100000     none            -> derived parallelism 24 (host CPUs)
```

* **Go ≤ 1.24**: `GOMAXPROCS` = host CPU count, blind to `cpu.max`. A pod limited to
  2 CPUs on a 64-core node ran 64 Ps; the cgroup throttled them → tail-latency spikes.
  Workarounds: `automaxprocs`, or set `GOMAXPROCS` yourself.
* **Go ≥ 1.25**: default is cgroup-aware and updates if the limit changes. It
  applies only when you have not set `GOMAXPROCS` yourself. (I believe the runtime
  also applies a minimum of 2 — verify in the release notes before relying on it.)
* A subtlety: a cpu.max **quota** is bandwidth, not "N CPUs". Even with a
  right-sized `GOMAXPROCS`, a bursty service can hit throttling inside a 100 ms
  period; many teams therefore prefer **requests (cpu.weight) without limits** for
  latency-critical services, and watch `nr_throttled` / `throttled_usec`.

## Where this meets Firecracker

The **jailer** is a small program that, before exec'ing Firecracker, creates a
cgroup for that microVM, writes limits into it (`cpu.max`/`memory.max` on v2,
`cpu.cfs_*`/`memory.limit_in_bytes` on v1 — it accepts `--cgroup <file>=<value>`
and `--cgroup-version 1|2`; check the docs for your release), writes its own
PID into `cgroup.procs`, then drops privileges, `chroot`s and `exec`s. So the VMM
— which contains the vCPU threads, device emulation and the whole guest's memory
accounting — is confined *from its first instruction*.

`cglab jail` reproduces that pattern: `spawn_in()` in `src/cg.rs` uses `pre_exec`
to write `0` to `cgroup.procs` between `fork` and `exec`:

```
$ cglab jail --cpu 50 --mem 16 --pids 16 -- sh -c '...'
jail: .../cglab-1187145/jail (limits: cpu.max="50000 100000" memory.max="16777216" pids.max="16")
exit: exit status: 0   usage_usec=30350 nr_throttled=0 memory.peak=1 MiB
```

A subtlety for VMs: the guest's RAM is host memory charged to the VMM's cgroup,
so `memory.max` must include guest RAM **plus** VMM overhead, or you will OOM-kill
a healthy VM. And a memory balloon (see `virtio-balloon`) lets you shrink that
footprint from outside.

## Gotchas

* Controllers must be enabled level by level: `+memory` in `parent/cgroup.subtree_control`
  before `parent/child/memory.max` even exists.
* `rmdir` fails with `EBUSY` while any process (even a dying one) is inside;
  `cgroup.kill` first, then retry (`Group::destroy`).
* Writes are `write(2)` of one short line; some files reject partial/long writes.
* `io.max` (block I/O limits) is not shown: systemd did not delegate the `io`
  controller to this user. As root it is `echo "8:0 rbps=10485760" > io.max`.
* Kernel 5.19+ has `memory.peak`; 5.14+ has `cgroup.kill`; PSI needs `CONFIG_PSI`.

## Try next

* **`io.max`.** As root (or with the `io` controller delegated), throttle
  buffered writes and watch the v2 controller account for writeback.
* **PSI triggers.** Write a threshold into `memory.pressure`, `epoll` on it, and
  react when the group stalls, without polling.
* **Gentler `memory.high`.** Repeat the `memory.high` run with swap on, or with a
  file-backed working set, and compare how hard the throttle bites.
* **A real VM in a jail.** Run a QEMU or Firecracker guest under `cglab jail` and
  measure how much the VMM needs above guest RAM before `memory.max` is safe.

## In short

v2 is one unified tree with the no-internal-processes rule, `memory.high`
throttling before `memory.max` kills, `cpu.max`/`cpu.weight`, writeback-aware
`io`, and PSI — which is the best noisy-neighbour signal because it measures
*waiting*, not utilisation. Firecracker's jailer puts each microVM in its own
cgroup before exec so it's confined from the first instruction.
