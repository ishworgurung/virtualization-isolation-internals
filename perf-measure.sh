#!/usr/bin/env bash
# perf-measure.sh: A/B benchmark for kernel-level performance changes, built
# for "mitigations=off vs on" but usable for any boot-time change.
#
#   ./perf-measure.sh run [LABEL]      measure; LABEL defaults to mitigations-on/-off
#   ./perf-measure.sh compare A.tsv B.tsv   side-by-side, B relative to A
#   ./perf-measure.sh list             show saved results
#
# Workflow: boot config A -> run; reboot into config B -> run; then compare.
#
# What is measured (each test REPS times; median is reported):
#   kernel  : syscalls, context switches, page faults, fork/exec, pipes, futex,
#             signals, io_uring vs read()  -- where mitigations cost the most
#   kvm     : VM-exit costs and virtio kick round trips (02-hypercalls,
#             09-kick-roundtrip) -- Spectre v2 / VMSCAPE mitigations hit VM exits
#   real    : shell fork+exec loop, tar of /usr/share/man, repeated gcc compiles
#   control : pure compute and memcpy -- should NOT change; if they do, the two
#             runs are not comparable (turbo, temperature, background load)
#
# Pinning: single-threaded tests run on the first online *isolated* CPU (quiet:
# no other tasks, no tick), multi-threaded ones on two non-isolated P-cores.
# Override with CPU=4 PAIR=0,2. Other knobs: REPS=5 STRESS_S=5 QUICK=1.
#
# Results: perf-results/<label>-<timestamp>.tsv (raw samples + environment).
# Needs: perf, stress-ng, python3; KVM tests need /dev/kvm and the built labs
# (make -C 02-hypercalls -C 09-kick-roundtrip -C 04-io_uring). No root.

set -u
shopt -s nullglob
cd "$(dirname "$0")" || exit 1
ROOT=$PWD
OUTDIR=$ROOT/perf-results
SYS=/sys/devices/system/cpu

REPS=${REPS:-5}
STRESS_S=${STRESS_S:-5}
if [[ ${QUICK:-0} == 1 ]]; then REPS=${REPS_QUICK:-3}; STRESS_S=2; fi

die() { echo "error: $*" >&2; exit 1; }
expand_cpus() {
  local part out=() a b i
  IFS=, read -ra parts <<<"$1"
  for part in "${parts[@]}"; do
    if [[ $part =~ ^([0-9]+)-([0-9]+)$ ]]; then
      a=${BASH_REMATCH[1]} b=${BASH_REMATCH[2]}
      for ((i = a; i <= b; i++)); do out+=("$i"); done
    elif [[ $part =~ ^[0-9]+$ ]]; then out+=("$part"); fi
  done
  echo "${out[*]:-}"
}
now_ns() { date +%s%N; }

