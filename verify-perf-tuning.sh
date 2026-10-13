#!/usr/bin/env bash
# verify-perf-tuning.sh: read-only check that the tuning described in
# optimizing-linux-for-ultra-performance.md is actually in effect.
#
# Expected values are read from your own config files (/etc/default/grub,
# /etc/sysctl.d, /etc/tmpfiles.d, /etc/security/limits.d) and compared with the
# running system, so the script follows whatever you configured.
#
# Needs no root; checks that need root are skipped and say so.
# Exit status: 0 = no FAIL (warnings allowed), 1 = at least one FAIL.
#
# Usage: ./verify-perf-tuning.sh [-q]     (-q: only print WARN and FAIL)
#
# Section 10 samples the CPU throttle counters for THROTTLE_SAMPLE_S seconds
# (default 5) to see whether the CPU is throttling right now. Set it to 0 to
# skip the wait; run it once idle and once under load to compare.

set -u
shopt -s nullglob

QUIET=0
[[ ${1:-} == -q ]] && QUIET=1

if [[ -t 1 ]]; then
  G=$'\e[32m' Y=$'\e[33m' R=$'\e[31m' B=$'\e[1m' D=$'\e[2m' N=$'\e[0m'
else
  G='' Y='' R='' B='' D='' N=''
fi
NPASS=0 NWARN=0 NFAIL=0
SHOWN=1   # was the last PASS/WARN/FAIL/info line printed? hints follow it
pass()    { NPASS=$((NPASS + 1)); SHOWN=$((!QUIET)); ((QUIET)) || printf '  %sPASS%s  %s\n' "$G" "$N" "$*"; }
warn()    { NWARN=$((NWARN + 1)); SHOWN=1; printf '  %sWARN%s  %s\n' "$Y" "$N" "$*"; }
fail()    { NFAIL=$((NFAIL + 1)); SHOWN=1; printf '  %sFAIL%s  %s\n' "$R" "$N" "$*"; }
info()    { SHOWN=$((!QUIET)); ((QUIET)) || printf '  %sinfo%s  %s\n' "$D" "$N" "$*"; }
hint()    { ((SHOWN)) && printf '        %s-> %s%s\n' "$D" "$*" "$N"; }
section() { ((QUIET)) || printf '\n%s%s%s\n' "$B" "$*" "$N"; }

CPU=/sys/devices/system/cpu
CMDLINE=$(</proc/cmdline)

# --- helpers -----------------------------------------------------------------

