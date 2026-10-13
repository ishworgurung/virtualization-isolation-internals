//! Thin helpers over the cgroup v2 filesystem.
//!
//! A **cgroup** ("control group") is a set of processes that the kernel
//! accounts for and limits *as a group*: total CPU time, memory, number of
//! tasks, I/O bandwidth. Containers, systemd services and Firecracker microVMs
//! are all confined this way. cgroups do not isolate what a process can *see*
//! (that is namespaces' job); they limit what it can *consume*.
//!
//! The entire cgroup API is a file tree (normally `/sys/fs/cgroup`):
//!   * `mkdir`  a directory        -> create a cgroup
//!   * `rmdir`  it                 -> destroy it (must be empty of processes)
//!   * write a PID to `cgroup.procs` -> move that process in
//!   * write to `memory.max`, `cpu.max`, ... -> set limits
//!   * read `memory.current`, `cpu.stat`, `*.pressure` ... -> observe
//! No syscalls beyond open/read/write/mkdir/rmdir — which is why this crate has
//! no dependencies.
//!
//! Children (threads and forked processes) start in their parent's cgroup, so
//! placing one process in a group confines everything it later creates.

use std::collections::HashMap;
use std::env;
use std::fs;
use std::io;
use std::os::unix::process::CommandExt; // adds `pre_exec` to Command
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus};
use std::thread::sleep;
use std::time::{Duration, Instant};

/// Where the cgroup v2 filesystem ("cgroup2") is conventionally mounted.
pub const CGROOT: &str = "/sys/fs/cgroup";

/// Is this a cgroup **v2** (unified hierarchy) host?
/// v2 has a single tree with a `cgroup.controllers` file at its root; v1 mounts
/// one tree per controller (`/sys/fs/cgroup/cpu`, `/sys/fs/cgroup/memory`, ...).
/// (A "hybrid" host mounts v1 trees there and v2 at `/sys/fs/cgroup/unified`;
/// this function reports `false` for it.)
pub fn is_v2() -> bool {
    Path::new(CGROOT).join("cgroup.controllers").exists()
}

/// Absolute path of the cgroup this process currently lives in.
/// `/proc/self/cgroup` on v2 has a single line `0::/some/path`
/// (hierarchy id 0, empty controller list, path relative to the cgroup root).
pub fn own_cgroup() -> io::Result<PathBuf> {
    let s = fs::read_to_string("/proc/self/cgroup")?;
    for line in s.lines() {
        if let Some(rest) = line.strip_prefix("0::") {
            return Ok(Path::new(CGROOT).join(rest.trim_start_matches('/')));
        }
    }
    Err(io::Error::new(io::ErrorKind::NotFound, "no `0::` line in /proc/self/cgroup: this is not a pure cgroup v2 host"))
}

/// Are we running with effective UID 0?  The `Uid:` line of
/// /proc/self/status lists real, effective, saved and filesystem UIDs;
/// `nth(2)` after the "Uid:" label is the effective one.
fn is_root() -> bool {
    fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("Uid:")).map(|l| l.split_whitespace().nth(2) == Some("0")))
        .unwrap_or(false)
}

/// Find a cgroup subtree we are allowed to create children in.
///
/// systemd **delegates** `user@<uid>.service` to the user, so an unprivileged
/// user owns that directory and may mkdir, set limits and move their own
/// processes inside it. Override with `CGLAB_ROOT=<dir>`. Root can use `/sys/fs/cgroup`.
/// (By default systemd enables only the `cpu`, `memory` and `pids` controllers
/// for such a delegated subtree — which is why this lab has no `io.max` demo.)
///
/// Strategy: start from our own cgroup and walk up towards the root until a
/// directory named `user@….service` is found.
pub fn delegated_root() -> io::Result<PathBuf> {
    if let Ok(p) = env::var("CGLAB_ROOT") {
        return Ok(p.into());
    }
    let mut p = own_cgroup()?;
    loop {
        let name = p.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if name.starts_with("user@") && name.ends_with(".service") {
            return Ok(p);
        }
        if !p.pop() || p == Path::new(CGROOT) {
            break;
        }
    }
    if is_root() {
        return Ok(CGROOT.into());
    }
    Err(io::Error::new(
        io::ErrorKind::PermissionDenied,
        "no delegated cgroup subtree found; run as root or set CGLAB_ROOT to a cgroup directory you own",
    ))
}

/// One cgroup directory.
pub struct Group {
    pub path: PathBuf,
}

impl Group {
    /// `mkdir` = create a cgroup. The kernel populates the new directory with
    /// its control files automatically; which ones appear depends on the
    /// controllers the parent enabled in its `cgroup.subtree_control`.
    pub fn create(parent: &Path, name: &str) -> io::Result<Group> {
        let path = parent.join(name);
        fs::create_dir(&path)?;
        Ok(Group { path })
    }

    /// Write a control file, e.g. `set("memory.max", "64M")`.
    /// Each write must be a single `write(2)`; `fs::write` of a short string is.
    /// Invalid values are rejected by the kernel with an error (e.g. EINVAL),
    /// which is returned here.
    pub fn set(&self, file: &str, val: &str) -> io::Result<()> {
        fs::write(self.path.join(file), val)
    }

    /// Read a control file, trimmed.
    pub fn get(&self, file: &str) -> io::Result<String> {
        Ok(fs::read_to_string(self.path.join(file))?.trim().to_string())
    }

