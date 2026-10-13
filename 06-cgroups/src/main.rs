//! cglab — a hands-on tour of cgroup v2.
//!
//! ```text
//! cglab info                      what does this host/cgroup look like?
//! cglab limits                    cpu.max, memory.high, memory.max (OOM), pids.max
//! cglab weight                    cpu.weight: proportional sharing under contention
//! cglab nip                       the "no internal processes" rule
//! cglab gomaxprocs                what Go >= 1.25 (and Rust std) derive from cpu.max
//! cglab jail --cpu 50 --mem 64 --pids 32 -- <cmd...>     jailer-style confinement
//! cglab clean                     remove stale cglab-* groups
//! cglab all                       info + limits + weight + nip + gomaxprocs
//! ```
//! Everything runs unprivileged inside a systemd-delegated subtree
//! (`user@UID.service`), or anywhere you point `CGLAB_ROOT`.
//!
//! How the demos generate load: the binary re-runs *itself* with a hidden
//! `worker` subcommand (see [`worker`]), started inside a freshly created
//! cgroup via `cg::spawn_in`. The parent then reads the cgroup's statistics
//! files to show what the kernel did to the worker.

mod cg;

use cg::{spawn_in, wait_timeout, Group, Sandbox};
use std::env;
use std::fs;
use std::io;
use std::os::unix::process::ExitStatusExt; // ExitStatus::signal()
use std::process::ExitCode;
use std::thread::sleep;
use std::time::{Duration, Instant};

const MIB: u64 = 1024 * 1024;

/// argv that re-invokes THIS binary as a worker, optionally behind a wrapper
/// (e.g. `taskset -c 0`).
/// Result: `[wrapper..., /path/to/cglab, args...]`.
fn worker_argv(wrapper: &[&str], args: &[&str]) -> Vec<String> {
    let exe = env::current_exe().unwrap().to_string_lossy().into_owned();
    wrapper.iter().map(|s| s.to_string()).chain([exe]).chain(args.iter().map(|s| s.to_string())).collect()
}

/// Print a bold section header (`\x1b[1m` / `\x1b[0m` are terminal escape
/// codes for bold on / off).
fn header(s: &str) {
    println!("\n\x1b[1m== {s} ==\x1b[0m");
}

// ───────────────────────────────────────────────────────────── worker side ──
// Hidden subcommands that run *inside* a cgroup and generate load.

fn worker(args: &[String]) {
    // n(i, default): numeric argument i, or `default` if missing/unparsable.
    let n = |i: usize, d: u64| args.get(i).and_then(|s| s.parse().ok()).unwrap_or(d);
    match args.first().map(|s| s.as_str()) {
        // cpu <threads> <secs>: burn CPU on N threads.
        Some("cpu") => {
            let deadline = Instant::now() + Duration::from_secs(n(2, 2));
            let hs: Vec<_> = (0..n(1, 1))
                .map(|_| {
                    std::thread::spawn(move || {
                        let mut x = 1u64;
                        while Instant::now() < deadline {
                            // Pointless arithmetic (a linear congruential
                            // generator step). black_box stops the optimiser
                            // from deleting it, so the thread really computes.
                            for _ in 0..10_000 {
                                x = std::hint::black_box(x.wrapping_mul(6364136223846793005).wrapping_add(1));
                            }
                        }
                        x
                    })
                })
                .collect();
            for h in hs {
                let _ = h.join();
            }
        }
        // mem <MiB> <secs>: allocate and keep touching memory.
        Some("mem") => {
            let (target, secs) = (n(1, 64), n(2, 3));
            let start = Instant::now();
            let deadline = start + Duration::from_secs(secs);
            let mut chunks: Vec<Vec<u8>> = vec![];
            while (chunks.len() as u64) < target && Instant::now() < deadline {
                // A zeroed 1 MiB allocation is served by fresh mmap()ed pages
                // that are not backed (or charged to the cgroup) until touched.
                let mut c = vec![0u8; MIB as usize];
                for i in (0..c.len()).step_by(4096) {
                    c[i] = 1; // touch every page so it is really charged
                }
                chunks.push(c);
                if chunks.len() % 8 == 0 {
                    println!("    worker: {:>3} MiB resident   t={:>5} ms", chunks.len(), start.elapsed().as_millis());
                }
            }
            // Keep the memory alive (and occasionally touched) until the deadline.
            while Instant::now() < deadline {
                for c in chunks.iter_mut() {
                    std::hint::black_box(&mut c[0]);
                }
                sleep(Duration::from_millis(50));
            }
            println!("    worker: finished normally with {} MiB", chunks.len());
        }
        // pids <n> <secs>: try to start N threads.
        // In Linux a thread is a task just like a process, and pids.max
        // limits tasks, so thread creation fails (EAGAIN) at the limit.
        Some("pids") => {
            let (want, secs) = (n(1, 20), n(2, 1));
            let mut ok = vec![];
            let mut first_err = None;
            for _ in 0..want {
                match std::thread::Builder::new().spawn(move || sleep(Duration::from_secs(secs))) {
                    Ok(h) => ok.push(h),
                    Err(e) => {
                        first_err.get_or_insert(e);
                    }
                }
            }
            println!("    worker: started {} of {} threads (+ main thread){}", ok.len(), want,
                first_err.map(|e| format!("; first failure: {e}")).unwrap_or_default());
            for h in ok {
                let _ = h.join();
            }
        }
        _ => eprintln!("unknown worker"),
    }
}

