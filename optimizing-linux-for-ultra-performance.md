# Optimizing a Linux system for ultra performance

How to get the most predictable, lowest-latency performance out of physical CPU
cores on a Linux workstation, for benchmarks, lock-free code, polling I/O
(io_uring SQPOLL, busy-poll rings) and VMM/vCPU threads.

Worked example throughout: **Intel i9-12900K** (Alder Lake, 8 P-cores with
SMT + 8 E-cores) on **Manjaro**, kernel 7.1. The commands are generic; only the
CPU numbers are specific to that machine.

> **Two goals, pick per task.**
> *Peak speed* (production): turbo on, everything fast.
> *Reproducible numbers* (benchmarking): turbo off, fixed frequency, isolated
> cores. The same setting (e.g. turbo) helps one and hurts the other.

Contents:

0. [Know your topology](#0-know-your-topology)
1. [Keep CPU microcode up to date](#1-keep-cpu-microcode-up-to-date)
2. [Pin hot threads to the right cores](#2-pin-hot-threads-to-the-right-cores)
3. [Isolate cores from the kernel](#3-isolate-cores-from-the-kernel)
4. [Frequency and sleep states](#4-frequency-and-sleep-states)
5. [SMT (hyper-threading)](#5-smt-hyper-threading)
6. [Speculative-execution mitigations](#6-speculative-execution-mitigations)
7. [Memory](#7-memory)
8. [Waiting well: spin, backoff, yield, block](#8-waiting-well-spin-backoff-yield-block)
9. [One-shot presets: tuned and RT kernels](#9-one-shot-presets-tuned-and-rt-kernels)
10. [BIOS / firmware](#10-bios--firmware)
11. [Measuring and verifying](#11-measuring-and-verifying)
12. [Making settings permanent](#12-making-settings-permanent)
13. [Checklist](#13-checklist)

Most commands below change the *running* system and are lost at reboot. Each
section ends with **Make it permanent**; [section 12](#12-making-settings-permanent)
explains the mechanisms (GRUB, sysctl.d, tmpfiles.d, systemd, limits.d) once.

---

## 0. Know your topology

Every decision below depends on which logical CPUs share a physical core and
which cores are fast.

```sh
lscpu --extended=CPU,CORE,MAXMHZ     # logical CPU -> physical core, max clock
lscpu | grep -E "Model name|L1d|L2|L3"
cat /proc/cmdline                    # current kernel command line
```

On the i9-12900K:

```text
CPU  CORE  MAXMHZ
 0,1    0   5100    P-core 0 (two SMT siblings)
 2,3    1   5100
 4,5    2   5200    <- "favoured" cores: highest boost
 6,7    3   5200    <-
 8..15 4-7  5100    remaining P-cores
16..23 8-15 3900    E-cores (no SMT, lower clock, smaller caches)
```

Two consequences:

* A thread that lands on an **E-core** runs noticeably slower. On this machine
  the same benchmark varied **4 s vs 21 s** between runs purely from where the
  scheduler put the threads.
* **SMT siblings** (0/1, 2/3, ...) share one physical core's execution units
  and L1/L2 caches. That is good for threads that ping-pong a cache line, bad
  for two compute-heavy threads.

---

## 1. Keep CPU microcode up to date

Microcode is the CPU's internal firmware: it implements and patches
instructions inside the chip. It is also a prerequisite for several items
below, so do it first.

**Common misconception:** disabling the microcode loader (`dis_ucode_ldr` on
the kernel command line) does *not* avoid Intel code. Every CPU always runs
Intel's microcode; the flag just **freezes you on the old version** from the
factory or BIOS. The choice is *old Intel blob with known holes* vs *new Intel
blob with them fixed*.

Why the update path is safe:

* Updates are **signed and encrypted by Intel, and the CPU verifies them
  itself**. The OS only hands the file over; a modified file is rejected by the
  silicon. Distro packagers, mirrors or an attacker cannot use this path.
* Updates are **not persistent**: they are loaded into the CPU on every boot and
  never written to the chip. Uninstall and reboot to go back.
* They come from Intel's public repository with per-CPU release notes.

What stale microcode costs you: fixes for published CPU bugs, some of which the
kernel **cannot** mitigate without microcode support. Example from the
12900K running revision `0x22`:

```text
$ journalctl -k -b | grep -i -e microcode -e "Data Sampling"
x86/CPU: Running old microcode
Register File Data Sampling: Vulnerable: No microcode
```

*Register File Data Sampling* (E-cores) lets a process read stale register
values left by another process. The kernel's fix relies on microcode making the
`VERW` instruction clear those registers; without the update there is no fix.

It does not cost performance you can't get back: individual mitigations can be
switched off on the kernel command line (see [section 6](#6-speculative-execution-mitigations)),
so the right setup for performance work is **new microcode + your own choice of
mitigations**.

Install / verify (Arch / Manjaro, GRUB):

```sh
sudo pacman -S intel-ucode           # amd-ucode on AMD
# remove dis_ucode_ldr from GRUB_CMDLINE_LINUX_DEFAULT if present:
sudoedit /etc/default/grub
sudo update-grub                     # adds intel-ucode.img as an early initrd
reboot

grep -m1 microcode /proc/cpuinfo     # revision should have gone up
cat /sys/devices/system/cpu/vulnerabilities/old_microcode
```

If pacman fails with `... exists in filesystem` (files present but no package
owns them, e.g. after `pacman -Rdd --dbonly`), let it take over just those
paths:

```sh
sudo pacman -Syu intel-ucode \
  --overwrite '/usr/lib/firmware/intel-ucode/*' \
  --overwrite /boot/intel-ucode.img \
  --overwrite '/usr/share/licenses/intel-ucode/*'
```

**Make it permanent:** already is. The package keeps `/boot/intel-ucode.img`
current, `update-grub` adds it to every boot entry, and it is reloaded on every
boot. Just make sure `dis_ucode_ldr` stays out of `/etc/default/grub`.

---

## 2. Pin hot threads to the right cores

Free, no reboot, and the biggest single win on a hybrid (P-core + E-core) CPU.

```sh
taskset -c 4,6 ./my_benchmark        # two different fast P-cores
taskset -c 4,5 ./my_benchmark        # two SMT siblings of one P-core
```

Which to choose:

| Workload | Pin to | Why |
|---|---|---|
| Two threads passing data back and forth (SPSC ring, ping-pong) | **SMT siblings** (4,5) | they share L1/L2: a cache-line hand-off costs a few ns instead of ~50 ns through L3 |
| Independent compute-heavy threads | **different physical cores** (4,6) | siblings would compete for the same execution units |
| Anything latency-critical | **P-cores only**, never E-cores | higher clock, bigger caches |

From code: Rust `core_affinity` crate or `libc::sched_setaffinity`; C
`sched_setaffinity(2)` / `pthread_setaffinity_np(3)`.

Pinning alone does not stop *other* tasks, interrupts and kernel timers from
running on those cores. That is what isolation is for.

**Make it permanent:** `taskset` only affects the command it starts. For
something that runs every boot, make it a systemd service and pin it there:

```ini
# /etc/systemd/system/mybench.service  (excerpt)
[Service]
ExecStart=/usr/local/bin/mybench
CPUAffinity=4 5
```

To keep *all* services and user sessions off chosen cores without a reboot
parameter (a softer alternative to `isolcpus`), set a default affinity for
everything systemd starts:

```ini
# /etc/systemd/system.conf.d/affinity.conf
[Manager]
CPUAffinity=0-3 8-23
```

Then `sudo systemctl daemon-reexec` (or reboot). Your pinned workload can
still be placed on 4-7 explicitly.

---

## 3. Isolate cores from the kernel

Kernel command line (needs a reboot). Add to `GRUB_CMDLINE_LINUX_DEFAULT` in
`/etc/default/grub`, then `sudo update-grub`:

```text
isolcpus=domain,managed_irq,4-7 nohz_full=4-7 rcu_nocbs=4-7 irqaffinity=0-3,8-23
```

| Parameter | Effect |
|---|---|
| `isolcpus=domain,managed_irq,4-7` | scheduler never places tasks on CPUs 4-7 unless explicitly pinned there; managed IRQs avoid them too |
| `nohz_full=4-7` | turns off the periodic timer tick (e.g. 1000 Hz) on those CPUs while a single task runs |
| `rcu_nocbs=4-7` | moves RCU callback work (kernel housekeeping) off them |
| `irqaffinity=0-3,8-23` | default CPUs for hardware interrupts: everything except the isolated ones |

Isolate **whole physical cores** (both SMT siblings: 4-7 = cores 2 and 3), or
the sibling's activity still disturbs you.

Effect: scheduler/timer/interrupt jitter on those cores drops from tens of µs
to near zero. Cost: those CPUs sit idle unless you pin work to them.

Check afterwards:

```sh
cat /sys/devices/system/cpu/isolated      # 4-7
cat /sys/devices/system/cpu/nohz_full     # 4-7
```

**Make it permanent:** these are kernel command-line parameters, so they only
exist permanently: in `/etc/default/grub` followed by `sudo update-grub`
([section 12](#kernel-command-line-grub)). There is no runtime equivalent.

If `irqbalance` is running (`systemctl is-active irqbalance`), it moves
interrupts around at runtime and can undo `irqaffinity`. Either disable it
(`sudo systemctl disable --now irqbalance`) or tell it to avoid the isolated
CPUs by setting `IRQBALANCE_BANNED_CPULIST=4-7` in its environment file.

Related sysctls that reduce background jitter on isolated cores (put them in
`/etc/sysctl.d/`, see [section 12](#kernel-tunables-sysctld)):

```ini
# no periodic NMI watchdog interrupts
kernel.nmi_watchdog = 0
# refresh VM statistics every 10 s instead of every 1 s
vm.stat_interval = 10
```

---

## 4. Frequency and sleep states

Runtime settings, reversible, gone after reboot unless you persist them.

```sh
# Frequency governor
sudo cpupower frequency-set -g performance
#   (or: powerprofilesctl set performance, if power-profiles-daemon runs)

# Deep sleep states: waking from them costs tens to hundreds of µs
sudo cpupower idle-set -D 0               # disable idle states with latency > 0

# Turbo: ON for peak speed, OFF for reproducible benchmark numbers
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo   # 1 = turbo off
```

Notes:

* With the `intel_pstate` driver, the `powersave` governor plus energy
  performance preference (EPP) `performance` is already close to the
  `performance` governor. Check with:
  ```sh
  cat /sys/devices/system/cpu/cpu0/cpufreq/{scaling_driver,scaling_governor,energy_performance_preference}
  ```
* Turbo frequency depends on temperature, power budget and how many cores are
  busy, so it varies run to run. Great for speed, bad for comparing A vs B.
* `power-profiles-daemon`, `tuned` and manual `cpupower` calls all write the
  same knobs. Use one of them, or they overwrite each other.
* `/sys/devices/system/cpu/intel_pstate/min_perf_pct` is the lowest speed a
  core may drop to, as a percentage of the maximum. `100` means cores never
  clock down.

### The trap: maximum clock + no idle states = a hot, *slower* CPU

It is tempting to turn every dial to maximum: turbo on, `min_perf_pct = 100`,
and all sleep states off (POLL only). Then **every idle core runs at full
clock doing nothing**. Measured on the 12900K, 3 minutes after boot, load
average 0.35 (idle):

| Setting | Idle package temperature | Throttling while idle |
|---|---|---|
| POLL only + `min_perf_pct 100` + turbo | **84 °C** (above the 80 °C "high" mark) | 2,199 package throttle events, 9 s of throttled clock in 3 min |
| C1E allowed (`intel_idle.max_cstate=2`) + default `min_perf_pct` + turbo | **44-48 °C** | none (counter flat over a 30 s sample) |

A hot chip gets *less* turbo headroom, so the cores doing real work run
slower. And busy cores never idle anyway: a thread that busy-polls keeps its
own core awake and at full clock whatever the idle settings are. So:

* keep **governor + EPP `performance` and turbo on**: cores still ramp to full
  speed within milliseconds of work arriving;
* **allow a shallow sleep state** (C1/C1E) and leave `min_perf_pct` at its
  default, so idle cores rest and clock down (700-800 MHz here);
* reserve POLL-only for short, dedicated benchmark sessions, and watch the
  temperature while you do.

Watch temperatures and throttling with:

```sh
sensors | grep Package                                        # lm_sensors
grep . /sys/devices/system/cpu/cpu0/thermal_throttle/*        # counts since boot
```

`verify-perf-tuning.sh` (section 11) samples these counters to separate
"throttling right now" from bursts at boot, and warns about exactly this
combination.

**Make it permanent:**

* *Governor.* Easiest: `powerprofilesctl set performance`, which
  power-profiles-daemon remembers across reboots. Without the daemon, a
  tmpfiles.d line sets it at boot:
  ```ini
  # /etc/tmpfiles.d/performance.conf
  w /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor - - - - performance
  ```
  (Leave power-profiles-daemon running *and* use this, and whichever starts
  last wins. Pick one.)
* *Turbo:*
  ```ini
  # /etc/tmpfiles.d/performance.conf
  w /sys/devices/system/cpu/intel_pstate/no_turbo - - - - 1
  ```
  (Leave this line out for production/peak speed; turbo is on by default.)
* *C-states.* Either cap them on the kernel command line:
  ```text
  intel_idle.max_cstate=1
  ```
  **Check what that leaves**, because the count is easy to misjudge: on the
  12900K with kernel 7.2, `max_cstate=1` left **only `POLL`**, not C1, so idle
  cores busy-spin (lowest wake-up latency, maximum power and heat). If you
  want to keep the shallow sleep state, raise the number by one and check
  again (`max_cstate=2` gave `POLL C1E` on the 12900K; Alder Lake's shallowest
  sleep state is C1E):
  ```sh
  cat /sys/devices/system/cpu/cpu0/cpuidle/state*/name
  ```
  Or disable every idle state except polling with tmpfiles.d, the permanent
  equivalent of `cpupower idle-set -D 0` (same effect: cores never rest):
  ```ini
  # /etc/tmpfiles.d/performance.conf
  w /sys/devices/system/cpu/cpu*/cpuidle/state[1-9]*/disable - - - - 1
  ```

---

## 5. SMT (hyper-threading)

For the lowest and most consistent latency per thread, turn SMT off:

```sh
echo off | sudo tee /sys/devices/system/cpu/smt/control   # runtime; "on" to revert
```

(or `nosmt` on the kernel command line). Gentler alternative: keep SMT on, but
isolate both siblings of a core and run only one thread on it.

Trade-off: SMT off costs total throughput (typically 10-30% for mixed
workloads) in exchange for per-thread consistency. Remember the exception in
[section 2](#2-pin-hot-threads-to-the-right-cores): siblings are the *fastest*
pair for cache-line ping-pong.

**Make it permanent:** add `nosmt` to the kernel command line
([section 12](#kernel-command-line-grub)). Or, to keep it switchable at runtime:

```ini
# /etc/tmpfiles.d/performance.conf
w /sys/devices/system/cpu/smt/control - - - - off
```

---

## 6. Speculative-execution mitigations

The kernel protects against Spectre/Meltdown-class CPU bugs, mostly at kernel
entry/exit and context switches. That makes **syscalls and context switches
more expensive**, which matters for syscall-heavy code and for comparing
io_uring vs plain syscalls.

```sh
cat /sys/devices/system/cpu/vulnerabilities/*     # what is on
```

Kernel command line options:

```text
mitigations=off
reg_file_data_sampling=off
```

The first turns all of them off; the second turns off one specific
mitigation (the kernel's `admin-guide/kernel-parameters` documentation lists
one parameter per issue). Don't put comments in `GRUB_CMDLINE_LINUX_DEFAULT`:
everything inside the quotes becomes part of the kernel command line.

**This is a real security downgrade.** Only on a dedicated machine that runs
no untrusted code; a web browser counts as untrusted code. Keep microcode
updated regardless (section 1): mitigations are the *choice*, microcode is the
*capability*.

**Make it permanent:** kernel command line only
([section 12](#kernel-command-line-grub)). Mitigations cannot be switched at
runtime.

---

## 7. Memory

* **Huge pages.** Fewer TLB misses for large buffers.
  ```sh
  cat /sys/kernel/mm/transparent_hugepage/enabled   # [always] madvise never
  ```
  With `madvise`, request them per buffer with `madvise(addr, len, MADV_HUGEPAGE)`.
  For guaranteed huge pages, reserve them explicitly (`vm.nr_hugepages`,
  hugetlbfs).
* **Lock memory.** `mlockall(MCL_CURRENT | MCL_FUTURE)` stops hot memory being
  swapped out or faulted in mid-run.
* **Pre-fault.** Touch (write) buffers once before the timed section, or use
  `MAP_POPULATE`, so page faults don't land in the measurement.
* **NUMA** (multi-socket servers, not a single desktop CPU): keep threads and
  their memory on the same node (`numactl --cpunodebind=N --membind=N`).
* **Cache lines.** Put independently-written hot variables on separate 64-byte
  lines to avoid false sharing. See `05-lockfree/src/padded.rs`: on the 12900K
  false sharing made increments **~4.6x slower** (3.5 ns → 16 ns each).

**Make it permanent:**

* *Transparent huge pages mode:* kernel command line
  `transparent_hugepage=always` (or `madvise`), or
  ```ini
  # /etc/tmpfiles.d/performance.conf
  w /sys/kernel/mm/transparent_hugepage/enabled - - - - always
  ```
* *Reserved huge pages, swapping, NUMA balancing:* sysctl
  ```ini
  # /etc/sysctl.d/99-performance.conf
  # 512 x 2 MiB = 1 GiB reserved at boot
  vm.nr_hugepages = 512
  # prefer dropping page cache over swapping
  vm.swappiness = 10
  # multi-socket only: stop automatic page migration
  kernel.numa_balancing = 0
  ```
  Reserving at boot is more reliable than later: memory fragments over time,
  and huge pages need contiguous physical memory.
* *Allow `mlockall` for a normal user.* Locked memory is capped by
  `RLIMIT_MEMLOCK`, which is small by default:
  ```text
  # /etc/security/limits.d/99-memlock.conf   (applies at next login)
  youruser  -  memlock  unlimited
  ```
  For a systemd service, use `LimitMEMLOCK=infinity` in its `[Service]`
  section instead.

---

## 8. Waiting well: spin, backoff, yield, block

How a thread waits when a queue is empty or a lock is busy matters as much as
any kernel knob.

| Strategy | Cost per wait | CPU while waiting | Use when |
|---|---|---|---|
| `spin_loop()` / x86 `pause` | tens of ns | 100% | the wait is expected to be very short, **and** there is no lock to fight over (e.g. SPSC ring) |
| Spin with backoff (growing pauses) | grows | 100% | contended locks: stops waiters hammering the lock's cache line |
| `yield_now()` / `sched_yield(2)` | ~1 µs syscall | 100% if the core is otherwise idle | oversubscribed systems (more threads than cores) |
| Block (`Condvar`, futex, channel) | wake-up ~µs | 0% | waits may be long; you care about CPU usage or power |

Measured on the 12900K with a `Mutex<VecDeque>` queue (10M items): replacing
`spin_loop()` with `yield_now()` made it **~2x faster**. Not because yielding
is magic: with a free core, `sched_yield` returns immediately, and the ~1 µs it
takes acts as a *backoff* that lets the other thread hold the lock
uncontended. It is still a syscall per wait and still burns the core; a
`Condvar` is the proper blocking design, and a lock-free ring removes the lock
entirely.

---

## 9. One-shot presets: tuned and RT kernels

* **tuned** (Arch/Manjaro: `extra/tuned`) applies profiles that bundle most of
  sections 4-5:
  ```sh
  sudo pacman -S tuned
  sudo systemctl enable --now tuned
  sudo tuned-adm profile latency-performance
  ```
  It conflicts with `power-profiles-daemon`; run one, not both. **Permanent by
  design:** the enabled service reapplies the active profile at every boot.
  Custom profiles live in `/etc/tuned/<name>/tuned.conf` and can include the
  sysctl and sysfs settings from this document, replacing the hand-written
  files in section 12.
* **Real-time kernel** (Manjaro ships `linuxXX-rt` packages): `PREEMPT_RT`
  makes almost everything in the kernel preemptible. It improves **worst-case
  latency / determinism**, not throughput, and usually lowers throughput
  slightly. Worth it for real-time audio, control loops, trading-style tail
  latency; not for "make my benchmark faster". Permanent once installed: pick
  it in the GRUB menu, or make it the default entry (`GRUB_DEFAULT` in
  `/etc/default/grub`, then `sudo update-grub`).

---

## 10. BIOS / firmware

Written for the worked-example machine: **MSI PRO Z690-A WIFI (MS-7D25)**,
DDR5, **MSI Click BIOS 5**. Other boards have the same settings under
different names.

### Identify the board and BIOS from Linux

```sh
cat /sys/class/dmi/id/{board_vendor,board_name,bios_version,bios_date}
```

```text
Micro-Star International Co., Ltd.
PRO Z690-A WIFI (MS-7D25)
A.70
06/23/2022
```

### Getting around Click BIOS 5

| Key | Does |
|---|---|
| **Del** (during power-on) | enter BIOS setup |
| **F7** | switch between EZ Mode and Advanced Mode (use Advanced) |
| **F10** | save and exit |
| **F11** (during power-on) | one-off boot menu |

MSI moves items between menus from one BIOS version to the next. The
locations below are the usual ones for Z690 Click BIOS 5; if an item isn't
there, look in the neighbouring OC sub-menus. The last column checks the
result from Linux either way.

### Settings

| Setting | Where (Advanced Mode) | Set to | Check from Linux |
|---|---|---|---|
| **BIOS version** | **M-FLASH** (flash from a FAT32 USB stick) | the latest release for *PRO Z690-A WIFI (MS-7D25)* from MSI's support page; match the exact model name, DDR5 variant | `cat /sys/class/dmi/id/bios_{version,date}` (A.70 from 2022 here) |
| **Power limits** | **OC → CPU Cooler Tuning** (a preset: boxed / tower air / water cooler), then **OC → Advanced CPU Configuration → Long / Short Duration Power Limit (W)** to see or set the values | Long = Short, sized to your cooler. Intel's reference for the 12900K is 241 W; this board currently has **288 W / 288 W** | `d=/sys/class/powercap/intel-rapl:0; for i in 0 1; do echo "$(cat $d/constraint_${i}_name) $(( $(cat $d/constraint_${i}_power_limit_uw)/1000000 )) W"; done` |
| **Memory speed** | **EZ Mode: XMP button**, or **OC → Extreme Memory Profile (X.M.P)** | Profile 1 (the rated speed printed on the DIMMs) | `sudo dmidecode -t memory \| grep -E "Configured Memory Speed\|Part Number"` |
| **Turbo** | **OC → Advanced CPU Configuration → Intel Turbo Boost** | Enabled (switch it off per session from Linux instead, section 4) | `cat /sys/devices/system/cpu/cpu4/cpufreq/cpuinfo_max_freq` → `5200000` (5.2 GHz favoured core) |
| **Speed Shift (HWP)** | **Advanced CPU Configuration → Intel Speed Shift Technology** | Enabled (it is what makes EPP work) | `grep -m1 -ow hwp_epp /proc/cpuinfo` |
| **C-states** | **Advanced CPU Configuration → Intel C-State, C1E Support, Package C State Limit** | leave on **Auto/Enabled**; control them from Linux (`intel_idle.max_cstate`, section 4) | `cat /sys/devices/system/cpu/cpu0/cpuidle/state*/name` |
| **Hyper-Threading** | **Advanced CPU Configuration → Hyper-Threading** | leave **Enabled**; switch SMT from Linux (`smt/control`, section 5) | `lscpu \| grep -E "Thread\|Off-line"` |
| **E-cores** | **Advanced CPU Configuration → Active Efficient Cores** (P-cores: *Active Performance Cores*). Leave **Legacy Game Compatibility Mode** off: it lets Scroll Lock park the E-cores | **All** normally; **0** only for single-core-type benchmarking | `lscpu -e=CPU,CORE,MAXMHZ` (E-cores show 3900 MHz) |
| **Fans** | **HARDWARE MONITOR**: pick **CPU_FAN1** / **PUMP_FAN1** (and the case fans), *Smart Fan Mode*, temperature source **CPU**, drag the curve points | curve that ramps from ~70 °C and reaches full speed by ~85 °C | `sensors \| grep Package` under load; `thermal_throttle` counters (section 4) |

**Why C-states, SMT and turbo stay "on" in the BIOS.** Anything disabled in
the BIOS is gone for Linux: a C-state the BIOS removes cannot be re-enabled
with `intel_idle.max_cstate`, and SMT disabled there cannot be switched back
on with `smt/control`. Leaving them on in the BIOS and choosing in Linux keeps
every choice reversible without entering setup. The BIOS is for what only it
can do: power limits, memory profile, enabling cores, and fans.

### Is the BIOS fine as it is? Worked verdict for this machine

Checked from Linux, then under a sustained full load:

| Item | Result | How it was checked |
|---|---|---|
| Memory | ✅ XMP already on: G.Skill F5-5600J3636D32G (DDR5-5600, CL36) runs at its rated 5600 MT/s | `sudo dmidecode -t memory \| grep -E "Configured Memory Speed\|^\s+Speed\|Part Number"`: *Configured Memory Speed* equals *Speed* |
| Turbo, Speed Shift, C-states, Hyper-Threading, E-cores | ✅ fine | 5.2 GHz max on cpu4, `hwp_epp` present, C1E available, SMT switchable, 16 cores |
| **Power limit 288 W / 288 W** | ❌ **too high for the cooler** | full load: package at **100 °C** (its critical mark), throttled **40-47 %** of the time |
| BIOS version A.70 (2022) | ⚠ old; update optional | not needed for performance (Linux loads newer microcode), worth it for fixes |

The load test:

```sh
stress-ng --cpu 16 --timeout 120 &
THROTTLE_SAMPLE_S=30 ./verify-perf-tuning.sh -q
```

```text
WARN  throttling NOW: package throttled 1414 ms of 3000 ms (47%), ... package now 100°C
```

(If DDR5 shows a *Configured Memory Speed* below its rated *Speed*, typically
4800 MT/s, XMP is off: turn on Profile 1. That is free performance, especially
for memory-bound code.)

At 288 W the CPU spends almost half its time slowing itself down to survive.
A lower limit usually gives the **same or higher sustained throughput** and
runs 10-20 °C cooler. So the one BIOS change this machine needs is the power
limit, and the right value is measured, not guessed.

### Finding the right power limit (from Linux, before touching the BIOS)

The package power limits can be set live through `intel-rapl` in sysfs (root
only). They revert to the BIOS values at reboot, so experimenting is safe.
This loop tries four limits and measures real throughput (stress-ng's
*bogo ops/s*), time spent throttled and temperature:

```sh
R=/sys/class/powercap/intel-rapl:0
T=/sys/devices/system/cpu/cpu0/thermal_throttle/package_throttle_total_time_ms
for w in 288 241 200 170; do
  echo $((w * 1000000)) | sudo tee $R/constraint_0_power_limit_uw $R/constraint_1_power_limit_uw >/dev/null
  sleep 30                                   # let it cool between runs
  t0=$(cat $T)
  ops=$(stress-ng --cpu 14 --timeout 60 --metrics-brief 2>&1 | awk '/metrc:.* cpu /{print $9}')
  printf "%3s W: %8s bogo-ops/s  throttled %5d ms/60s  package %s\n" \
    "$w" "$ops" $(( $(cat $T) - t0 )) "$(sensors | awk '/^Package/{print $4}')"
done
```

* `constraint_0` is PL1 (long duration), `constraint_1` is PL2 (short duration);
  both get the same value, keeping PL1 = PL2.
* `--cpu 14` matches the 14 online CPUs that are *not* isolated (the
  scheduler won't place stress workers on isolated CPUs 4 and 6).
* Column 9 of stress-ng's `metrc:` line is *bogo ops/s (real time)*.
* If a written value doesn't read back (`cat $R/constraint_0_power_limit_uw`),
  the BIOS has locked the limit; test by changing it in the BIOS instead.

**Pick the lowest wattage whose bogo-ops/s is within a few percent of the
best**, ideally with near-zero throttled time. Then:

1. **Make it permanent** in the BIOS (step by step in
   [Setting the power limit in the BIOS](#setting-the-power-limit-in-the-bios)
   below).
2. **Make the CPU fan/pump curve steeper** in **HARDWARE MONITOR**. If the
   fans aren't at full speed by ~85 °C, fixing the curve alone may allow a
   higher limit; re-run the loop after changing it.
3. **Re-check under load:**
   ```sh
   stress-ng --cpu 16 --timeout 120 &
   THROTTLE_SAMPLE_S=30 ./verify-perf-tuning.sh -q
   ```
   Target: no *throttling NOW* warning, package below ~95 °C.

### Results on this machine: 170 W

The loop above, on the 12900K with its air cooler:

| Limit | Run 1 (bogo-ops/s) | Run 2 (bogo-ops/s) | Throttled per 60 s | Package right after run 2 |
|---|---|---|---|---|
| 288 W | 29,795 | 29,975 | 26.6-26.7 s | **100 °C** |
| 241 W | 29,594 | 29,744 | 27.2-27.4 s | **100 °C** |
| 200 W | 29,490 | 29,610 | 26.9-27.6 s | 75 °C |
| 170 W | 29,412 | 29,629 | 25.3-26.4 s | 76 °C |

Two runs, same conclusion: 170 W is within 1.2-1.3 % of 288 W. The
temperature column is read just *after* each run, so it is a lower bound, but
it still separates the two groups: at 288 and 241 W the package was still at
its 100 °C ceiling, at 200 and 170 W it had already fallen to 75-76 °C. The
throttled time stays at ~26 s per minute at *every* limit: those are the
per-core hot spots explained below, which no power limit removes.

**Cutting the limit by 41 % cost 1.3 % throughput.** turbostat shows why:

```sh
sudo turbostat --quiet --show PkgWatt,PkgTmp,Busy%,Bzy_MHz stress-ng --cpu 14 --timeout 30
```

At 170 W the package draws exactly the limit (`PkgWatt 169.92`, so the limit
applies), yet the P-cores still hold **~4.93-4.97 GHz** and the E-cores
**3.86 GHz**: essentially full all-core turbo. The last few hundred MHz above
that cost disproportionate power (power rises much faster than clock speed at
the top of the range), so the extra 118 W at 288 W bought heat, a 100 °C
package and throttling, not speed. **170 W is the setting.**

#### Reading per-core temperatures

Why did the throttle counter still record ~25 s of throttling per minute at
170 W, with no loss of throughput? Per-core temperatures under load answer it:

```sh
stress-ng --cpu 14 --timeout 30 &
sudo turbostat --quiet --show Core,CoreTmp,PkgTmp,PkgWatt --interval 5
```

| Core (turbostat ID) | CPUs | Peak at 170 W |
|---|---|---|
| 0x8, 0xc (P-cores 2, 3) | 4, 6 (isolated, idle) | 69-82 °C |
| 0x0, 0x4 (P-cores 0, 1) | 0, 2 | 78-86 °C |
| **0x10-0x1c (P-cores 4-7)** | 8-14 | **89-100 °C** |
| 0x20-0x27 (E-core clusters) | 16-23 | 81-87 °C |

* **The throttling is hot spots.** A few P-cores repeatedly reach 100 °C and
  briefly slow down; the package average looks fine. Those are the events the
  counter records.
* **A 20-25 °C spread between cores doing identical work is unusually
  large.** Some spread is normal; this much, consistently on the same cores,
  usually means **uneven contact between the chip and the cooler**.
* **Within 5 s of the load stopping, everything fell to 69 °C, then 46 °C.** A
  fast drop means the heat was stuck between the chip and the cooler (paste,
  mounting pressure, the chip's lid), not in the cooler. An air cooler's
  heatsink takes *minutes* to fill up, though, so to judge sustained load, run
  a 10-minute test:
  ```sh
  stress-ng --cpu 14 --timeout 600 &
  sudo turbostat --quiet --show Core,CoreTmp,PkgTmp,PkgWatt --interval 30
  ```
  Temperatures that **keep climbing** point at the heatsink, fans or case
  airflow; temperatures that **plateau with the same cores 20 °C hotter**
  point at contact.

#### Fixing uneven contact (air cooler)

In order of likelihood:

1. **LGA1700 mounting kit.** LGA1700 uses a wider hole spacing than earlier
   Intel sockets (78 mm vs 75 mm) and the chip sits lower. Many air coolers
   from before 2022 need a separate LGA1700 kit; mounted on the old holes or
   standoffs, pressure is low or uneven. Check the cooler maker's LGA1700
   compatibility page.
2. **Re-seat, and read the old paste first.** Evenly thin everywhere: contact
   was fine. Thick at the edges and thin in the middle, or bare patches:
   uneven pressure or a bowed lid. Clean both surfaces, apply fresh paste,
   tighten the screws diagonally in small steps until they stop (the maker's
   torque, no more).
3. **LGA1700 contact frame.** Replaces the socket's retention bracket, which
   is known to bow 12th/13th-gen CPU lids slightly so the cooler base doesn't
   sit flat. Cheap; users commonly report a few to ~10 °C lower temperatures
   and a more even spread. Fit it during step 2 to disassemble once.
4. **Fan curve and airflow.** CPU_FAN1 near 100 % by ~80 °C; tower fan
   blowing toward a rear exhaust; enough intake.

Success after the hardware work: loaded P-cores within ~10 °C of each other
and none at 100 °C at 170 W. A higher limit is then possible, though the
throughput table suggests little to gain above 170 W.

### Setting the power limit in the BIOS

On the PRO Z690-A WIFI (Click BIOS 5, A.70), about two minutes:

1. Reboot and tap **`Del`** at the MSI logo.
2. In EZ Mode, press **`F7`** for Advanced Mode.
3. *(Safety net)* **OC PROFILE**: save the current settings to a slot.
4. **OC** (left) → **Advanced CPU Configuration** → `Enter`.
5. **Long Duration Power Limit (W)**: type `170`, `Enter`.
   **Short Duration Power Limit (W)**: type `170`, `Enter`.
   Leave everything else (*Long Duration Maintained*, *CPU Current Limit*)
   as it is.
6. **`F10`** → **Yes** to save and reboot.

Can't find it? On some versions the two lines sit in another OC sub-menu
(for example *CPU Features*). Something went wrong? Enter setup again and
press **`F6`** (optimized defaults), or load the profile from step 3.

Verify from Linux. A reboot resets anything written through sysfs, so reading
170 here proves the BIOS setting:

```sh
d=/sys/class/powercap/intel-rapl:0
for i in 0 1; do echo "$(cat $d/constraint_${i}_name) $(( $(cat $d/constraint_${i}_power_limit_uw)/1000000 )) W"; done
```

```text
long_term 170 W
short_term 170 W
```

and under load (PkgWatt should settle at ~170):

```sh
sudo turbostat --quiet --show PkgWatt,PkgTmp,Bzy_MHz stress-ng --cpu 14 --timeout 30
```

**Alternative without the BIOS:** apply it from Linux at every boot through
tmpfiles.d ([section 12](#files-under-sys-tmpfilesd)); `verify-perf-tuning.sh`
then checks it automatically:

```ini
# /etc/tmpfiles.d/performance.conf
w /sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw - - - - 170000000
w /sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw - - - - 170000000
```

The BIOS route is still preferable: it applies from power-on, before Linux
starts, and for any OS booted.

### What the machine draws now

With the final settings: 170 W package limit; governor, EPP and turbo on
`performance`; `intel_idle.max_cstate=2` (POLL + C1E).

| Situation | Behaviour | CPU energy |
|---|---|---|
| Full all-core load | capped at 170 W instead of 288 W, for 98.8 % of the throughput | **~40 % less power for the same work** |
| Light or single-threaded work | a few busy cores draw far below 170 W, so the cap never applies; EPP `performance` ramps them to **5.1-5.2 GHz** within milliseconds | top frequency when needed; slightly more than the default `balance_performance` on light work (the price of instant ramp-up) |
| Idle | cores clock down to ~800 MHz and sleep in C1E | **~13-14 W** package (measured) |

**170 W is the CPU package only**, averaged over time (the long-term limit
averages over ~56 s, the short-term one over ~2.4 ms, so brief spikes above
it are normal). RAPL measures the package, its cores and the "uncore"
(cache, memory controller), nothing else in the box:

| Part | Max draw | Idle (measured) |
|---|---|---|
| CPU package (i9-12900K) | 170 W (limit) | ~13-14 W |
| GPU (GTX 1660 SUPER) | 125 W (its power limit) | ~13 W |
| Motherboard, chipset, Wi-Fi | ~15-30 W (estimate) | |
| CPU voltage regulators | ~10-15 % on top of CPU power | |
| RAM (2x DDR5), 2 NVMe SSDs, sound card, fans, USB | ~25-45 W together (estimate) | |
| Power-supply losses | ~8-12 % of everything | |

Rough whole-machine totals at the wall: **~400-450 W** with CPU and GPU both
flat out, **~230-260 W** with only the CPU loaded, **~60-80 W** idle. Only a
plug-in wall power meter sees the whole machine including PSU losses; from
software you can see the two biggest parts:

```sh
sudo turbostat --quiet --show PkgWatt --interval 5                 # CPU package
nvidia-smi --query-gpu=power.draw --format=csv,noheader -l 5       # NVIDIA GPU
```

(turbostat keeps running until Ctrl+C; use two terminals to watch both.)

**Idle and the C-state cap.** `max_cstate=2` blocks the deep core sleep states
(C6 and beyond), which are also what let the whole package drop into its own
low-power states. That buys faster wake-ups at some idle-power cost. On this
machine the package idles at ~13-14 W with the cap, low enough that the cap
stays. To measure what it costs on another machine: remove
`intel_idle.max_cstate=...` from GRUB, `sudo update-grub`, reboot, idle for a
minute, and compare:

```sh
sudo turbostat --quiet --show PkgWatt,Busy% --interval 10 --num_iterations 3
```

If the package drops well below the capped figure, prefer the deep states for
everyday use and disable them only per latency-critical session
(`sudo cpupower idle-set -D 0`, reverted at reboot).

### After a BIOS update

* **Every setting resets to default.** Before flashing, save your settings in
  **OC PROFILE** (to a profile slot or a USB stick) and reload them afterwards,
  or go through the table above again. Re-check the power limits and XMP:
  defaults differ between versions.
* **The boot entry may disappear.** A BIOS update can wipe the UEFI boot list.
  If the machine doesn't find Manjaro, press **F11** at power-on and pick the
  disk. If GRUB is gone from the list for good, reinstall it from a Manjaro
  live USB (`manjaro-chroot -a`, then `grub-install` and `update-grub`).
* **Microcode:** a new BIOS carries newer microcode, but Linux still loads the
  `intel-ucode` package version on every boot (section 1), whichever is newer.
* Run `./verify-perf-tuning.sh` afterwards; it confirms the Linux side is
  unchanged.

### What cools the CPU (and why there is no thermald on a desktop)

| Layer | Who does it |
|---|---|
| Fans and AIO pump | The **motherboard firmware**: a fan-controller chip on the board reads the CPU temperature and follows the fan curves set in the BIOS (on MSI boards: *Hardware Monitor*). Works with no OS involvement. |
| CPU self-protection | The CPU lowers its own clock when it gets too hot. This is what the `thermal_throttle` counters count. |
| Emergency stop | The CPU forces a power-off if it overheats anyway. |

`thermald` is **laptop software**: it slows the CPU down *before* the hardware
has to, to keep a thin machine cool and quiet (laptop fans are usually run by
the embedded controller's firmware). On a desktop it exits by design
(`Non mobile platform, exiting..`), and you would not want it in a
performance setup anyway. Disable it if it is enabled:
`sudo systemctl disable --now thermald`.

If the CPU runs hot under load, make the CPU fan/pump curve steeper (for
example ramping from ~70 °C) before lowering power limits. Linux usually
cannot see desktop fans until the board's fan-controller driver is loaded
(MSI Z690 boards typically use a Nuvoton NCT6687; try `sudo modprobe
nct6683`, or the out-of-tree `nct6687d` driver). That is only needed to watch or
control fans from Linux; the BIOS already runs them.

---

## 11. Measuring and verifying

**Check the configuration first.** [`verify-perf-tuning.sh`](verify-perf-tuning.sh)
(repo root, read-only, no root needed) compares what your config files ask for
(`/etc/default/grub`, `/etc/sysctl.d`, `/etc/tmpfiles.d`, `limits.d`) with the
running system. It reports PASS/WARN/FAIL for microcode, command-line
parameters, isolation (including IRQs and threads still on isolated CPUs),
temperature and throttling (with a live sample, plus a warning for the section 4
trap),
idle states, frequency owners, sysctls, huge pages, memlock and mitigations.
Run it after every change and reboot:

```sh
./verify-perf-tuning.sh        # everything
./verify-perf-tuning.sh -q     # only WARN and FAIL; exit status 1 if any FAIL
```

Then measure the effect:

```sh
sudo turbostat --quiet --interval 1           # per-core real frequency, C-state residency, power
perf stat -e cycles,instructions,cache-misses,context-switches,cpu-migrations ./bench
perf stat -r 10 ./bench                       # 10 runs, reports mean and variance
```

Benchmark hygiene:

* Pin (`taskset`), and use the same cores for A and B.
* Turbo off (or at least fixed) when comparing.
* Warm up once, then take the **best** or the **median** of several runs;
  noise only ever makes a run slower.
* Watch `cpu-migrations` and `context-switches` in `perf stat`: they should be
  ~0 for a well-pinned, isolated run.
* Change one thing at a time.

**Make it permanent:** let `perf` read hardware counters without root:

```ini
# /etc/sysctl.d/99-performance.conf
kernel.perf_event_paranoid = 1
```

---

## 12. Making settings permanent

Every knob in this document lives in one of six places. Pick the mechanism
by *what kind* of setting it is:

| Kind of setting | Where it lives | Applied | Apply now / check |
|---|---|---|---|
| Kernel boot parameters (`isolcpus`, `nohz_full`, `nosmt`, `mitigations`, `intel_idle.max_cstate`, `dis_ucode_ldr`) | `/etc/default/grub` | at boot, before anything runs | reboot; `cat /proc/cmdline` |
| Kernel tunables under `/proc/sys` (`vm.*`, `kernel.*`) | `/etc/sysctl.d/*.conf` | early boot (`systemd-sysctl`) | `sudo sysctl --system`; `sysctl <name>` |
| Files under `/sys` (governor, turbo, SMT, THP, idle states) | `/etc/tmpfiles.d/*.conf` | early boot (`systemd-tmpfiles-setup`) | `sudo systemd-tmpfiles --create /etc/tmpfiles.d/performance.conf`; `cat <file>` |
| Per-service CPU pinning, memory-lock limit | the service's `.service` file | when the service starts | `sudo systemctl daemon-reload && sudo systemctl restart <svc>` |
| Per-user limits (`memlock`) | `/etc/security/limits.d/*.conf` | at login | log out and in; `ulimit -l` |
| Power limits, XMP, E-cores | BIOS / UEFI setup | power-on | `turbostat`, `lscpu` |

> **Comments go on their own line.** In sysctl.d, tmpfiles.d, systemd unit
> files and `/etc/default/grub`, a `#` only starts a comment at the *beginning*
> of a line. `vm.swappiness = 10  # note` sends `10  # note` to the kernel
> (numeric keys happen to tolerate it, text keys don't), and a `#` inside the
> GRUB quotes lands on the kernel command line.

(A tuned profile, [section 9](#9-one-shot-presets-tuned-and-rt-kernels), can
replace the sysctl and tmpfiles files with one managed profile.)

### Kernel command line (GRUB)

```sh
sudoedit /etc/default/grub
```

Add parameters, separated by spaces, inside the quotes of
`GRUB_CMDLINE_LINUX_DEFAULT`. Keep what is already there (on an encrypted
Manjaro install that includes `cryptdevice=...` and `root=...`; removing them
makes the system unbootable). Example:

```sh
GRUB_CMDLINE_LINUX_DEFAULT="<existing parameters> isolcpus=domain,managed_irq,4-7 nohz_full=4-7 rcu_nocbs=4-7 irqaffinity=0-3,8-23 intel_idle.max_cstate=1"
```

Then regenerate the GRUB config and reboot:

```sh
sudo update-grub      # Manjaro/Debian wrapper for grub-mkconfig -o /boot/grub/grub.cfg
reboot
cat /proc/cmdline     # confirm the parameters are there
```

If a parameter makes the system misbehave: at the GRUB menu press `e` on the
entry, delete it from the `linux` line, and boot with Ctrl+X. That change is
one-off; fix `/etc/default/grub` afterwards.

(`GRUB_CMDLINE_LINUX` also exists: it applies to recovery entries too. Tuning
belongs in `_DEFAULT` so the recovery entry stays a clean fallback.)

### Kernel tunables (sysctl.d)

One file collecting everything from this document:

```ini
# /etc/sysctl.d/99-performance.conf
kernel.nmi_watchdog = 0
vm.stat_interval = 10
vm.swappiness = 10
vm.nr_hugepages = 512
kernel.perf_event_paranoid = 1
# kernel.numa_balancing = 0     # multi-socket machines only
```

```sh
sudo sysctl --system            # apply every sysctl.d file now
sysctl vm.swappiness            # check one value
```

Files are read in name order and later ones win; `99-` makes yours last.
Don't edit `/etc/sysctl.conf` directly on Arch/Manjaro: the `sysctl.d`
directory is the maintained mechanism.

### Files under /sys (tmpfiles.d)

`/sys` is rebuilt by the kernel at every boot, so writing to it with `echo`
lasts only until reboot. systemd-tmpfiles re-applies writes at boot. Format:
`w <path> - - - - <value>` (the four dashes are mode, user, group, age, unused
for writes); paths may contain globs.

```ini
# /etc/tmpfiles.d/performance.conf
w /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor - - - - performance
w /sys/devices/system/cpu/intel_pstate/no_turbo          - - - - 1
w /sys/kernel/mm/transparent_hugepage/enabled            - - - - always
# w /sys/devices/system/cpu/smt/control                  - - - - off
# w /sys/devices/system/cpu/cpu*/cpuidle/state[1-9]*/disable - - - - 1
```

```sh
sudo systemd-tmpfiles --create /etc/tmpfiles.d/performance.conf   # apply now
cat /sys/devices/system/cpu/intel_pstate/no_turbo                  # check
```

Daemons that manage the same files (power-profiles-daemon, tuned) start
*after* tmpfiles and overwrite them. Use one owner per knob. Concretely: with
power-profiles-daemon running, drop the `scaling_governor` line. On
`intel_pstate` its `performance` profile leaves the governor at `powersave`
but sets EPP to `performance`, which behaves almost the same.

### Per service and per user (systemd, limits.d)

```ini
# /etc/systemd/system/mybench.service
[Unit]
Description=Pinned benchmark

[Service]
ExecStart=/usr/local/bin/mybench
# run only on these CPUs
CPUAffinity=4 5
# allow mlockall()
LimitMEMLOCK=infinity
# higher scheduling priority
Nice=-10

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now mybench
```

For programs you start by hand, per-user limits go in
`/etc/security/limits.d/` (see [section 7](#7-memory)).

### Undoing

Delete the line (or the whole file) and reboot. For GRUB, run
`sudo update-grub` after editing. Nothing here changes the disk or firmware
permanently except BIOS settings.

---

## 13. Checklist

In rough order of benefit per effort:

| # | Step | Reboot? | Made permanent in | Risk |
|---|---|---|---|---|
| 1 | Microcode up to date | yes | package + GRUB (automatic) | none |
| 2 | Pin hot threads to P-cores (`taskset`) | no | service `CPUAffinity=` | none |
| 3 | Governor `performance`, deep C-states off (keep C1/C1E) | no | power-profiles-daemon or tmpfiles.d; `intel_idle.max_cstate=N` in GRUB (check which states remain) | more power/heat; POLL-only + `min_perf_pct 100` throttles an idle CPU (section 4) |
| 4 | Turbo off (benchmarks) / on (production) | no | tmpfiles.d | none |
| 5 | `isolcpus` + `nohz_full` + `rcu_nocbs` + `irqaffinity` | yes | GRUB only | isolated CPUs idle unless used |
| 6 | Pre-fault / `mlockall` / huge pages | no | app code; sysctl.d; limits.d | memory use |
| 7 | Right waiting strategy (section 8) | no | app code | none |
| 8 | SMT off | no | `nosmt` in GRUB or tmpfiles.d | lower total throughput |
| 9 | `mitigations=off` | yes | GRUB only | **security** |
| 10 | BIOS power limits (measured: 170 W here), XMP, E-cores off | yes | BIOS (power limit also via tmpfiles.d) | heat / stability |

Everything is reversible: delete the line or file, then reboot (plus
`sudo update-grub` for GRUB changes).

For most benchmarking, **2 + 3 + 4 + 5** gets you roughly 90% of the benefit.