    /// Parse a "flat keyed" file (`key value\n` per line) like `cpu.stat` or
    /// `memory.events`. Non-numeric lines are skipped.
    /// Missing files yield an empty map rather than an error.
    pub fn kv(&self, file: &str) -> HashMap<String, u64> {
        let mut m = HashMap::new();
        if let Ok(s) = fs::read_to_string(self.path.join(file)) {
            for line in s.lines() {
                let mut it = line.split_whitespace();
                if let (Some(k), Some(v)) = (it.next(), it.next()) {
                    if let Ok(v) = v.parse() {
                        m.insert(k.to_string(), v);
                    }
                }
            }
        }
        m
    }

    /// Convenience: one value from a flat-keyed file (0 if absent).
    pub fn stat(&self, file: &str, key: &str) -> u64 {
        self.kv(file).get(key).copied().unwrap_or(0)
    }

    /// Kill everything in this cgroup and its descendants, then remove the
    /// directories bottom-up. `cgroup.kill` (Linux 5.14+) atomically SIGKILLs the
    /// whole subtree, avoiding the fork-race of walking `cgroup.procs`
    /// (a process could fork a new child between us reading the list and
    /// killing the entries).
    ///
    /// Child cgroups are subdirectories; control files are regular files, so
    /// `is_dir()` tells them apart. Errors are ignored: this is best-effort
    /// cleanup.
    pub fn destroy(&self) {
        if let Ok(rd) = fs::read_dir(&self.path) {
            for e in rd.flatten() {
                if e.path().is_dir() {
                    Group { path: e.path() }.destroy();
                }
            }
        }
        let _ = fs::write(self.path.join("cgroup.kill"), "1");
        // rmdir fails with EBUSY until the killed tasks have finished exiting
        // (and until all child cgroups are gone). Retry for up to ~2 s.
        for _ in 0..200 {
            if fs::remove_dir(&self.path).is_ok() {
                return;
            }
            sleep(Duration::from_millis(10));
        }
    }
}

/// A private cgroup subtree for one lab run: `<delegated>/cglab-<pid>`.
///
/// Note the shape (the v2 "no internal processes" rule in action):
///
/// ```text
///   cglab-<pid>/          <- interior node: has controllers enabled, NO processes
///     cpu-quota/          <- leaf: processes live here
///     mem-high/           <- leaf
/// ```
///
/// Removed automatically (with everything inside) when dropped.
pub struct Sandbox {
    pub group: Group,
}

impl Sandbox {
    pub fn new() -> io::Result<Sandbox> {
        let root = delegated_root()?;
        // Make sure cpu/memory/pids are available to *our* children: a controller
        // can only be enabled in a group if it is enabled in the parent's
        // subtree_control. (Idempotent if it already is.)
        let _ = fs::write(root.join("cgroup.subtree_control"), "+cpu +memory +pids");
        let group = Group::create(&root, &format!("cglab-{}", std::process::id()))?;
        // Enable the controllers for the leaves we will create below. This is
        // what makes files like `cpu.max`/`memory.max` appear in the leaves.
        group.set("cgroup.subtree_control", "+cpu +memory +pids").map_err(|e| {
            io::Error::new(e.kind(), format!("enabling controllers in {}: {e}", group.path.display()))
        })?;
        Ok(Sandbox { group })
    }

    /// Create a leaf group under the sandbox.
    pub fn leaf(&self, name: &str) -> io::Result<Group> {
        Group::create(&self.group.path, name)
    }
}

impl Drop for Sandbox {
    fn drop(&mut self) {
        self.group.destroy();
    }
}

/// Spawn `argv` **already inside** `group`.
///
/// `pre_exec` runs in the forked child *after fork, before exec*. We write "0" to
/// `cgroup.procs`, which means "move the calling process". So the program starts
/// life confined and can never run a single instruction outside its limits.
/// This is the same pattern Firecracker's `jailer` uses: it joins the cgroup
/// first, then (after further lock-down) exec()s Firecracker.
///
/// The alternative — spawn first, then write the child's PID into
/// `cgroup.procs` from the parent — leaves a window in which the child runs
/// unconfined and may already have forked.
pub fn spawn_in(group: &Group, argv: &[String]) -> io::Result<Child> {
    let procs = group.path.join("cgroup.procs");
    let mut cmd = Command::new(&argv[0]);
    cmd.args(&argv[1..]);
    // SAFETY: after fork() only the calling thread exists in the child, so code
    // there must not depend on locks another thread might have held (e.g. the
    // allocator's). The closure only opens a file and writes one byte (std
    // converts short paths to C strings on the stack), and this lab spawns
    // from a single-threaded parent, so no such lock can be held.
    unsafe {
        cmd.pre_exec(move || std::fs::write(&procs, "0"));
    }
    cmd.spawn()
}

/// Wait up to `d` for the child; `None` means it is still running.
/// (std has no wait-with-timeout, so poll `try_wait` every 25 ms.)
pub fn wait_timeout(child: &mut Child, d: Duration) -> io::Result<Option<ExitStatus>> {
    let start = Instant::now();
    loop {
        if let Some(st) = child.try_wait()? {
            return Ok(Some(st));
        }
        if start.elapsed() >= d {
            return Ok(None);
        }
        sleep(Duration::from_millis(25));
    }
}
