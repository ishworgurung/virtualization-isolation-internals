#!/usr/bin/env python3
"""
observe.py -- watch ioeventfd at work inside a real QEMU virtual machine.

Labs 07-09 built their own tiny VMM to show ioeventfd.  This script asks the
same question of QEMU, the VMM that real clouds and desktops run: when a guest
kicks a virtio disk, does the kick come back to QEMU's code, or does KVM deal
with it in the kernel?

The experiment
--------------
1. Build a tiny initramfs whose only program, /init (guest_init.c), reads the
   VM's disk 20 000 times, one 4 KiB read at a time.  One read = one kick.
2. Boot the host's own Linux kernel in QEMU with KVM, twice.  The only
   difference between the two boots is one device option:
       -device virtio-blk-pci,...,ioeventfd=off    (QEMU handles every kick)
       -device virtio-blk-pci,...,ioeventfd=on     (KVM turns each kick into an
                                                    eventfd signal)
3. While each guest runs, ask QEMU for KVM's per-vCPU counters (how many exits
   of each kind) just before and just after the workload.  The difference is
   what the workload caused, with the boot excluded.
4. List the special kernel-object fds (eventfds and friends) the QEMU process
   holds, to see the extra eventfds that ioeventfd=on creates.

With --strace, it instead boots once under strace and counts the KVM set-up
calls QEMU makes (KVM_IOEVENTFD, KVM_IRQFD, ...).

Words used below
----------------
QMP       "QEMU Machine Protocol": a control socket on a running QEMU.  You send
          one JSON command per line and get one JSON reply.  It's how tools
          like libvirt drive QEMU.
KVM stats KVM keeps counters for each vCPU: `exits` (all exits into KVM),
          `mmio_exits` (MMIO exits KVM couldn't handle, so they went to QEMU),
          `halt_exits` (the guest went idle), and more.  QEMU's QMP command
          `query-stats` reads them for us, so no root, perf or debugfs needed.
          (Needs Linux 5.14+ and QEMU 7.1+.)
virtio-blk-pci  the virtio disk, attached as a PCI device.  Its doorbell sits
          in a PCI memory region, so a kick is an MMIO write: that's why the
          counter to watch is `mmio_exits`.

Usage:  ./observe.py [iters]       A/B comparison (default 20000 reads)
        ./observe.py --strace      count QEMU's KVM_IOEVENTFD / KVM_IRQFD calls
        ./observe.py --verbose     also print every QMP command and reply
env:    KERNEL=/path/to/vmlinuz    guest kernel (default: the running kernel)
        QEMU=qemu-system-x86_64    QEMU binary
"""
import json, os, platform, re, shutil, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
STRACE = "--strace" in sys.argv
VERBOSE = "--verbose" in sys.argv
ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]  # positional args only
ITERS = int(ARGS[0]) if ARGS else 20000
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
# The guest runs the same kernel as the host.  The initramfs carries no kernel
# modules, so that kernel must have the virtio-pci and virtio-blk drivers built
# in (=y), as most distribution kernels do.
KERNEL = os.environ.get("KERNEL", f"/usr/lib/modules/{platform.release()}/vmlinuz")
WORK = tempfile.mkdtemp(prefix="qemu-observe-")  # scratch dir, removed at the end


# --------------------------------------------------------------- the guest

def build_initramfs():
    """Compile guest_init.c and pack it as the guest's initramfs.

    An initramfs is a "cpio" archive (an old, simple format like tar), here
    gzip-compressed.  The kernel unpacks it into RAM at boot and runs /init.
    Ours holds just /init plus two empty directories, /dev and /proc, for
    /init to mount things on.  Returns the archive's path."""
    root = os.path.join(WORK, "root")
    os.makedirs(os.path.join(root, "dev"))
    os.makedirs(os.path.join(root, "proc"))
    # -static: the archive has no shared libraries, so /init must contain all it needs.
    subprocess.check_call(["gcc", "-O2", "-static", "-o", os.path.join(root, "init"),
                           os.path.join(HERE, "guest_init.c")])
    out = os.path.join(WORK, "initramfs.gz")
    # "newc" is the cpio variant the kernel expects.
    cmd = f"cd {root} && find . | cpio -o -H newc --quiet | gzip -1 > {out}"
    subprocess.check_call(cmd, shell=True)
    return out