// ───────────────────────────────────────────────────────────────── demos ──

/// `cglab info`: where we are in the hierarchy and what we are allowed to use.
fn info() -> io::Result<()> {
    header("host cgroup info");
    println!("cgroup v2 unified hierarchy : {}", cg::is_v2());
    if !cg::is_v2() {
        println!("This host is cgroup v1 (or hybrid): one tree per controller under /sys/fs/cgroup/<controller>.");
        return Ok(());
    }
    println!("this process's cgroup       : {}", cg::own_cgroup()?.display());
    let root = cg::delegated_root()?;
    println!("delegated root we can use   : {}", root.display());
    let rg = Group { path: root.clone() };
    // cgroup.controllers  = controllers this group MAY use (granted by its parent)
    // cgroup.subtree_control = controllers it has turned on for its children
    println!("controllers available there : {}", rg.get("cgroup.controllers").unwrap_or_default());
    println!("controllers enabled below   : {}", rg.get("cgroup.subtree_control").unwrap_or_default());
    println!("logical CPUs (std)          : {:?}  <- Rust std already honours cpu.max & affinity",
        std::thread::available_parallelism());
    // System-wide Pressure Stall Information. The first line ("some ...") is the
    // share of time at least one task was stalled waiting for that resource.
    for r in ["cpu", "memory", "io"] {
        if let Ok(s) = fs::read_to_string(format!("/proc/pressure/{r}")) {
            println!("system PSI {r:<6}: {}", s.lines().next().unwrap_or(""));
        }
    }
    println!("(systemd delegated only the controllers listed above; `io` needs root or Delegate=io — so no io.max demo here.)");
    Ok(())
}

/// Print a multi-line flat-keyed events file on one line.
fn print_events(g: &Group, file: &str) {
    println!("    {file}: {}", g.get(file).unwrap_or_default().replace('\n', "  "));
}

