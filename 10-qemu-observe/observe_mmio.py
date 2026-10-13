#!/usr/bin/env python3
"""
observe_mmio.py -- the same experiment as observe.py, but the virtio disk is
attached with the virtio-mmio transport instead of virtio-pci.

Read observe.py first: this script imports it and reuses everything (the QMP
client, the KVM counters, booting and timing the guest).  Only three things
differ, and they're all in this file:

1. The machine.  virtio-mmio devices need no PCI bus, and QEMU's "microvm"
   machine is built for that: no PCI, and almost none of a PC's legacy
   hardware beyond a serial port and interrupt controllers.  It's QEMU's
   answer to Firecracker, which also uses virtio-mmio only.  The guest learns
   where each virtio-mmio device is (address and interrupt line) from small
   ACPI tables that microvm generates.  (With -machine microvm,acpi=off, QEMU
   instead adds "virtio_mmio.device=..." to the kernel command line, which
   guest_init_virt_mmio.c passes to the module.)
       q35     + -device virtio-blk-pci       (observe.py)
       microvm + -device virtio-blk-device    (here: "-device" is the plain
                                               virtio-blk, and microvm plugs it
                                               into one of its virtio-mmio slots)

2. How ioeventfd is switched.  On virtio-pci, ioeventfd is an option of the
   device.  On virtio-mmio it's an option of the transport, the "virtio-mmio"
   slot the device sits in, so it's set for all slots at once:
       -global virtio-mmio.ioeventfd=on|off
   We also set force-legacy=off, to use virtio-mmio version 2 (the modern
   one, as Firecracker does) rather than QEMU's default, the legacy version 1.

3. The guest program.  Distribution kernels ship the virtio-mmio driver as a
   module, not built in, so the initramfs also carries virtio_mmio.ko and the
   guest's /init (guest_init_virt_mmio.c) loads it before opening the disk.

What's the same: the kick is still an MMIO write, now to the device's single
QueueNotify register (the guest writes the queue's number there).  So the
counter to watch is still `mmio_exits`.

Usage:  ./observe_mmio.py [iters]      A/B comparison (default 20000 reads)
        ./observe_mmio.py --strace     count QEMU's KVM_IOEVENTFD / KVM_IRQFD calls
        ./observe_mmio.py --verbose    also print every QMP command and reply
        ./observe_mmio.py --boot-log   also show each guest's console live: the
                                       kernel's boot messages, then /init's output
env:    KERNEL=/path/to/vmlinuz        guest kernel (default: the running kernel)
        MODULES=/usr/lib/modules/<version>   where that kernel's modules are
        QEMU=qemu-system-x86_64        QEMU binary
"""
import glob, os, platform, shutil, subprocess, sys, threading, time

import observe  # the PCI version: same directory
from observe import ITERS, KERNEL, QEMU, STRACE, WORK, HERE

MODULES = os.environ.get("MODULES", f"/usr/lib/modules/{platform.release()}")
BOOT_LOG = "--boot-log" in sys.argv


def find_virtio_mmio_module():
    """Find virtio_mmio.ko for the guest kernel.  Returns the path, or None if
    it isn't a module (then it's built in and nothing needs loading).

    Module files live under /usr/lib/modules/<kernel version>/kernel/..., often
    compressed: virtio_mmio.ko.zst, .ko.xz or .ko.gz.  They only fit the exact
    kernel they were built with, which is why MODULES must match KERNEL."""
    found = glob.glob(os.path.join(MODULES, "kernel/drivers/virtio/virtio_mmio.ko*"))
    return found[0] if found else None


def build_initramfs():
    """Like observe.build_initramfs(), with two changes: /init is compiled from
    guest_init_virt_mmio.c, and the archive gets a /modules directory holding
    virtio_mmio.ko for /init to load."""
    root = os.path.join(WORK, "root")
    for d in ("dev", "proc", "modules"):
        os.makedirs(os.path.join(root, d))
    subprocess.check_call(["gcc", "-O2", "-static", "-o", os.path.join(root, "init"),
                           os.path.join(HERE, "guest_init_virt_mmio.c")])
    module = find_virtio_mmio_module()
    if module:
        shutil.copy(module, os.path.join(root, "modules"))
    out = os.path.join(WORK, "initramfs.gz")
    subprocess.check_call(f"cd {root} && find . | cpio -o -H newc --quiet | gzip -1 > {out}", shell=True)
    return out