# Is exact token $1 on the running kernel command line?
has_param() { [[ " $CMDLINE " == *" $1 "* ]]; }
# Value of the last key=value for key $1 on the running command line.
param_val() { tr ' ' '\n' <<<"$CMDLINE" | sed -n "s/^$1=//p" | tail -1; }
# "0-3,8,10-11" -> "0 1 2 3 8 10 11"
expand_cpus() {
  local part out=() a b i
  IFS=, read -ra parts <<<"$1"
  for part in "${parts[@]}"; do
    [[ $part =~ ^[0-9]+$ ]] && { out+=("$part"); continue; }
    if [[ $part =~ ^([0-9]+)-([0-9]+)$ ]]; then
      a=${BASH_REMATCH[1]} b=${BASH_REMATCH[2]}
      for ((i = a; i <= b; i++)); do out+=("$i"); done
    fi
  done
  echo "${out[*]:-}"
}
# isolcpus=domain,managed_irq,4-7 -> "4-7" (drop the flag words)
cpulist_of() { tr ',' '\n' <<<"$1" | grep -E '^[0-9]' | paste -sd, -; }
# sysfs "[always] madvise never" -> "always"; plain values unchanged.
sysfs_value() {
  local v
  v=$(<"$1") || return 1
  [[ $v =~ \[([^]]+)\] ]] && v=${BASH_REMATCH[1]}
  echo "$v"
}
trim() { local s=$*; s=${s#"${s%%[![:space:]]*}"}; echo "${s%"${s##*[![:space:]]}"}"; }
# Collapse runs of whitespace (sysctl prints some multi-value keys with tabs).
norm() { trim "$(tr -s '[:space:]' ' ' <<<"$*")"; }
is_online() { [[ " $(online_cpus) " == *" $1 "* ]]; }
online_cpus() { expand_cpus "$(<"$CPU/online")"; }
svc_active() { systemctl is-active --quiet "$1" 2>/dev/null; }

printf '%sPerformance tuning check%s  (%s, kernel %s)\n' "$B" "$N" \
  "$(sed -n 's/^model name\s*: //p' /proc/cpuinfo | head -1)" "$(uname -r)"

# --- 1. microcode --------------------------------------------------------------
section "1. Microcode"
rev=$(awk '/^microcode/ {print $3; exit}' /proc/cpuinfo)
info "running microcode revision: ${rev:-unknown}"
if [[ -r $CPU/vulnerabilities/old_microcode ]]; then
  om=$(<"$CPU/vulnerabilities/old_microcode")
  if [[ $om == "Not affected" ]]; then pass "old_microcode: $om"
  else fail "old_microcode: $om"; hint "install intel-ucode/amd-ucode, remove dis_ucode_ldr, update-grub, reboot"; fi
fi
if grep -q GenuineIntel /proc/cpuinfo && command -v pacman >/dev/null; then
  if pacman -Q intel-ucode >/dev/null 2>&1; then pass "intel-ucode installed ($(pacman -Q intel-ucode | cut -d' ' -f2))"
  else fail "intel-ucode package not installed"; fi
fi
if has_param dis_ucode_ldr; then fail "dis_ucode_ldr on the running command line (microcode loading disabled)"
else pass "microcode loader enabled (no dis_ucode_ldr)"; fi

# --- 2. kernel command line vs /etc/default/grub -------------------------------------
section "2. Kernel command line (/etc/default/grub vs running kernel)"
grub_line=$(sed -n 's/^GRUB_CMDLINE_LINUX_DEFAULT=//p' /etc/default/grub 2>/dev/null | tail -1)
grub_line=${grub_line#[\"\']}; grub_line=${grub_line%[\"\']}
missing=()
for tok in $grub_line; do has_param "$tok" || missing+=("$tok"); done
if ((${#missing[@]})); then
  fail "in /etc/default/grub but NOT in the running kernel: ${missing[*]}"
  hint "run 'sudo update-grub' and reboot"
else
  pass "all $(wc -w <<<"$grub_line") GRUB_CMDLINE_LINUX_DEFAULT parameters are active"
fi
# Tuning parameters running now but not in the config: a one-off edit at the GRUB menu?
for key in isolcpus nohz_full rcu_nocbs irqaffinity intel_idle.max_cstate processor.max_cstate \
           mitigations nosmt transparent_hugepage idle; do
  for tok in $CMDLINE; do
    [[ $tok == "$key" || $tok == "$key="* ]] || continue
    [[ " $grub_line " == *" $tok "* ]] || warn "'$tok' is active but not in /etc/default/grub (lost at next boot)"
  done
done
if [[ -r /boot/grub/grub.cfg ]]; then
  stale=()
  for tok in $grub_line; do grep -qF -- "$tok" /boot/grub/grub.cfg || stale+=("$tok"); done
  if ((${#stale[@]})); then fail "grub.cfg is stale, missing: ${stale[*]}"; hint "sudo update-grub"
  else pass "/boot/grub/grub.cfg contains all parameters"; fi
else
  info "/boot/grub/grub.cfg not readable without root; skipped"
fi

# --- 3. CPU isolation ------------------------------------------------------------------
section "3. CPU isolation"
iso_param=$(param_val isolcpus)
if [[ -z $iso_param ]]; then
  info "no isolcpus= on the command line; isolation checks skipped"
  ISO=()
else
  want=$(expand_cpus "$(cpulist_of "$iso_param")")
  have=$(expand_cpus "$(<"$CPU/isolated")")
  if [[ $want == "$have" ]]; then pass "isolated CPUs: $have"
  else fail "isolcpus asks for [$want] but kernel isolated [$have]"; fi
  read -ra ISO <<<"$have"

  nz=$(param_val nohz_full)
  if [[ -n $nz ]]; then
    if [[ $(expand_cpus "$nz") == "$(expand_cpus "$(<"$CPU/nohz_full")")" ]]; then pass "nohz_full CPUs: $(<"$CPU/nohz_full")"
    else fail "nohz_full=$nz but kernel reports $(<"$CPU/nohz_full")"; fi
  else
    warn "isolcpus without nohz_full: the timer tick still interrupts isolated CPUs"
  fi
  if [[ -n $(param_val rcu_nocbs) ]]; then pass "rcu_nocbs=$(param_val rcu_nocbs)"
  else warn "no rcu_nocbs=: RCU callbacks may run on isolated CPUs"; fi

  online=" $(online_cpus) "
  offline=()
  for c in "${ISO[@]}"; do [[ $online == *" $c "* ]] || offline+=("$c"); done
  if ((${#offline[@]})); then
    info "isolated but offline (SMT off?): ${offline[*]}; usable isolated CPUs:$(for c in "${ISO[@]}"; do [[ $online == *" $c "* ]] && printf ' %s' "$c"; done)"
  fi

  # Hardware interrupts landing on isolated CPUs.
  irq_hits=()
  for d in /proc/irq/[0-9]*; do
    eff=$(cat "$d/effective_affinity_list" 2>/dev/null) || continue
    [[ -z $eff ]] && continue
    for c in $(expand_cpus "$eff"); do
      if [[ " ${ISO[*]} " == *" $c "* ]]; then
        n=${d##*/}
        irq_hits+=("$n($(awk -v k="$n:" '$1 == k {print $NF}' /proc/interrupts))->cpu$c")
        break
      fi
    done
  done
  if ((${#irq_hits[@]})); then
    warn "${#irq_hits[@]} IRQs still target isolated CPUs: ${irq_hits[*]}"
    hint "usually per-queue 'managed' IRQs (NVMe, multi-queue NICs/Wi-Fi) the kernel spreads one per CPU;"
    hint "irqaffinity= cannot move them. NVMe queue IRQs fire only for I/O issued from that CPU."
  else
    pass "no hardware IRQs target isolated CPUs"
  fi
  if svc_active irqbalance; then warn "irqbalance is running and may move IRQs onto isolated CPUs"; hint "disable it or set IRQBALANCE_BANNED_CPULIST"
  else pass "irqbalance not running"; fi

  # User-space threads currently on isolated CPUs (kernel threads have ppid 2).
  mapfile -t tasks < <(ps -eLo pid=,ppid=,psr=,comm= | awk -v iso=" ${ISO[*]} " \
    '$2 != 2 && $1 != 2 && index(iso, " " $3 " ") {print $4 "(pid " $1 ", cpu " $3 ")"}' | sort -u)
  if ((${#tasks[@]})); then
    warn "user-space threads on isolated CPUs right now: ${tasks[*]}"
    hint "fine if you pinned them there on purpose; otherwise something ignores the isolation"
  else
    pass "no user-space threads on isolated CPUs"
  fi
fi

# --- 4. idle states (C-states) ---------------------------------------------------------
section "4. Idle states"
first=$(online_cpus | cut -d' ' -f1)
drv=$(cat "$CPU/cpuidle/current_driver" 2>/dev/null || echo none)
states=()
for s in "$CPU/cpu$first"/cpuidle/state*; do
  d=$(<"$s/disable"); states+=("$(<"$s/name")$([[ $d == 1 ]] && echo '(disabled)')")
done
info "cpuidle driver: $drv; states on cpu$first: ${states[*]:-none}"
mc=$(param_val intel_idle.max_cstate)
[[ -n $mc ]] && info "intel_idle.max_cstate=$mc (module reports $(cat /sys/module/intel_idle/parameters/max_cstate 2>/dev/null || echo '?'))"
usable=0
for s in "$CPU/cpu$first"/cpuidle/state*; do
  [[ $(<"$s/name") != POLL && $(<"$s/disable") == 0 ]] && usable=$((usable + 1))
done
if ((usable == 0)); then
  warn "only POLL is available: idle cores busy-spin (lowest wake-up latency, maximum power and heat)"
  hint "intended for benchmarking? if you wanted to keep C1, raise intel_idle.max_cstate by 1 and re-run this script"
else
  pass "$usable sleep state(s) besides POLL enabled"
fi

# --- 5. frequency and turbo --------------------------------------------------------------
section "5. Frequency, turbo, SMT"
info "scaling driver: $(cat "$CPU/cpu$first/cpufreq/scaling_driver" 2>/dev/null || echo none)"
govs=$(cat "$CPU"/cpu[0-9]*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | awk '{printf "%s%s x%s", s, $2, $1; s=", "}')
epps=$(cat "$CPU"/cpu[0-9]*/cpufreq/energy_performance_preference 2>/dev/null | sort | uniq -c | awk '{printf "%s%s x%s", s, $2, $1; s=", "}')
info "governor: ${govs:-n/a};  EPP: ${epps:-n/a}"
[[ -r $CPU/intel_pstate/no_turbo ]] && info "turbo: $([[ $(<"$CPU/intel_pstate/no_turbo") == 1 ]] && echo OFF || echo ON)"
[[ -r $CPU/smt/control ]] && info "SMT: $(<"$CPU/smt/control")"
owners=()
svc_active power-profiles-daemon && owners+=("power-profiles-daemon($(powerprofilesctl get 2>/dev/null))")
svc_active tuned && owners+=("tuned($(tuned-adm active 2>/dev/null | sed -n 's/^Current active profile: //p'))")
tmpgov=$(grep -hE '^\s*w\S*\s+\S*scaling_governor' /etc/tmpfiles.d/*.conf 2>/dev/null | head -1)
[[ -n $tmpgov ]] && owners+=("tmpfiles.d")
if ((${#owners[@]} > 1)); then
  warn "several things manage CPU frequency: ${owners[*]}; the last one to run wins"
  hint "keep one owner per knob (section 4 of the guide)"
elif ((${#owners[@]} == 1)); then
  pass "single frequency owner: ${owners[0]}"
fi

# --- 6. /etc/tmpfiles.d writes to /sys -----------------------------------------------------
section "6. /etc/tmpfiles.d (sysfs values)"
found=0
for f in /etc/tmpfiles.d/*.conf; do
  while read -r type path _ _ _ _ arg; do
    [[ $type == w* && $path == /sys/* ]] || continue
    found=1
    arg=$(trim "$arg")
    ok=0 bad=()
    # shellcheck disable=SC2206  # glob expansion of the tmpfiles path is intended
    paths=($path)
    for p in "${paths[@]}"; do
      [[ -e $p ]] || continue
      # Offline CPUs (e.g. SMT off) keep their sysfs files but have no live value.
      [[ $p =~ /cpu([0-9]+)/ ]] && ! is_online "${BASH_REMATCH[1]}" && continue
      v=$(sysfs_value "$p" 2>/dev/null) || { bad+=("$p=<unreadable>"); continue; }
      if [[ $v == "$arg" ]]; then ok=$((ok + 1)); else bad+=("$v"); fi
    done
    total=$((ok + ${#bad[@]}))
    short=${path#/sys/devices/system/}
    if ((total == 0)); then
      warn "$short: matches no existing file"
    elif ((${#bad[@]} == 0)); then
      pass "$short = $arg ($ok/$total)"
    else
      fail "$short: want '$arg', found: $(printf '%s\n' "${bad[@]}" | sort | uniq -c | awk '{printf "%s%s x%s", s, $2, $1; s=", "}') ($ok/$total ok)"
      [[ $path == *scaling_governor* ]] && svc_active power-profiles-daemon && \
        hint "power-profiles-daemon starts after tmpfiles and resets the governor; drop this line or disable the daemon"
      hint "apply now: sudo systemd-tmpfiles --create $f"
    fi
  done < <(grep -vE '^\s*(#|$)' "$f")
done
((found)) || info "no 'w /sys/...' lines in /etc/tmpfiles.d"

# --- 7. /etc/sysctl.d ------------------------------------------------------------------------
section "7. /etc/sysctl.d (kernel tunables)"
declare -A want_val want_src
for f in /etc/sysctl.conf /etc/sysctl.d/*.conf; do   # later files override earlier ones
  [[ -r $f ]] || continue
  lineno=0 commented=()
  while IFS= read -r line; do
    lineno=$((lineno + 1))
    [[ $line =~ ^[[:space:]]*([#\;]|$) ]] && continue
    [[ $line == *=* ]] || continue
    key=$(trim "${line%%=*}"); val=${line#*=}
    key=${key#-}                                     # leading '-': ignore write errors
    if [[ $val =~ [[:space:]][#\;] ]]; then
      commented+=("$lineno")
      val=${val%%[[:space:]][#;]*}
    fi
    want_val[$key]=$(trim "$val"); want_src[$key]="${f##*/}:$lineno"
  done <"$f"
  if ((${#commented[@]})); then
    warn "${f##*/}: trailing comments on line(s) ${commented[*]} are sent to the kernel as part of the value"
    hint "sysctl.d only treats lines STARTING with # or ; as comments; numeric keys happen to"
    hint "tolerate the extra text, string keys do not. Move each comment to its own line."
  fi
done
for key in $(printf '%s\n' "${!want_val[@]}" | sort); do
  [[ $key == *'*'* ]] && { info "$key: glob keys not checked"; continue; }
  if ! live=$(sysctl -n "$key" 2>/dev/null); then
    warn "$key (${want_src[$key]}): no such sysctl on this system (interface/module absent?)"
    continue
  fi
  live=$(norm "$live")
  if [[ $live == "$(norm "${want_val[$key]}")" ]]; then pass "$key = $live"
  else fail "$key: configured '${want_val[$key]}' (${want_src[$key]}), live '$live'"; hint "sudo sysctl --system"; fi
done
((${#want_val[@]})) || info "no sysctl.d settings found"

# --- 8. memory --------------------------------------------------------------------------------
section "8. Memory"
info "transparent hugepages: $(sysfs_value /sys/kernel/mm/transparent_hugepage/enabled)"
req=$(sysctl -n vm.nr_hugepages 2>/dev/null || echo 0)
tot=$(awk '/^HugePages_Total/ {print $2}' /proc/meminfo)
if ((req > 0)); then
  if ((tot >= req)); then pass "huge pages reserved: $tot (free $(awk '/^HugePages_Free/ {print $2}' /proc/meminfo))"
  else warn "vm.nr_hugepages=$req but only $tot allocated (memory too fragmented)"; hint "reserve at boot, or use hugepages= on the kernel command line"; fi
fi
memlock=$(ulimit -l)
want_ml=''
for f in /etc/security/limits.conf /etc/security/limits.d/*.conf; do
  [[ -r $f ]] || continue
  while read -r dom _ item val _; do
    [[ $item == memlock ]] || continue
    if [[ $dom == "$USER" || $dom == '*' ]] || { [[ $dom == @* ]] && id -nG | tr ' ' '\n' | grep -qx "${dom#@}"; }; then
      want_ml=$val
    fi
  done < <(grep -vE '^\s*(#|$)' "$f")
done
if [[ -n $want_ml ]]; then
  if [[ $memlock == "$want_ml" || ($want_ml == unlimited && $memlock == unlimited) ]]; then pass "memlock limit for $USER: $memlock"
  else fail "memlock configured '$want_ml' for $USER, current shell has '$memlock'"; hint "log out and back in (limits apply at login)"; fi
else
  info "memlock limit for $USER: $memlock (no limits.d entry)"
fi

# --- 9. mitigations ---------------------------------------------------------------------------
section "9. Speculative-execution mitigations"
vuln=()
for v in "$CPU"/vulnerabilities/*; do [[ $(<"$v") == Vulnerable* ]] && vuln+=("${v##*/}"); done
if has_param mitigations=off; then
  info "mitigations=off: ${#vuln[@]} issue(s) reported Vulnerable, as expected: ${vuln[*]:-none}"
  warn "mitigations are OFF: only acceptable on a machine that runs no untrusted code (incl. web browsers)"
elif ((${#vuln[@]})); then
  fail "Vulnerable without mitigations=off: ${vuln[*]}"; hint "usually missing microcode (section 1)"
else
  pass "no CPU vulnerability reported as Vulnerable"
fi

# --- 10. thermals and throttling ------------------------------------------------------------
section "10. Thermals and throttling"
# coretemp exposes the package sensor as the tempN whose label is "Package id 0".
pkg_in='' pkg_max='' pkg_crit=''
for h in /sys/class/hwmon/hwmon*; do
  [[ $(<"$h/name") == coretemp ]] || continue
  for l in "$h"/temp*_label; do
    [[ $(<"$l") == "Package id 0" ]] || continue
    base=${l%_label}
    pkg_in=$base"_input" pkg_max=$base"_max" pkg_crit=$base"_crit"
  done
done
# CPU busy % between two /proc/stat snapshots. The load average is a 1-minute
# moving average and still reads "idle" seconds after a load starts.
stat_snap() { awk '/^cpu / {idle = $5 + $6; tot = 0; for (i = 2; i <= 9; i++) tot += $i; print tot, idle}' /proc/stat; }
busy_pct() { # $1 $2 = "tot idle" before, $3 $4 = after
  local dt=$(($3 - $1)) di=$(($4 - $2))
  ((dt > 0)) && echo $((100 * (dt - di) / dt)) || echo 0
}
read -r t0 i0 < <(stat_snap); sleep 1; read -r t1 i1 < <(stat_snap)
busy=$(busy_pct "$t0" "$i0" "$t1" "$i1")
idle=0; ((busy < 10)) && idle=1
if [[ -r $pkg_in ]]; then
  t=$(($(<"$pkg_in") / 1000)) hi=$(($(cat "$pkg_max" 2>/dev/null || echo 80000) / 1000)) cr=$(($(cat "$pkg_crit" 2>/dev/null || echo 100000) / 1000))
  info "package temperature ${t}°C (high ${hi}°C, critical ${cr}°C), CPU busy ${busy}%"
  if ((t >= cr)); then
    fail "package at/above its critical temperature (${t}°C)"
  elif ((t >= hi && idle)); then
    warn "package above its ${hi}°C high mark while nearly idle (${t}°C, CPU busy ${busy}%)"
    hint "idle cores are not resting: see the POLL/min_perf_pct check below, and the BIOS fan curve"
  elif ((t >= hi)); then
    info "above the ${hi}°C high mark under load; fine if it doesn't throttle (sample below)"
  else
    pass "package temperature ${t}°C is below its ${hi}°C high mark"
  fi
else
  info "coretemp package sensor not found; temperature checks skipped"
fi

# The config trap: no sleep states + never clocking down = every idle core at full clock.
mpp=$(cat "$CPU/intel_pstate/min_perf_pct" 2>/dev/null || echo 0)
if ((usable == 0 && mpp >= 100)); then
  warn "only POLL idle AND min_perf_pct=$mpp: idle cores spin at maximum clock"
  hint "this heats an idle CPU into throttling and lowers turbo for the cores doing work;"
  hint "allow a sleep state (intel_idle.max_cstate=2) and/or drop min_perf_pct; busy-polling threads stay awake anyway"
fi

# Throttle counters (per CPU for cores, per package for the package).
tt_sum() { # $1 = counter file name; sums over online CPUs
  local c sum=0 f
  for c in $(online_cpus); do
    f=$CPU/cpu$c/thermal_throttle/$1
    [[ -r $f ]] && sum=$((sum + $(<"$f")))
  done
  echo "$sum"
}
tt_pkg() { cat "$CPU/cpu$first/thermal_throttle/$1" 2>/dev/null || echo 0; }
if [[ -d $CPU/cpu$first/thermal_throttle ]]; then
  up=$(cut -d. -f1 /proc/uptime)
  info "since boot (${up}s): package throttled $(tt_pkg package_throttle_count)x / $(tt_pkg package_throttle_total_time_ms) ms; cores throttled $(tt_sum core_throttle_count)x / $(tt_sum core_throttle_total_time_ms) ms (summed)"
  hint "since-boot counts include the startup burst; the sample below shows what happens now"
  secs=${THROTTLE_SAMPLE_S:-5}
  if ((secs > 0)); then
    p0=$(tt_pkg package_throttle_total_time_ms) c0=$(tt_sum core_throttle_total_time_ms)
    read -r t0 i0 < <(stat_snap)
    sleep "$secs"
    read -r t1 i1 < <(stat_snap)
    busy=$(busy_pct "$t0" "$i0" "$t1" "$i1")
    idle=0; ((busy < 10)) && idle=1
    tmax=''; [[ -r $pkg_in ]] && tmax=", package now $(($(<"$pkg_in") / 1000))°C"
    dp=$(($(tt_pkg package_throttle_total_time_ms) - p0)) dc=$(($(tt_sum core_throttle_total_time_ms) - c0))
    if ((dp == 0 && dc == 0)); then
      pass "no throttling during a ${secs}s sample (CPU busy ${busy}%${tmax})"
    else
      warn "throttling NOW: package throttled ${dp} ms of ${secs}000 ms ($((dp / (secs * 10)))%), cores +${dc} ms summed (CPU busy ${busy}%${tmax})"
      hint "under full load: steeper BIOS fan curve or a lower power limit raises sustained clocks"
      ((idle)) && hint "while idle: idle cores are not resting (see above)"
    fi
  fi
else
  info "no thermal_throttle counters exposed; throttle checks skipped"
fi

# --- summary ----------------------------------------------------------------------------------
printf '\n%sSummary:%s %s%d pass%s, %s%d warn%s, %s%d fail%s\n' "$B" "$N" \
  "$G" "$NPASS" "$N" "$Y" "$NWARN" "$N" "$R" "$NFAIL" "$N"
((NFAIL == 0))