/// `cglab limits`: one leaf cgroup per limit type, each with a worker that
/// tries to exceed it.
fn limits(sb: &Sandbox) -> io::Result<()> {
    // ───────────────────────────────────────────────── cpu.max
    header("cpu.max — hard CPU bandwidth cap (CFS quota / period)");
    let g = sb.leaf("cpu-quota")?;
    // Format: "<quota_us> <period_us>". 20000 100000 => at most 20 ms of CPU
    // time per 100 ms window across ALL threads of the group = 0.2 CPUs.
    // (Replaces v1's cpu.cfs_quota_us + cpu.cfs_period_us.)
    // Once the group has used its quota, all its threads are "throttled"
    // (not scheduled) until the next period starts.
    g.set("cpu.max", "20000 100000")?;
    println!("cpu.max = 20000 100000  (0.2 CPU).  Worker: 4 busy threads for 2 s of wall time.");
    let t = Instant::now();
    spawn_in(&g, &worker_argv(&[], &["worker", "cpu", "4", "2"]))?.wait()?;
    let wall = t.elapsed().as_secs_f64();
    // cpu.stat usage_usec = total CPU time consumed by the group, microseconds.
    let used = g.stat("cpu.stat", "usage_usec") as f64 / 1e6;
    println!("    wall {wall:.2} s, CPU consumed {used:.2} s  => average {:.2} CPUs (asked for 4, capped at 0.2)", used / wall);
    // nr_periods: enforcement periods that elapsed while the group was runnable;
    // nr_throttled: how many of those ended with the group throttled;
    // throttled_usec: total time threads spent throttled (summed over threads).
    println!("    cpu.stat: nr_periods={} nr_throttled={} throttled_usec={}",
        g.stat("cpu.stat", "nr_periods"), g.stat("cpu.stat", "nr_throttled"), g.stat("cpu.stat", "throttled_usec"));
    println!("    cpu.pressure  : {}", g.get("cpu.pressure").unwrap_or_else(|e| e.to_string()).replace('\n', "\n                    "));
    println!("    (PSI: throttled/queued tasks show up as stall time — the 'noisy neighbour / starved tenant' signal.)");

    // ───────────────────────────────────────────────── memory.high
    header("memory.high — soft limit: THROTTLE + reclaim, never kill");
    let g = sb.leaf("mem-high")?;
    g.set("memory.high", &(32 * MIB).to_string())?;
    let _ = g.set("memory.swap.max", "0"); // no swap: anon memory can't be reclaimed
    println!("memory.high = 32 MiB. Worker wants 64 MiB. Watch it slow down once it crosses 32.");
    let mut c = spawn_in(&g, &worker_argv(&[], &["worker", "mem", "64", "4"]))?;
    // Sample memory.current WHILE the worker is alive (it reads 0 after the group empties).
    let mut cur_mib = 0;
    match wait_timeout(&mut c, Duration::from_secs(5))? {
        Some(st) => println!("    worker exited: {st}"),
        None => {
            cur_mib = g.get("memory.current").ok().and_then(|s| s.parse::<u64>().ok()).unwrap_or(0) / MIB;
            println!("    worker made no further progress for 5 s -> killing it (it was throttled, NOT OOM-killed)");
            let _ = c.kill();
            let _ = c.wait();
        }
    }
    println!("    memory.current while stalled: {cur_mib} MiB (held at the high mark)");
    print_events(&g, "memory.events");
    println!("    ^ `high` counts reclaim/throttle episodes. `oom_kill` stays 0: memory.high never kills.");
    println!("    Why it crawls: swap is off, and anonymous pages can't be reclaimed without swap, so the kernel");
    println!("    can only *delay* the allocating task (up to 2 s per episode). With swap, or a file-backed");
    println!("    working set, reclaim would make progress and the slowdown would be far gentler.");
    println!("    memory.pressure: {}", g.get("memory.pressure").unwrap_or_default().replace('\n', "\n                     "));

    // ───────────────────────────────────────────────── memory.max
    header("memory.max — hard limit: reclaim, then the OOM killer");
    let g = sb.leaf("mem-max")?;
    g.set("memory.max", &(32 * MIB).to_string())?;
    let _ = g.set("memory.swap.max", "0");
    println!("memory.max = 32 MiB. Worker wants 128 MiB.");
    let mut c = spawn_in(&g, &worker_argv(&[], &["worker", "mem", "128", "10"]))?;
    let st = c.wait()?;
    // signal() is Some(n) if the process was killed by signal n; 9 = SIGKILL.
    match st.signal() {
        Some(9) => println!("    worker died from SIGKILL (the cgroup OOM killer)"),
        other => println!("    worker exit: {st} (signal {other:?})"),
    }
    // memory.events: `max` = times usage hit memory.max, `oom` = times the
    // OOM path ran, `oom_kill` = processes killed by it.
    print_events(&g, "memory.events");
    // memory.peak (Linux 5.19+): highest usage ever recorded for this group.
    println!("    memory.peak: {} MiB", g.get("memory.peak").ok().and_then(|s| s.parse::<u64>().ok()).unwrap_or(0) / MIB);
    println!("    ^ the OOM kill is *contained*: it only considers tasks in this cgroup, so a tenant can't OOM its neighbours or the host.");
    println!("    (check `dmesg` for 'Memory cgroup out of memory' if you have permission)");

    // ───────────────────────────────────────────────── pids.max
    header("pids.max — fork-bomb protection");
    let g = sb.leaf("pids")?;
    g.set("pids.max", "8")?;
    println!("pids.max = 8 (threads count too). Worker tries to start 20 threads.");
    spawn_in(&g, &worker_argv(&[], &["worker", "pids", "20", "1"]))?.wait()?;
    print_events(&g, "pids.events");
    println!("    ^ `max` counts fork/clone attempts rejected by the limit.");
    Ok(())
}