def qemu_cmd(initrd, disk, ioeventfd, serial, sock):
    """The QEMU command line.  Compared with observe.qemu_cmd():
      -machine microvm,accel=kvm          instead of q35: no PCI bus at all
      -global virtio-mmio.ioeventfd=...   THE variable under test, set on the
                                          transport instead of the device
      -global virtio-mmio.force-legacy=off   modern virtio-mmio (version 2)
      -device virtio-blk-device           virtio-blk with no transport attached;
                                          microvm puts it in a virtio-mmio slot
    Everything else (CPU, memory, serial log, QMP, kernel, disk file) is the same."""
    tag = "on" if ioeventfd else "off"
    return [QEMU, "-machine", "microvm,accel=kvm", "-cpu", "host", "-smp", "1", "-m", "256",
            "-display", "none",
            "-no-reboot",
            "-serial", f"file:{serial}",
            "-qmp", f"unix:{sock},server=on,wait=off",
            "-kernel", KERNEL, "-initrd", initrd,
            # "quiet" hides the kernel's boot messages.  --boot-log wants all of
            # them: loglevel=7 prints everything except debug messages.
            "-append", f"console=ttyS0 {'loglevel=7' if BOOT_LOG else 'quiet'} panic=-1 iters={ITERS}",
            "-global", f"virtio-mmio.ioeventfd={tag}",
            "-global", "virtio-mmio.force-legacy=off",
            "-drive", f"file={disk},if=none,id=disk0,format=raw,cache=none,aio=threads",
            "-device", "virtio-blk-device,drive=disk0"]


# observe.run() and observe.strace_mode() build their command line by calling
# observe.qemu_cmd().  Replacing it there makes them boot our microvm instead.
observe.qemu_cmd = qemu_cmd


def follow(path, stop):
    """Print lines from `path` as they're added, until `stop` is set, like
    `tail -f`.  `path` is the guest's serial log: QEMU writes the guest
    console into it byte by byte, so we print only complete lines."""
    pos, partial = 0, ""
    while True:
        done = stop.is_set()  # checked first, so we still read the last bytes
        try:
            with open(path, errors="replace") as f:
                f.seek(pos)
                partial += f.read()
                pos = f.tell()
        except FileNotFoundError:
            pass  # QEMU hasn't created it yet
        *lines, partial = partial.split("\n")
        for line in lines:
            print("  guest | " + line.rstrip("\r"), flush=True)
        if done:
            return
        time.sleep(0.05)


def run(initrd, disk, ioeventfd):
    """observe.run(), plus the live console when --boot-log is given.

    The guest's console already goes to a file (-serial file:...), which
    observe.run() watches for START and DONE.  We follow the same file from a
    second thread and print it.  The name must match observe.run()'s.

    The boot messages don't disturb the measurement.  Each byte the guest
    prints is a port I/O exit (the serial port is an emulated 8250 UART), but
    the kernel prints them during boot, before the "before" snapshot."""
    if not BOOT_LOG:
        return observe.run(initrd, disk, ioeventfd)
    serial = os.path.join(WORK, f"serial-{'on' if ioeventfd else 'off'}.log")
    stop = threading.Event()
    t = threading.Thread(target=follow, args=(serial, stop))
    t.start()
    try:
        return observe.run(initrd, disk, ioeventfd)
    finally:
        stop.set()
        t.join()


def main():
    if not shutil.which(QEMU):
        sys.exit(f"{QEMU} not found (install QEMU, or set QEMU=/path/to/qemu-system-x86_64)")
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        sys.exit("/dev/kvm not accessible (is KVM enabled? are you in the 'kvm' group?)")
    if not os.access(KERNEL, os.R_OK):
        sys.exit(f"kernel image {KERNEL} not readable; set KERNEL=/path/to/vmlinuz")

    initrd = build_initramfs()
    disk = observe.make_disk()

    if STRACE:
        observe.strace_mode(initrd, disk)
        return

    print(f"kernel   : {KERNEL}\n"
          f"module   : {find_virtio_mmio_module() or '(virtio_mmio built in)'}\n"
          f"workload : {ITERS} x 4 KiB O_DIRECT reads on virtio-blk over virtio-mmio (microvm), one at a time\n")
    res = {}
    for io in (False, True):
        print(f"booting guest with virtio-mmio.ioeventfd={'on' if io else 'off'} ...", flush=True)
        res[io] = run(initrd, disk, io)

    # The same table as observe.py prints.
    off, on = res[False], res[True]
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