# ----------------------------------------------------------------------------- compare
compare() {
  [[ $# == 2 && -r $1 && -r $2 ]] || die "usage: $0 compare A.tsv B.tsv"
  python3 - "$1" "$2" <<'PY'
import sys, statistics as st, math

def load(path):
    env, rows = {}, {}
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("# "):
            k, _, v = line[2:].partition(": ")
            env[k] = v
        elif line and not line.startswith("#"):
            test, group, unit, better, samples = line.split("\t")
            xs = [float(x) for x in samples.split(",") if x]
            if xs:
                rows[test] = (group, unit, better, xs)
    return env, rows

(ea, ra), (eb, rb) = load(sys.argv[1]), load(sys.argv[2])
la, lb = ea.get("label", "A"), eb.get("label", "B")
tty = sys.stdout.isatty()
c = (lambda code, s: f"\033[{code}m{s}\033[0m") if tty else (lambda code, s: s)

print(c("1", f"A = {la}  ({sys.argv[1]})"))
print(c("1", f"B = {lb}  ({sys.argv[2]})"))
print()
print(c("1", "Environment differences (anything besides the change under test makes the comparison less fair):"))
diffs = [k for k in sorted(set(ea) | set(eb)) if k not in ("label", "date", "file") and ea.get(k) != eb.get(k)]
def items(k, v):
    # Long list-valued keys: show only what differs.
    if k == "cmdline":
        return set(v.split())
    if k == "vulnerabilities":
        return {x.strip() for x in v.split(";") if x.strip()}
    return None
for k in diffs:
    a, b = ea.get(k, "-"), eb.get(k, "-")
    ia, ib = items(k, a), items(k, b)
    if ia is not None:
        print(f"  {k}:")
        for x in sorted(ia - ib):
            print(f"    only A: {x}")
        for x in sorted(ib - ia):
            print(f"    only B: {x}")
    else:
        print(f"  {k}:\n    A: {a}\n    B: {b}")
if not diffs:
    print("  none")
print()

def spread(xs):
    m = st.median(xs)
    return (max(xs) - min(xs)) / m * 100 if m else 0

hdr = f"{'test':<34} {'unit':<9} {'A median':>12} {'B median':>12} {'B vs A':>9}  verdict"
groups = {}
for t in ra:
    if t in rb:
        groups.setdefault(ra[t][0], []).append(t)
ratios = {}
for g in ["kernel", "kvm", "real", "control"] + sorted(set(groups) - {"kernel", "kvm", "real", "control"}):
    if g not in groups:
        continue
    print(c("1", f"[{g}]"))
    print(hdr)
    for t in groups[g]:
        _, unit, better, xa = ra[t]
        xb = rb[t][3]
        ma, mb = st.median(xa), st.median(xb)
        if ma == 0 or mb == 0:
            continue
        # slowdown factor > 1 means B is worse, whatever the unit direction
        slow = mb / ma if better == "lower" else ma / mb
        ratios.setdefault(g, []).append(slow)
        pct = (slow - 1) * 100
        noise = max(spread(xa), spread(xb)) / 2
        if abs(pct) <= max(noise, 1.0):
            verdict = c("2", "~ same (within noise)")
        elif pct > 0:
            verdict = c("31", f"B {pct:.1f}% slower")
        else:
            verdict = c("32", f"B {-pct:.1f}% faster")
        print(f"{t:<34} {unit:<9} {ma:>12.4g} {mb:>12.4g} {pct:>+8.1f}%  {verdict}")
    print()

print(c("1", "Summary (geometric mean of per-test slowdown, B vs A; + = B slower):"))
for g, rs in ratios.items():
    gm = math.exp(sum(math.log(r) for r in rs) / len(rs))
    print(f"  {g:<8} {(gm - 1) * 100:+6.1f}%   ({len(rs)} tests)")
ctl = ratios.get("control")
if ctl:
    gm = math.exp(sum(math.log(r) for r in ctl) / len(ctl))
    if abs(gm - 1) > 0.03:
        print(c("33", "  WARNING: control tests moved by more than 3%: the runs differ in more than the"
                       " change under test (turbo, temperature, background load?)"))
PY
}

list() {
  local f
  for f in "$OUTDIR"/*.tsv; do printf '%s  %s\n' "$(sed -n 's/^# date: //p' "$f")" "$f"; done | sort
  compgen -G "$OUTDIR/*.tsv" >/dev/null || echo "no results in $OUTDIR yet"
}

# ----------------------------------------------------------------------------- run
run() {
  command -v perf >/dev/null || die "perf not found"
  command -v stress-ng >/dev/null || die "stress-ng not found"
  command -v python3 >/dev/null || die "python3 not found"

  local cmdline label
  cmdline=$(</proc/cmdline)
  if [[ " $cmdline " == *" mitigations=off "* ]]; then label=mitigations-off; else label=mitigations-on; fi
  label=${1:-$label}

  # --- CPU choice: quiet isolated CPU for single-threaded, load-balanced P-cores for pairs
  local online iso c maxf best
  online=" $(expand_cpus "$(<"$SYS/online")") "
  iso=()
  for c in $(expand_cpus "$(cat "$SYS/isolated" 2>/dev/null)"); do [[ $online == *" $c "* ]] && iso+=("$c"); done
  best=0
  for c in $online; do maxf=$(cat "$SYS/cpu$c/cpufreq/cpuinfo_max_freq" 2>/dev/null || echo 0); ((maxf > best)) && best=$maxf; done
  pcores=()
  for c in $online; do
    [[ " ${iso[*]} " == *" $c "* ]] && continue
    maxf=$(cat "$SYS/cpu$c/cpufreq/cpuinfo_max_freq" 2>/dev/null || echo 0)
    ((maxf >= best * 9 / 10)) && pcores+=("$c")    # within 10% of the fastest: P-core
  done
  CPU1=${CPU:-${iso[0]:-${pcores[1]:-0}}}
  PAIR=${PAIR:-${pcores[0]:-0},${pcores[1]:-1}}

  mkdir -p "$OUTDIR"
  local stamp out
  stamp=$(date +%Y%m%d-%H%M%S)
  out=$OUTDIR/$label-$stamp.tsv

  # --- environment snapshot: what must match between A and B
  local pkg vuln
  pkg=$(for h in /sys/class/hwmon/hwmon*; do [[ $(<"$h/name") == coretemp ]] && cat "$h/temp1_input"; done 2>/dev/null | head -1)
  vuln=$(for v in "$SYS"/vulnerabilities/*; do printf '%s=%s; ' "${v##*/}" "$(<"$v")"; done)
  {
    echo "# label: $label"
    echo "# date: $(date -Is)"
    echo "# kernel: $(uname -r)"
    echo "# cmdline: $cmdline"
    echo "# cpu: $(sed -n 's/^model name\s*: //p' /proc/cpuinfo | head -1)"
    echo "# microcode: $(awk '/^microcode/ {print $3; exit}' /proc/cpuinfo)"
    echo "# governor: $(cat "$SYS"/cpu[0-9]*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | xargs)"
    echo "# epp: $(cat "$SYS"/cpu[0-9]*/cpufreq/energy_performance_preference 2>/dev/null | sort | uniq -c | xargs)"
    echo "# turbo: $([[ $(cat "$SYS/intel_pstate/no_turbo" 2>/dev/null) == 1 ]] && echo off || echo on)"
    echo "# smt: $(cat "$SYS/smt/control" 2>/dev/null)"
    echo "# power_limit_w: $(( $(cat /sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw 2>/dev/null || echo 0) / 1000000 ))"
    echo "# pinning: single=$CPU1 pair=$PAIR"
    echo "# reps: $REPS stress_s: $STRESS_S"
    echo "# vulnerabilities: $vuln"
  } >"$out"

  echo "perf-measure: label=$label  reps=$REPS  single-thread CPU $CPU1, pair $PAIR"
  echo "  -> $out"
  local load; load=$(cut -d' ' -f1 /proc/loadavg)
  awk -v l="$load" 'BEGIN {exit !(l > 1.0)}' && echo "  WARNING: load average $load: close other programs for clean numbers"
  [[ -n $pkg ]] && echo "  package temperature at start: $((pkg / 1000))°C"
  echo

  # --- helpers ------------------------------------------------------------------
  # measure NAME GROUP UNIT lower|higher CMD...   CMD prints one number per run
  measure() {
    local name=$1 group=$2 unit=$3 better=$4; shift 4
    local i v samples=()
    printf '  %-34s' "$name"
    [[ $1 == sng ]] || "$@" >/dev/null 2>&1        # warm-up (stress-ng runs are long enough)
    for ((i = 0; i < REPS; i++)); do
      v=$("$@" 2>/dev/null | tail -1)
      [[ $v =~ ^[0-9.]+$ ]] || { printf ' (failed)\n'; return; }
      samples+=("$v")
      printf '.'
    done
    printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$group" "$unit" "$better" "$(IFS=,; echo "${samples[*]}")" >>"$out"
    printf ' %s %s (median)\n' "$(printf '%s\n' "${samples[@]}" | sort -g | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}')" "$unit"
  }
  on1() { taskset -c "$CPU1" "$@"; }
  on2() { taskset -c "$PAIR" "$@"; }
  sng() { # stress-ng stressor on one CPU -> bogo ops/s
    on1 stress-ng --"$1" 1 "${@:2}" --timeout "$STRESS_S" --metrics-brief 2>&1 |
      awk -v s="$1" '$0 ~ "metrc:.* " s " " {print $9}'
  }
  timed_ms() { local t0; t0=$(now_ns); "$@" >/dev/null 2>&1; awk -v a="$t0" -v b="$(now_ns)" 'BEGIN {printf "%.3f\n", (b - a) / 1e6}'; }

  # individual tests (each prints one number)
  t_syscall()   { on1 perf bench syscall basic 2>&1 | awk '/usecs\/op/ {printf "%.2f\n", $1 * 1000}'; }
  t_pipe()      { on1 perf bench sched pipe -l 200000 2>&1 | awk '/usecs\/op/ {print $1}'; }
  t_uring()     { on2 "$ROOT/04-io_uring/uring_bench" 1000000 | awk -v m="$1" 'index($0, m) == 1 {print $(NF - 1)}'; }
  t_kvm()       { on1 "$ROOT/02-hypercalls/kvm_exits" 2>&1 | awk -v m="$1" 'index($0, m) {for (i = 1; i < NF; i++) if ($(i + 1) == "ns/iter") print $i}'; }
  t_kick()      { # $1 = "classic"/"fast", $2 = 1 (pipelined) or 2 (ping-pong) -> us/req
    on2 "$ROOT/09-kick-roundtrip/kick_roundtrip" 2>&1 |
      awk -v w="$1" -v n="$2" '$1 == w":" {k++; if (k == n) {match($0, /\( *[0-9.]+ us\/req/); s = substr($0, RSTART + 1); print s + 0}}'
  }
  t_forkexec()  { timed_ms on1 bash -c 'for ((i = 0; i < 2000; i++)); do /bin/true; done'; }
  t_tar()       { timed_ms on1 bash -c 'tar cf - -C /usr/share man | cat >/dev/null'; }  # ~32k files: open/stat/read
  t_gcc()       { timed_ms on1 bash -c "for i in 1 2 3 4 5 6 7 8 9 10; do gcc -O2 -c -o /dev/null '$ROOT/04-io_uring/uring_bench.c'; done"; }
  t_cpu()       { sng cpu --cpu-method matrixprod; }
  t_memcpy()    { on1 perf bench mem memcpy -s 256MB -l 5 2>&1 | awk '/GB\/sec/ {print $1}'; }

  echo "[kernel] syscalls, context switches, faults, process creation"
  measure syscall_getppid            kernel ns   lower  t_syscall
  measure null_write_ops             kernel ops/s higher sng null
  measure context_switch_pipe        kernel us   lower  t_pipe
  measure switch_ops                 kernel ops/s higher sng switch
  measure page_fault_ops             kernel ops/s higher sng fault
  measure fork_ops                   kernel ops/s higher sng fork
  measure exec_ops                   kernel ops/s higher sng exec
  measure pipe_ops                   kernel ops/s higher sng pipe
  measure futex_ops                  kernel ops/s higher sng futex
  measure signal_queue_ops           kernel ops/s higher sng sigq
  measure mmap_ops                   kernel ops/s higher sng mmap --mmap-bytes 16M
  if [[ -x $ROOT/04-io_uring/uring_bench ]]; then
    measure read_loop_ops              kernel ops/s higher t_uring "read() loop"
    measure io_uring_batch_ops         kernel ops/s higher t_uring "io_uring batch"
    measure io_uring_sqpoll_ops        kernel ops/s higher t_uring "io_uring SQPOLL"
  else
    echo "  (skipping io_uring: build with make -C 04-io_uring)"
  fi

  echo "[kvm] VM exits and device round trips"
  if [[ -r /dev/kvm && -w /dev/kvm && -x $ROOT/02-hypercalls/kvm_exits ]]; then
    measure kvm_exit_heavy_io          kvm    ns   lower  t_kvm "bench OUT"
    measure kvm_exit_light_cpuid       kvm    ns   lower  t_kvm "bench CPUID"
    measure kvm_exit_light_vmcall      kvm    ns   lower  t_kvm "bench VMCALL"
  else
    echo "  (skipping kvm_exits: needs /dev/kvm access and make -C 02-hypercalls)"
  fi
  if [[ -r /dev/kvm && -w /dev/kvm && -x $ROOT/09-kick-roundtrip/kick_roundtrip ]]; then
    measure kick_pipelined_classic     kvm    us   lower  t_kick classic 1
    measure kick_pipelined_fast        kvm    us   lower  t_kick fast 1
    measure kick_pingpong_classic      kvm    us   lower  t_kick classic 2
    measure kick_pingpong_fast         kvm    us   lower  t_kick fast 2
  else
    echo "  (skipping kick_roundtrip: needs /dev/kvm access and make -C 09-kick-roundtrip)"
  fi

  echo "[real] everyday workloads"
  measure shell_forkexec_2000        real   ms   lower  t_forkexec
  measure tar_usr_share_man          real   ms   lower  t_tar
  measure gcc_compile_x10            real   ms   lower  t_gcc

  echo "[control] should not change between A and B"
  measure cpu_matrixprod_ops         control ops/s higher t_cpu
  measure memcpy_256MB               control GB/s higher t_memcpy

  [[ -n $pkg ]] && pkg=$(for h in /sys/class/hwmon/hwmon*; do [[ $(<"$h/name") == coretemp ]] && cat "$h/temp1_input"; done 2>/dev/null | head -1) &&
    echo && echo "package temperature at end: $((pkg / 1000))°C"
  echo
  echo "saved: $out"
  local other
  other=''
  for f in "$OUTDIR"/*.tsv; do   # newest result from a different label
    [[ ${f##*/} == "$label-"* ]] && continue
    [[ -z $other || $f -nt $other ]] && other=$f
  done
  if [[ -n $other ]]; then
    echo "compare with the latest other run:"
    echo "  $0 compare $other $out"
  else
    echo "next: reboot into the other configuration, run '$0 run' again, then '$0 compare A B'"
  fi
}

case ${1:-} in
  run) shift; run "$@" ;;
  compare) shift; compare "$@" ;;
  list) list ;;
  *) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