/// `cglab weight`: proportional CPU sharing between two sibling groups.
fn weight(sb: &Sandbox) -> io::Result<()> {
    header("cpu.weight — proportional share, only matters under CONTENTION");
    // Weights are *relative*: they only bite when CPUs are oversubscribed. On a
    // 24-CPU box two single-threaded jobs never contend, so we force contention by
    // pinning both to CPU 0 with `taskset`.
    // (taskset sets the CPU affinity and then exec()s the worker, so the worker
    // is still the process that spawn_in placed in the cgroup.)
    let a = sb.leaf("w100")?;
    let b = sb.leaf("w400")?;
    a.set("cpu.weight", "100")?; // default weight is 100 (range 1..10000); replaces v1 cpu.shares
    b.set("cpu.weight", "400")?;
    println!("two groups, weights 100 vs 400, each one busy thread pinned to CPU 0 for 3 s");
    let ca = spawn_in(&a, &worker_argv(&["taskset", "-c", "0"], &["worker", "cpu", "1", "3"]));
    let cb = spawn_in(&b, &worker_argv(&["taskset", "-c", "0"], &["worker", "cpu", "1", "3"]));
    let (mut ca, mut cb) = match (ca, cb) {
        (Ok(x), Ok(y)) => (x, y),
        _ => {
            println!("  `taskset` not available; skipping.");
            return Ok(());
        }
    };
    ca.wait()?;
    cb.wait()?;
    // With 100:400 the scheduler gives the groups 1/5 and 4/5 of CPU 0.
    let (ua, ub) = (a.stat("cpu.stat", "usage_usec") as f64, b.stat("cpu.stat", "usage_usec") as f64);
    println!("    w100 used {:.2} s   w400 used {:.2} s   => ratio 1 : {:.1}  (expected ~1 : 4)", ua / 1e6, ub / 1e6, ub / ua);
    Ok(())
}

/// `cglab nip`: demonstrate both halves of the no-internal-process rule.
fn nip(sb: &Sandbox) -> io::Result<()> {
    header("the v2 'no internal processes' rule");
    println!("Rule: a cgroup may EITHER hold processes OR delegate controllers to children — not both.\n");

    // Format an error with its raw errno (EBUSY = 16 is expected here).
    let full = |e: &io::Error| format!("{} (errno {})", e, e.raw_os_error().unwrap_or(0));

    // Case 1: group already has a process; try to enable controllers for children.
    let g1 = sb.leaf("has-proc")?;
    let mut sleeper = spawn_in(&g1, &["sleep".into(), "5".into()])?;
    println!("1) `has-proc` contains a process (pid {}). Enable +memory for its children:", sleeper.id());
    match g1.set("cgroup.subtree_control", "+memory") {
        Ok(()) => println!("   unexpectedly succeeded"),
        Err(e) => println!("   write to cgroup.subtree_control -> {}   <== refused", full(&e)),
    }
    let _ = sleeper.kill();
    let _ = sleeper.wait();

    // Case 2: enable controllers first, then try to put a process in.
    let g2 = sb.leaf("delegating")?;
    g2.set("cgroup.subtree_control", "+memory")?;
    let leaf = Group::create(&g2.path, "leaf")?;
    println!("\n2) `delegating` has +memory enabled for children. Try to move a process INTO it:");
    // An ordinary child process (it starts in OUR cgroup); we then try to
    // migrate it by writing its PID into the target's cgroup.procs.
    let mut s2 = std::process::Command::new("sleep");
    s2.arg("5");
    let mut s2 = s2.spawn()?;
    match g2.set("cgroup.procs", &s2.id().to_string()) {
        Ok(()) => println!("   unexpectedly succeeded"),
        Err(e) => println!("   write pid to delegating/cgroup.procs -> {}   <== refused", full(&e)),
    }
    match leaf.set("cgroup.procs", &s2.id().to_string()) {
        Ok(()) => println!("   write pid to delegating/leaf/cgroup.procs   -> ok   (leaves hold processes)"),
        Err(e) => println!("   leaf move failed: {}", full(&e)),
    }
    let _ = s2.kill();
    let _ = s2.wait();

    println!("\nWhy it exists: with controllers enabled, a process sitting in an interior group would\n\
              compete for CPU/memory *against its own child groups* — v1's inconsistent semantics.\n\
              The rule keeps accounting well-defined: resources are only ever consumed at leaves.\n\
              (Exceptions: the root cgroup, and threaded subtrees for threaded controllers such as cpu and pids.)");
    Ok(())
}