def make_disk():
    """Create the VM's disk: a 64 MiB file of zeros.

    truncate() only sets the file's size; no data blocks are written.  Such a
    "sparse" file takes no real space, and reading it costs the host almost
    nothing, so the measurement is about the virtio path, not the host disk."""
    disk = os.path.join(WORK, "disk.img")
    with open(disk, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    return disk


# ----------------------------------------------------- talking to QEMU (QMP)

class Qmp:
    """A minimal QMP client.

    The conversation over the Unix socket, one JSON object per line:
        QEMU  -> {"QMP": {...version...}}                  greeting on connect
        us    -> {"execute": "qmp_capabilities"}           required first command
        QEMU  -> {"return": {}}
        us    -> {"execute": "query-stats", "arguments": {...}}
        QEMU  -> {"return": [...]}                         or {"error": {...}}
    QEMU may also send {"event": ...} lines at any time (e.g. "the guest reset
    the device"); we skip those.  Run with --verbose to see it all."""

    def __init__(self, path):
        # QEMU creates the socket shortly after starting: retry for up to ~5 s.
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                self.s.close()
                time.sleep(0.05)
        else:
            raise RuntimeError(f"could not connect to QEMU's QMP socket {path}")
        self.f = self.s.makefile("rw")
        greeting = self.f.readline()
        if VERBOSE:
            print("  QMP <-", greeting.strip())
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        """Send one command and return QEMU's reply as a dict."""
        line = json.dumps({"execute": name, "arguments": args})
        if VERBOSE:
            print("  QMP ->", line)
        self.f.write(line + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if VERBOSE:
                print("  QMP <-", json.dumps(r)[:300])  # replies can be long
            if "return" in r or "error" in r:
                return r
            # anything else is an asynchronous event: keep reading


def kvm_stats(q):
    """Ask QEMU for KVM's per-vCPU counters, summed over all vCPUs.

    Returns {counter name: value}.  Some stats are histograms (lists of
    buckets); we keep only the plain numbers."""
    r = q.cmd("query-stats", target="vcpu", providers=[{"provider": "kvm"}])
    if "error" in r:
        raise RuntimeError(f"query-stats failed: {r['error']}")
    total = {}
    for vcpu in r["return"]:
        for st in vcpu["stats"]:
            if isinstance(st["value"], int):
                total[st["name"]] = total.get(st["name"], 0) + st["value"]
    return total


# ------------------------------------------------------- looking at QEMU

def fd_summary(pid):
    """Count the kernel-object fds that the QEMU process holds, by kind.

    Every open fd of a process appears as a symlink /proc/<pid>/fd/<n>.  For
    a normal file it points at the file's path.  Kernel objects that have no
    path, such as eventfds or KVM's VM and vCPU handles, point at a
    pseudo-name instead: "anon_inode:[eventfd]", "anon_inode:kvm-vm", ...
    We count those."""
    kinds = {}
    for fd in os.listdir(f"/proc/{pid}/fd"):
        try:
            target = os.readlink(f"/proc/{pid}/fd/{fd}")
        except OSError:
            continue  # the fd was closed while we were looking
        if target.startswith("anon_inode:"):
            kinds[target] = kinds.get(target, 0) + 1
    return kinds


def wait_for(path, needle, timeout):
    """Wait until the guest's serial log (a file QEMU writes) contains
    `needle`, then return the whole log.  On timeout, raise with the end of
    the log, which usually shows what went wrong in the guest."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(path) as f:
                data = f.read()
            if needle in data:
                return data
        except FileNotFoundError:
            pass  # QEMU hasn't created the file yet
        time.sleep(0.02)
    try:
        with open(path) as f:
            tail = f.read()[-800:]
    except FileNotFoundError:
        tail = "(QEMU never created the serial log)"
    raise TimeoutError(f"never saw {needle!r} in the guest's output; last lines:\n{tail}")


# ------------------------------------------------------------ running QEMU

def qemu_cmd(initrd, disk, ioeventfd, serial, sock):
    """Build the QEMU command line.  Option by option:
      -machine q35,accel=kvm   a modern PC; run the guest on KVM (hardware
                               virtualisation), not TCG (QEMU's CPU emulator)
      -cpu host                give the guest the host CPU's features
      -smp 1 -m 256            one vCPU (so there's one set of counters), 256 MiB RAM
      -display none            no window
      -no-reboot               if the guest reboots or panics, exit instead
      -serial file:...         the guest's serial port (its console) goes to a file
      -qmp unix:...            the QMP control socket; wait=off: start the guest
                               without waiting for us to connect
      -kernel/-initrd/-append  boot this kernel directly, with this initramfs and
                               kernel command line; no disk bootloader involved
      -drive ... cache=none    QEMU opens disk.img with O_DIRECT (no host page cache)
                 aio=threads   QEMU does the disk I/O in its own thread pool
      -device virtio-blk-pci   the virtio disk under test"""
    tag = "on" if ioeventfd else "off"
    return [QEMU, "-machine", "q35,accel=kvm", "-cpu", "host", "-smp", "1", "-m", "256",
            "-display", "none",
            "-no-reboot",
            "-serial", f"file:{serial}",
            "-qmp", f"unix:{sock},server=on,wait=off",
            "-kernel", KERNEL, "-initrd", initrd,
            "-append", f"console=ttyS0 quiet panic=-1 iters={ITERS}",
            "-drive", f"file={disk},if=none,id=disk0,format=raw,cache=none,aio=threads",
            # THE variable under test: does QEMU register an ioeventfd for this
            # device's doorbell?
            "-device", f"virtio-blk-pci,drive=disk0,ioeventfd={tag}"]


def run(initrd, disk, ioeventfd):
    """Boot one guest and measure its workload.

    Returns (counter changes during the workload, guest-measured ms, fd kinds)."""
    tag = "on" if ioeventfd else "off"
    serial = os.path.join(WORK, f"serial-{tag}.log")
    sock = os.path.join(WORK, f"qmp-{tag}.sock")
    qemu_log = os.path.join(WORK, f"qemu-{tag}.log")
    with open(qemu_log, "w") as log:
        p = subprocess.Popen(qemu_cmd(initrd, disk, ioeventfd, serial, sock), stdout=log, stderr=log)
    try:
        q = Qmp(sock)
        wait_for(serial, "START", 60)  # boot finished; the workload starts in 1 s
        before = kvm_stats(q)
        fds = fd_summary(p.pid)
        out = wait_for(serial, "DONE", 120)
        after = kvm_stats(q)
        elapsed = float(out.split("elapsed_ms=")[1].split()[0])
        # after - before = what the workload caused (boot-time exits excluded)
        return {k: after[k] - before.get(k, 0) for k in after}, elapsed, fds
    except Exception:
        # Show QEMU's own error messages: they usually explain the failure.
        with open(qemu_log) as f:
            msg = f.read().strip()
        if msg:
            print(f"QEMU said:\n{msg}", file=sys.stderr)
        raise
    finally:
        p.kill()  # the guest never shuts down by itself
        p.wait()


def strace_mode(initrd, disk):
    """Boot once under strace and count the KVM set-up calls QEMU makes.

    strace prints every system call a process makes.  We trace only ioctl(),
    the call QEMU uses to talk to KVM, and count four kinds:
      KVM_CREATE_IRQCHIP   create KVM's in-kernel interrupt controller
      KVM_SET_GSI_ROUTING  tell KVM how interrupt numbers map to deliveries
                           (for PCI devices: MSI messages)
      KVM_IOEVENTFD        "when the guest writes here, signal this eventfd"
      KVM_IRQFD            "when this eventfd is signalled, interrupt the guest"
    """
    if not shutil.which("strace"):
        sys.exit("strace not installed")
    serial, sock, log = (os.path.join(WORK, n) for n in ("serial-t.log", "qmp-t.sock", "strace.log"))
    if VERBOSE:
        print(f"strace log: {log}\nguest serial log: {serial}")

    # -f: follow all of QEMU's threads; -qq: no attach/exit messages;
    # -e trace=ioctl: only ioctl() calls; -o: write to a file.
    cmd = ["strace", "-f", "-qq", "-e", "trace=ioctl", "-o", log] + qemu_cmd(initrd, disk, True, serial, sock)
    p = subprocess.Popen(cmd, stderr=subprocess.DEVNULL)
    try:
        # strace slows every ioctl down, KVM_RUN included, so the guest crawls
        # and we don't wait for the workload.  QEMU registers its ioeventfds
        # while building the machine and again when the guest's driver
        # switches the device on, all within the first few seconds; 40 s is
        # plenty.
        print("booting under strace for 40 s ...", flush=True)
        time.sleep(40)
    finally:
        # strace doesn't exit on SIGTERM while its traced program runs:
        # kill QEMU (strace's child) first, then strace.
        subprocess.call(["pkill", "-KILL", "-P", str(p.pid)])
        p.kill()
        p.wait()
    with open(log) as f:
        text = f.read()
    print("KVM set-up ioctls issued by QEMU during ~40 s of (strace-slowed) boot:")
    for name in ["KVM_CREATE_IRQCHIP", "KVM_SET_GSI_ROUTING", "KVM_IOEVENTFD", "KVM_IRQFD"]:
        # strace writes lines like "ioctl(12, KVM_IOEVENTFD, 0x7f...) = 0".
        # Matching "NAME," avoids counting longer names that start the same.
        print(f"  {name:<22} x{len(re.findall(name + ',', text))}")
    print("\n(strace shows each call's argument as a bare pointer, so it can't tell you the"
          "\n doorbell address or the fd.  For that: attach a debugger to QEMU, or, as root,"
          "\n trace the kernel functions kvm_ioeventfd() and kvm_irqfd() with bpftrace.)")


# ------------------------------------------------------------------- main

def main():
    # Check the requirements first, with messages that say what to do.
    if not shutil.which(QEMU):
        sys.exit(f"{QEMU} not found (install QEMU, or set QEMU=/path/to/qemu-system-x86_64)")
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        sys.exit("/dev/kvm not accessible (is KVM enabled? are you in the 'kvm' group?)")
    if not os.access(KERNEL, os.R_OK):
        sys.exit(f"kernel image {KERNEL} not readable; set KERNEL=/path/to/vmlinuz")

    initrd = build_initramfs()
    disk = make_disk()

    if STRACE:
        strace_mode(initrd, disk)
        return

    print(f"kernel   : {KERNEL}\n"
          f"workload : {ITERS} x 4 KiB O_DIRECT reads on virtio-blk-pci, one at a time\n")
    res = {}
    for io in (False, True):
        print(f"booting guest with ioeventfd={'on' if io else 'off'} ...", flush=True)
        res[io] = run(initrd, disk, io)

    off, on = res[False], res[True]
    # KVM's names for its per-vCPU counters.
    keys = ["exits", "mmio_exits", "io_exits", "halt_exits", "irq_exits"]
    print(f"\n{'KVM vCPU counter during the workload':<40}{'ioeventfd=off':>16}{'ioeventfd=on':>16}")
    for k in keys:
        if k in off[0]:
            print(f"  {k:<38}{off[0][k]:>16}{on[0].get(k, 0):>16}")
    print(f"  {'guest-measured elapsed (ms)':<38}{off[1]:>16.1f}{on[1]:>16.1f}")
    print(f"  {'us per 4K read':<38}{off[1] * 1e3 / ITERS:>16.2f}{on[1] * 1e3 / ITERS:>16.2f}")
    print("\nkernel-object fds held by the QEMU process:")
    for io in (False, True):
        kinds = ", ".join(f"{k.split(':', 1)[1]} x{v}" for k, v in sorted(res[io][2].items()))
        print(f"  ioeventfd={'on ' if io else 'off'}: {kinds}")


if __name__ == "__main__":
    try:
        main()
    finally:
        shutil.rmtree(WORK, ignore_errors=True)  # remove the scratch dir, even on errors