/// Effective CPU limit implied by `cpu.max` of this cgroup AND all ancestors
/// (the tightest wins), in CPUs (quota / period). `None` = unlimited.
/// This is the input Go >= 1.25 starts from when choosing its default
/// GOMAXPROCS (Go then applies its own rounding and minimum).
fn effective_cpu_limit(start: &std::path::Path) -> Option<f64> {
    let mut best: Option<f64> = None;
    let mut p = start.to_path_buf();
    loop {
        if let Ok(s) = fs::read_to_string(p.join("cpu.max")) {
            let mut it = s.split_whitespace();
            if let (Some(q), Some(per)) = (it.next(), it.next()) {
                if q != "max" {
                    if let (Ok(q), Ok(per)) = (q.parse::<f64>(), per.parse::<f64>()) {
                        let lim = q / per;
                        best = Some(best.map_or(lim, |b: f64| b.min(lim)));
                    }
                }
            }
        }
        if p == std::path::Path::new(cg::CGROOT) || !p.pop() {
            break;
        }
    }
    best
}

/// `cglab gomaxprocs`: show how a CPU quota translates into "how many
/// threads should my runtime run in parallel".
fn gomaxprocs(sb: &Sandbox) -> io::Result<()> {
    header("cpu.max -> GOMAXPROCS (the container-CPU footgun)");
    println!("Before Go 1.25, GOMAXPROCS defaulted to the number of *host* CPUs even inside a\n\
              container limited to, say, 2 CPUs: the runtime ran 24 Ps, the cgroup throttled\n\
              them, and you got latency spikes. Go 1.25 defaults it to roughly min(NumCPU, ceil(cpu.max quota/period)).\n");
    let ncpu = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1);
    for (name, max) in [("half-cpu", "50000 100000"), ("two-and-a-half", "250000 100000"), ("unlimited", "max 100000")] {
        let g = sb.leaf(name)?;
        g.set("cpu.max", max)?;
        let lim = effective_cpu_limit(&g.path);
        // Our simple rule: ceil(limit), clamped to [1, host CPUs]. Go's actual
        // rule may additionally enforce a minimum (see the note printed below).
        let derived = lim.map_or(ncpu, |l| (l.ceil() as usize).clamp(1, ncpu));
        println!("  cpu.max = {max:<14} effective limit {:>5}   -> derived parallelism {derived:>2}   (host has {ncpu})",
            lim.map_or("none".to_string(), |l| format!("{l:.2}")));
    }
    println!("\nRust's std::thread::available_parallelism() also takes the cgroup CPU quota into account. Also");
    println!("note: cpu.max is a *bandwidth* cap, not a CPU count — 4 threads × 0.5 CPU each still");
    println!("get throttled after 50% of a core per period, so latency-sensitive services often");
    println!("prefer requests (cpu.weight) without a hard limit. (I recall Go applying a floor of");
    println!("2 when deriving this; check the Go 1.25 release notes for the exact rule.)");
    Ok(())
}

/// `cglab jail --cpu PCT --mem MIB --pids N -- cmd args...`
/// Run any command confined, the way Firecracker's jailer confines a VMM.
/// Options come in `--flag value` pairs before the `--` separator.
fn jail(args: &[String]) -> io::Result<()> {
    let sep = args.iter().position(|a| a == "--").ok_or_else(|| io::Error::other("usage: cglab jail [--cpu PCT] [--mem MIB] [--pids N] -- cmd args..."))?;
    let (opts, cmd) = (&args[..sep], &args[sep + 1..]);
    if cmd.is_empty() {
        return Err(io::Error::other("no command given"));
    }
    let sb = Sandbox::new()?;
    let g = sb.leaf("jail")?;
    let mut i = 0;
    while i + 1 < opts.len() {
        let v = &opts[i + 1];
        match opts[i].as_str() {
            // PCT% of one CPU = PCT*1000 us of CPU time per 100000 us period.
            "--cpu" => g.set("cpu.max", &format!("{} 100000", v.parse::<u64>().unwrap_or(100) * 1000))?, // PCT of one CPU
            "--mem" => {
                g.set("memory.max", &(v.parse::<u64>().unwrap_or(64) * MIB).to_string())?;
                let _ = g.set("memory.swap.max", "0"); // otherwise it could just swap
            }
            "--pids" => g.set("pids.max", v)?,
            o => return Err(io::Error::other(format!("unknown option {o}"))),
        }
        i += 2;
    }
    println!("jail: {} (limits: cpu.max={:?} memory.max={:?} pids.max={:?})", g.path.display(),
        g.get("cpu.max").unwrap_or_default(), g.get("memory.max").unwrap_or_default(), g.get("pids.max").unwrap_or_default());
    let st = spawn_in(&g, cmd)?.wait()?;
    println!("exit: {st}");
    println!("usage_usec={} nr_throttled={} memory.peak={} MiB",
        g.stat("cpu.stat", "usage_usec"), g.stat("cpu.stat", "nr_throttled"),
        g.get("memory.peak").ok().and_then(|s| s.parse::<u64>().ok()).unwrap_or(0) / MIB);
    print_events(&g, "memory.events");
    print_events(&g, "pids.events");
    Ok(())
    // `sb` is dropped here, which kills anything left and removes the cgroups.
}

/// `cglab clean`: remove sandboxes left behind by an interrupted run (Ctrl-C
/// skips the Drop-based cleanup).
fn clean() -> io::Result<()> {
    let root = cg::delegated_root()?;
    for e in fs::read_dir(&root)?.flatten() {
        if e.file_name().to_string_lossy().starts_with("cglab-") {
            println!("removing {}", e.path().display());
            Group { path: e.path() }.destroy();
        }
    }
    Ok(())
}

/// Dispatch on the first argument (default: `all`).
fn run() -> io::Result<()> {
    let args: Vec<String> = env::args().skip(1).collect();
    let cmd = args.first().map(|s| s.as_str()).unwrap_or("all");
    if cmd == "worker" {
        // We are a child process started by one of the demos, already inside
        // its cgroup: just generate the requested load.
        worker(&args[1..]);
        return Ok(());
    }
    if cmd == "info" || cmd == "all" {
        info()?;
        if cmd == "info" {
            return Ok(());
        }
    }
    if cmd == "clean" {
        return clean();
    }
    if cmd == "jail" {
        return jail(&args[1..]);
    }
    if !cg::is_v2() {
        return Err(io::Error::other("cgroup v2 required"));
    }
    let sb = Sandbox::new()?; // removed automatically on drop
    println!("sandbox: {}", sb.group.path.display());
    match cmd {
        "limits" => limits(&sb)?,
        "weight" => weight(&sb)?,
        "nip" => nip(&sb)?,
        "gomaxprocs" => gomaxprocs(&sb)?,
        "all" => {
            limits(&sb)?;
            weight(&sb)?;
            nip(&sb)?;
            gomaxprocs(&sb)?;
        }
        _ => return Err(io::Error::other(format!("unknown command `{cmd}`; see the header of src/main.rs"))),
    }
    Ok(())
}

/// Entry point: print any error and exit non-zero.
fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("cglab: {e}");
            ExitCode::FAILURE
        }
    }
}
