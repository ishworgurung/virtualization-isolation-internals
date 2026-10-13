# 10 — ioeventfd in a real QEMU virtual machine

**Builds on:** [07-ioeventfd](../07-ioeventfd/), [08-irqfd](../08-irqfd/) and [09-kick-roundtrip](../09-kick-roundtrip/); [01-virtio](../01-virtio/) for virtio itself.

Labs `07`–`09` showed ioeventfd with a VMM written from scratch, a few hundred lines
of C. This lab checks that the same thing happens in **QEMU**, the VMM that real
clouds and desktops use, running a **real Linux kernel** as the guest.

The question is the same one as in lab 09: when the guest kicks a virtio disk,
does the kick come back to QEMU's code, or does KVM deal with it inside the
kernel? This time we don't write the VMM. We flip one QEMU option and read KVM's
own counters.

## Words you'll need

| Term | Meaning |
| --- | --- |
| **KVM** | The part of the Linux kernel that runs virtual machines on the CPU's virtualisation hardware. |
| **VMM** | The user program that creates and drives a VM through KVM, and pretends to be its devices. |
| **QEMU** | The most widely used VMM. It can also emulate a whole CPU in software (TCG); with `accel=kvm`, as here, it leaves the CPU to KVM. |
| **guest** | Everything running inside the VM: here a Linux kernel plus one small program. |
| **vCPU** | One of the guest's CPUs. In QEMU it's a thread that calls `ioctl(KVM_RUN)` to run guest code. |
| **VM exit** | The guest stops and the CPU switches to KVM. Some exits KVM handles itself and resumes the guest quickly; the rest it passes up to QEMU (`KVM_RUN` returns), which is much slower. |
| **MMIO** | Talking to a device with ordinary memory writes to special addresses. A virtio doorbell is one. See [lab 09](../09-kick-roundtrip/README.md#mmio-talking-to-a-device-with-ordinary-memory-writes). |
| **kick** | The guest's write to a device's doorbell: "there's a new request for you". |
| **virtio** | A standard for virtual devices, so one guest driver works with any VMM. [Below](#virtio-and-qemu-the-pieces-this-lab-uses) has the details. |
| **virtio-blk** | The standard virtual disk. `virtio-blk-pci` attaches it as a PCI device; the guest sees it as `/dev/vda`. |
| **ioeventfd** | A KVM feature ([lab 07](../07-ioeventfd/)): "when the guest writes to this doorbell, signal this eventfd and resume the guest". The kick then never reaches QEMU's vCPU thread. |
| **irqfd** | The opposite direction ([lab 08](../08-irqfd/)): "when this eventfd is signalled, interrupt the guest". |
| **initramfs** | A small archive of files the kernel unpacks into RAM at boot. It runs the archive's `/init` as the first program. |
| **QMP** | QEMU's control socket: JSON commands in, JSON replies out. Tools like libvirt use it to manage VMs. |

## What the experiment does

```text
  observe.py (the host)                  QEMU process               the guest
  ─────────────────────                  ────────────               ─────────
  1. compile guest_init.c, pack it
     into an initramfs
  2. start QEMU  ───────────────────────▶ boots your host's ──────▶ Linux kernel
                                          kernel image               runs /init
                                                                     (guest_init.c)
  3. wait for "START" on the serial  ◀────────────────────────────── prints START
     log, then snapshot KVM's exit
     counters (exits, mmio_exits,
     ...) ── QMP query-stats ──────────▶ reads them from KVM
  4.                                                                 reads /dev/vda
                                                                     20 000 times,
                                                                     4 KiB each
  5. wait for "DONE", snapshot the   ◀────────────────────────────── prints DONE
     same counters again; after                                      elapsed_ms=...
     minus before = what the reads
     caused
  6. kill QEMU, repeat with the other ioeventfd setting
```

**The counters** are KVM's per-vCPU statistics. KVM keeps these running totals
in the kernel whether or not anyone looks at them, and adds 1 every time a
particular event happens:

| Counter | Adds 1 each time... |
| --- | --- |
| `exits` | the guest stops and the CPU switches to KVM, for any reason |
| `mmio_exits` | an MMIO access KVM can't handle itself, so `KVM_RUN` returns to QEMU (the kicks, when ioeventfd is off) |
| `io_exits` | the same for port I/O (the `in`/`out` instructions) |
| `halt_exits` | the guest runs `hlt` because it has nothing to do |
| `irq_exits` | a host hardware interrupt pulls the CPU out of the guest |

KVM makes them available through a special file descriptor per vCPU, which QEMU
holds (`kvm-vcpu-stats:0` in the results). QMP's `query-stats` command returns
their current values. They only ever go up and include everything since the VM
started, boot included. That's why the script reads them twice and subtracts:
only the reads happen between the two snapshots.

Things to notice:

* **One read = one kick.** `guest_init.c` reads with `pread()`, which waits for each
  read to finish before the next starts ("queue depth 1"). So the guest's virtio
  driver kicks the disk exactly once per read, 20 000 times.
* **Only one thing changes between the two runs:** the device option
  `-device virtio-blk-pci,...,ioeventfd=off` or `ioeventfd=on`.
* **No disk image and no distribution.** The guest runs your host's own kernel,
  and its only program is `guest_init.c`, in an initramfs. Its disk is an empty
  64 MiB file, so the host has nothing real to read and the numbers are about the
  virtio path, not your SSD.
* **No root, perf or debugfs.** QEMU already has the counters' file descriptor, so
  asking QEMU is enough.

## Virtio and QEMU: the pieces this lab uses

The QEMU command line in [observe.py](observe.py) has two lines that set up the
disk:

```text
-drive  file=disk.img,if=none,id=d0,format=raw,cache=none,aio=threads
-device virtio-blk-pci,drive=d0,ioeventfd=on
```

This section explains the technology behind them, from the guest's driver down to
the file on the host.

### Virtio: one driver for every VMM

Before virtio, VMMs pretended to be real hardware, such as an IDE disk controller
or an Intel network card. The guest used its ordinary drivers, but those drivers
were written for real chips. They poke many registers per operation, and every
poke is a VM exit.

**Virtio** is a set of devices designed *for* virtual machines. It's a published
standard (OASIS "Virtual I/O Device"), so the same guest driver works with QEMU,
Firecracker, cloud-hypervisor and others. A virtio device has three parts:

| Part | What it is | In this lab |
| --- | --- | --- |
| **device type** | What the device does: block (disk), net, console, rng, vsock, gpu, fs, ... | `blk` |
| **transport** | How the guest finds the device and reaches its registers: PCI, MMIO or (on IBM mainframes) CCW | `pci` |
| **virtqueues** | The rings in guest memory that carry the actual requests | one queue |

So `virtio-blk-pci` reads as "a virtio block device, attached over PCI".

### Virtqueues: the requests live in guest memory

The doorbell only says "look at the queue". The request itself is written into
guest RAM, in a **virtqueue**, which both the guest driver and the device can
read and write. Lab [01-virtio](../01-virtio/) builds one from scratch. A virtqueue
has three parts:

* **descriptor table:** entries saying "a buffer at guest address A, length L,
  readable (or writable) by the device";
* **available ring:** the driver adds the numbers of descriptors ready for the
  device;
* **used ring:** the device adds the ones it has finished.

One of this lab's 4 KiB reads looks like this:

```text
guest driver                                     QEMU (the device)
────────────                                     ─────────────────
1. write 3 descriptors:
     [header: "read (IN), sector S"]  device reads
     [data: 4 KiB buffer]             device writes
     [status: 1 byte]                 device writes
2. put the chain on the available ring
3. write the queue number to the doorbell ───▶ 4. take the chain from the available
   (the kick)                                    ring, read 4 KiB of disk.img into
                                                 the guest's buffer, write status
                                                 = OK
                                              5. put the chain on the used ring
6. interrupt wakes the driver  ◀──────────── 6. send the interrupt (MSI-X)
7. read the used ring, return the data
   to pread()
```

Only step 3 and step 6 cross between guest and host. Everything else is plain
memory access, which is why virtio is fast. This lab counts step 3.

**Kicks can be skipped.** Under load, a device can tell the driver "don't kick me,
I'm already working through the queue", and a driver can tell the device "don't
interrupt me yet". That's the `EVENT_IDX` feature, on by default in QEMU. In this
lab there's only ever one request in flight, so the device is always idle when
the next one arrives and every request is kicked: 20 000 reads, 20 000 kicks.

### virtio-blk: the virtual disk

virtio-blk is about the simplest disk possible. Each request is a header (the
operation and a sector number), data buffers and a status byte, as above. The
operations are few: read (`IN`), write (`OUT`), flush, discard, write-zeroes and
"get the disk's ID". The guest sees `/dev/vda`, `/dev/vdb`, and so on.

It supports **multiqueue**: several virtqueues, typically one per vCPU, so vCPUs
don't compete for one queue. QEMU's `num-queues` property sets how many. With this
lab's single vCPU there's one.

Its sibling **virtio-scsi** is a SCSI controller instead: one device carries many
disks, the guest sees `/dev/sda`, and it supports more SCSI features. The cost is
an extra protocol layer per request. Clouds commonly use either one.

### virtio-pci: finding the device and ringing its doorbell

With the PCI transport, the virtio device looks like an ordinary PCI card to the
guest (vendor ID `0x1af4`). The guest kernel finds it when it scans the PCI bus,
just like a real card, and loads the virtio-pci driver for it, which then hands it
to virtio-blk.

A PCI card has **BARs** ("base address registers"): ranges of addresses where its
registers appear. The firmware or the guest kernel picks those addresses at boot.
In modern virtio-pci, the device's registers, including a **notify** area (the
doorbells), sit in a *memory* BAR. So a kick is an MMIO write, which is why it shows
up in the results as `mmio_exits`, not `io_exits`. Older "legacy" virtio-pci put
its registers in an *I/O-port* BAR instead. Its kicks would have been `io_exits`.

Interrupts go the other way as **MSI-X**. Instead of a dedicated interrupt wire, the
device raises an interrupt by writing a small message to a special address, and
each virtqueue can have its own interrupt ("vector"). KVM delivers these messages,
either through an irqfd or through QEMU's `KVM_SIGNAL_MSI` ioctl.

### How `/dev/vda` appears in the guest

Nothing inside the guest creates the disk. The QEMU command line does, the same way
plugging a drive into a PC does: `-device virtio-blk-pci,drive=d0` adds a PCI card
to the virtual machine's hardware before the guest starts. The guest kernel then
*discovers* it at boot:

```text
QEMU command line                 -device virtio-blk-pci,drive=d0
  → a PCI card exists in the VM   (vendor 0x1af4, "virtio block")
guest kernel boots
  → scans the PCI bus             reads each slot's ID registers
  → virtio-pci driver claims it   "vendor 0x1af4 is mine"
  → asks the device its type      block → hand it to the virtio-blk driver
  → virtio-blk sets it up         reads the capacity (64 MiB), creates its virtqueue
  → registers a disk named "vda"  "vd" = virtio disk, "a" = the first one
devtmpfs
  → /dev/vda appears              the kernel adds the file for every disk it registers
```

Two details in [guest_init.c](guest_init.c) come from this:

* It mounts **devtmpfs** on `/dev`. That's the kernel's own `/dev`, filled in
  automatically, so nobody has to create device files by hand. (On a normal
  distribution, udev adds names and permissions on top.)
* It **retries opening `/dev/vda` for up to 10 s**, because the kernel may start
  `/init` before the virtio-blk driver has finished the steps above.

A second `-device virtio-blk-pci` would appear as `/dev/vdb`, and so on.

### virtio-mmio: no PCI at all

**virtio-mmio** is the lighter alternative. There's no PCI bus to scan: the guest
kernel is told "there's a virtio device at address X, interrupt Y" on its command
line, in a device tree, or in ACPI tables. [observe_mmio.py](observe_mmio.py) tries it. Every queue shares one `QueueNotify` register, and the
guest writes the queue's number into it. Firecracker uses virtio-mmio, because it
boots faster and needs less emulated hardware.

### QEMU's side: frontend and backend

QEMU splits a disk in two, which is why there are two options:

```text
     guest                          QEMU                               host
┌────────────┐   ┌──────────────────────┐   ┌──────────────────────┐   ┌──────────┐
│ virtio-blk │──▶│ -device              │──▶│ -drive               │──▶│ disk.img │
│ driver     │   │ virtio-blk-pci       │   │ block layer:         │   │ (a file) │
│ (/dev/vda) │   │ FRONTEND: what the   │   │ BACKEND: where the   │   └──────────┘
└────────────┘   │ guest sees           │   │ bytes live, and how  │
                 └──────────────────────┘   │ they're read/written │
                                            └──────────────────────┘
```

The `drive=d0` / `id=d0` pair connects the two. You can swap either side on its
own: the same `disk.img` could be attached as `virtio-scsi` or an IDE disk
instead, and the same `virtio-blk-pci` could be backed by a network disk.

The backend options in this lab:

| Option | Meaning | Alternatives |
| --- | --- | --- |
| `format=raw` | disk.img is a plain byte-for-byte image | `qcow2`: QEMU's own format, which grows as it's written and supports snapshots and backing files (a base image shared by many VMs) |
| `cache=none` | QEMU opens disk.img with `O_DIRECT`, bypassing the **host's** page cache. The guest has its own page cache, so caching again on the host would waste memory. | `writeback` (the default: use the host cache), `unsafe` (also ignore flushes; fast, loses data on a crash) |
| `aio=threads` | QEMU does the file reads with a pool of worker threads | `native` (Linux AIO; needs `cache=none`), `io_uring` |
| `if=none` | don't attach a disk automatically: `-device` does it | `if=virtio`, `if=ide`: shortcuts that do both in one option |

The frontend option that matters here is `ioeventfd=on|off`. It decides which QEMU
thread first runs the device code when the guest kicks:

* **off:** the vCPU thread. `KVM_RUN` returns with `KVM_EXIT_MMIO`, QEMU sees a
  write to the notify area, and runs virtio-blk's queue handler before resuming
  the guest.
* **on:** QEMU's **event loop**. That's a thread that sleeps until one of many fds
  becomes ready, then runs that fd's handler. The ioeventfd is one of those fds.
  By default it's QEMU's main loop thread. With `iothread=`, it's a dedicated
  **iothread**, so disk work doesn't compete with QEMU's other jobs (the QMP
  socket, timers, ...). The `[io_uring]` fds in the results are most likely QEMU's
  event loops, which can use io_uring to wait on their fds.

In both cases the same code in QEMU's virtio-blk device (`hw/block/virtio-blk.c`)
takes the request off the virtqueue and hands it to the block layer. Only the
thread that runs it changes.

### Moving the device out of QEMU

ioeventfd and irqfd reduce a device to "wait on one eventfd, write another". Once
that's true, the device code doesn't have to be in QEMU at all:

| Setup | Where the device code runs | Examples |
| --- | --- | --- |
| QEMU, `ioeventfd=off` | QEMU's vCPU thread | this lab's first run |
| QEMU, `ioeventfd=on` | QEMU's main loop or an iothread | this lab's second run |
| **vhost** | a kernel thread (`vhost-<qemu pid>`), which waits on the ioeventfd and signals the irqfd itself | vhost-net, vhost-scsi, vhost-vsock |
| **vhost-user** | another user process. QEMU sends it the eventfds and a memfd of guest memory with `SCM_RIGHTS` ([lab 11](../11-scm-rights/)) | vhost-user-blk (`qemu-storage-daemon`, SPDK), virtiofsd, DPDK switches |
| **vDPA** | hardware: a network card that understands virtqueues | SmartNICs, some cloud NICs |

In every row after the first, QEMU is only on the control path: it sets the device
up, then stays out of the way of each request.

## Run it

```sh
./observe.py              # the A/B comparison, 20 000 reads (a few seconds)
./observe.py 50000        # more reads
./observe.py --verbose    # also print the JSON conversation with QEMU
./observe.py --strace     # instead: count the KVM set-up calls QEMU makes (~1 min)
```

Needs `/dev/kvm` access, `qemu-system-x86_64` (7.1 or newer), `gcc` with static libc,
`cpio` and `gzip`, plus `strace` for `--strace`.

The guest kernel defaults to your running kernel. The initramfs has no kernel
modules, so that kernel needs the virtio-pci and virtio-blk drivers built in
(`CONFIG_VIRTIO_PCI=y`, `CONFIG_VIRTIO_BLK=y`), as most distribution kernels do.
To use another one: `KERNEL=/path/to/vmlinuz ./observe.py`.

## Results (this machine)

Linux 7.2, QEMU 11.1, 20 000 reads:

```text
KVM vCPU counter during the workload       ioeventfd=off    ioeventfd=on
  exits                                            41446           41471
  mmio_exits                                       20000               0
  io_exits                                            55              55
  halt_exits                                       20149           20226
  irq_exits                                           77              95
  guest-measured elapsed (ms)                      268.6           242.7
  us per 4K read                                   13.43           12.13

kernel-object fds held by the QEMU process:
  ioeventfd=off: [eventfd] x5, [io_uring] x3, [signalfd] x1, kvm-vcpu-stats:0 x1, kvm-vcpu:0 x1, kvm-vm x1
  ioeventfd=on : [eventfd] x8, [io_uring] x3, [signalfd] x1, kvm-vcpu-stats:0 x1, kvm-vcpu:0 x1, kvm-vm x1
```

Each number is a counter's change between START and DONE, so it counts the 20 000
reads and nothing else. The [counter table](#what-the-experiment-does) above says
what each one counts.

### How to read it

* **`mmio_exits` 20 000 → 0: that's ioeventfd working.** With it off, all 20 000
  kicks came back to QEMU's vCPU thread. With it on, not one did: KVM recognised the
  doorbell address, signalled QEMU's eventfd, and resumed the guest by itself.
* **But `exits` stays about 41 400 either way.** ioeventfd doesn't stop the guest
  from exiting. The doorbell write still stops the guest and switches to KVM (it
  has to: that's how KVM finds out). What disappears is the second, more expensive
  step: leaving the kernel to run QEMU's code, then coming back. The arithmetic
  works out: about 20 000 kicks + about 20 000 halts + roughly 1 000 others (of kinds
  the table doesn't list) ≈ 41 400, in both runs.
* **`halt_exits` ≈ 20 000: the guest idles once per read.** After each kick the
  guest's program waits for its data, and the guest has nothing else to run, so it
  halts until the disk's "done" interrupt wakes it. That interrupt arrives while the
  guest is halted, which is why it doesn't show up as extra exits of its own.
* **The time is about the same, and that's expected.** In four runs on this
  machine, the total ranged from 237 to 292 ms with ioeventfd off, and from 243 to
  302 ms with it on. Sometimes one was faster, sometimes the other. It's run-to-run
  noise, not a win for either. With one request at a time, the guest just waits
  either way, and ioeventfd trades the trip to QEMU for a hand-off from KVM to
  another QEMU thread, which also costs time. Lab 09 measured the same thing in its
  ping-pong mode. ioeventfd pays off with many requests in flight, and by keeping
  the vCPU thread free, not in single-request latency.
* **Three more eventfds with ioeventfd on (5 → 8):** the eventfds QEMU created to
  register with `KVM_IOEVENTFD`. The listing also shows QEMU's handles on KVM
  itself: `kvm-vm` (the VM), `kvm-vcpu:0` (the one vCPU), and `kvm-vcpu-stats:0`
  (the counters that `query-stats` reads).

### What `--strace` shows

strace lists every system call a program makes. `--strace` boots once (with
ioeventfd on) under strace and counts QEMU's KVM set-up calls:

```text
KVM set-up ioctls issued by QEMU during ~40 s of (strace-slowed) boot:
  KVM_CREATE_IRQCHIP     x1     create KVM's in-kernel interrupt controller
  KVM_SET_GSI_ROUTING    x5     tell KVM how to deliver each interrupt number
  KVM_IOEVENTFD          x10    "when the guest writes here, signal this eventfd"
  KVM_IRQFD              x2     "when this eventfd is signalled, interrupt the guest"
```

(The descriptions on the right are added here; the script prints only the counts.)

These are the same calls labs 07 and 08 make by hand. There are more
`KVM_IOEVENTFD` calls than doorbells because the same call also *removes* a
registration. QEMU likely removes and re-adds them as the guest's driver resets
and starts the device during boot. strace shows the arguments only as pointers,
so it can't confirm that, and it can't show which interrupts go through the two
irqfds either. The guest's disk interrupts are PCI MSI-X messages, and QEMU can
deliver those through an irqfd or with a separate ioctl, depending on its
configuration.

## The same experiment over virtio-mmio

[observe_mmio.py](observe_mmio.py) runs the same A/B test with the disk on the
**virtio-mmio** transport, the one Firecracker uses, instead of virtio-pci.

```sh
./observe_mmio.py            # A/B comparison, 20 000 reads
./observe_mmio.py --strace   # count QEMU's KVM set-up calls
./observe_mmio.py --boot-log # also show each guest's console live, full kernel boot log included
```

With `--boot-log`, every guest console line appears prefixed with `guest |`. You can
watch the story from this section happen: the kernel boots without seeing any disk,
`Run /init as init process`, then `/init` loads the module. At that point
`virtio_blk virtio0: [vda] ... (64.0 MiB)` appears, followed by `START` and `DONE`.

It reuses everything in `observe.py` and changes three things:

| | observe.py (PCI) | observe_mmio.py (MMIO) |
| --- | --- | --- |
| machine | `-machine q35`: a modern PC with a PCI bus | `-machine microvm`: no PCI bus, almost no legacy PC hardware. QEMU's answer to Firecracker. |
| disk device | `-device virtio-blk-pci` | `-device virtio-blk-device`: virtio-blk with no transport of its own. microvm puts it into one of its virtio-mmio slots. |
| ioeventfd switch | a device option: `virtio-blk-pci,ioeventfd=on` | a transport option, set for every slot: `-global virtio-mmio.ioeventfd=on` (plus `force-legacy=off` for the modern virtio-mmio version 2) |
| guest program | [guest_init.c](guest_init.c) | [guest_init_virt_mmio.c](guest_init_virt_mmio.c): the same, plus loading a kernel module |

**Why the guest has to load a module.** Distribution kernels build the virtio-pci
driver in, but ship the virtio-mmio driver as a module (`virtio_mmio.ko.zst`),
because real PCs never need it. The initramfs has no udev or `modprobe` to load
it, so `observe_mmio.py` copies the module file into the archive, and
`guest_init_virt_mmio.c` loads it with the `finit_module()` system call, which is
what `modprobe` uses. The driver then finds the device through small ACPI tables
that microvm provides, and `/dev/vda` appears.

### Results (this machine, 20 000 reads)

```text
KVM vCPU counter during the workload       ioeventfd=off    ioeventfd=on
  exits                                           101237          101406
  mmio_exits                                       60000           40000
  io_exits                                            53              53
  halt_exits                                       20082           20170
  irq_exits                                           82              99
  guest-measured elapsed (ms)                      404.2           390.0
  us per 4K read                                   20.21           19.50
```

`--strace` (booting with ioeventfd on):

```text
  KVM_CREATE_IRQCHIP     x0
  KVM_SET_GSI_ROUTING    x114
  KVM_IOEVENTFD          x3
  KVM_IRQFD              x0
```

### How to read it, compared with PCI

* **ioeventfd still takes the kick: `mmio_exits` drops by exactly 20 000.** The
  kick is a write of the queue's number to the device's `QueueNotify` register.
  KVM matches it (address plus datamatch on the queue number, as in lab 07) and
  signals the eventfd.
* **But 2 MMIO exits per read remain (40 000).** On virtio-pci with MSI-X, the
  guest knows from the interrupt vector alone which queue has news. A
  virtio-mmio device has one interrupt line for everything. So the guest's
  interrupt handler (`vm_interrupt()` in the kernel's `drivers/virtio/virtio_mmio.c`)
  reads the device's `InterruptStatus` register to find out why, then writes
  `InterruptACK` to clear it. Both go to QEMU. The read needs an answer that only
  QEMU has, and an ioeventfd can only swallow writes. The ACK is a write, but it
  has to change the device's state in QEMU, so QEMU registers an ioeventfd only for
  `QueueNotify`. 20 000 interrupts × 2 = 40 000.
* **The interrupt itself isn't an irqfd here.** `KVM_IRQFD x0`: in this setup QEMU
  raises each interrupt with an ioctl. A separate strace run counted about 20 000
  `KVM_IRQ_LINE_STATUS` calls, one per read. Lab 08's irqfd isn't in play.
* **Overall it's the slower transport per request:** about 101 000 exits against
  41 000, and about 20 µs per read against 12–13 µs on PCI. virtio-mmio trades
  that for simplicity: no PCI bus to emulate, no PCI enumeration at boot, a
  smaller VMM. That's the right trade for Firecracker, which boots thousands of
  short-lived microVMs, and a reason full VMs use virtio-pci.
* **Fewer `KVM_IOEVENTFD` calls (3 vs 10):** there's no PCI device to reset and
  reprogram at boot, so fewer registrations are removed and re-added.

### A closer look: where every exit goes

The table shows only five of KVM's counters. KVM keeps many more, and
`query-stats` returns all of them. These are the ones that moved by more than
1 000 in another 20 000-read run:

| Counter (change during the reads) | ioeventfd=off | ioeventfd=on | Adds 1 each time... |
| --- | --- | --- | --- |
| `mmio_exits` | 60 000 | 40 000 | an MMIO access goes to QEMU |
| `insn_emulation` | 60 000 | 60 000 | KVM decodes a guest instruction to see what it did (here: every MMIO access) |
| `host_state_reload` | 80 116 | 60 144 | roughly: the vCPU thread leaves guest mode for longer, by returning to QEMU or sleeping in a halt |
| `halt_exits` | 20 077 | 20 194 | the guest runs `hlt` |
| `req_event` | 20 002 | 20 002 | KVM has an interrupt to deliver to the guest |
| `exits` | 101 256 | 101 583 | any exit |

**Every read touches the device's registers 3 times:** the kick (a write to
`QueueNotify`), then, in the interrupt handler, a read of `InterruptStatus` and a
write to `InterruptACK`.

**`insn_emulation` is 60 000 in both runs.** That's 3 per read, with ioeventfd on
too. Every one of those accesses stops the guest, and KVM decodes the instruction
to see what it was. That includes the kick. ioeventfd doesn't prevent that exit.
It only changes what happens after KVM has decoded the kick:

* **off:** KVM hands all 3 accesses to QEMU: `mmio_exits` = 60 000;
* **on:** KVM signals the eventfd for the kick itself, and passes on only the
  other 2: `mmio_exits` = 40 000.

**`host_state_reload` shows the saving directly.** It goes up roughly each time the
vCPU thread leaves guest mode for longer: returning to QEMU, or sleeping in a halt.

* off: 60 000 trips to QEMU + 20 000 halts ≈ 80 000
* on: 40 000 trips to QEMU + 20 000 halts ≈ 60 000

So ioeventfd saved exactly one trip out to QEMU and back per read, 20 000 in total.

**Why `exits` doesn't move (~101 000 in both runs).** Per read that's about 5 exits:

| Per read | Exits | Handled by |
| --- | --- | --- |
| the kick | 1 | QEMU (off) or KVM itself (on) |
| read `InterruptStatus` + write `InterruptACK` | 2 | QEMU |
| the guest halts, waiting for the disk | 1 | KVM |
| one more, about 21 000 per run | 1 | not visible in the counters |

The kick's exit is there either way, so the total can't change. `req_event` is
exactly 20 002, one interrupt delivered per read. So the fifth exit is probably
part of interrupt delivery, most likely the guest's "end of interrupt" write to
its interrupt controller. That's an inference: no counter names it. `perf kvm stat`
(needs root) breaks exits down by reason and would settle it.

**Why the time is the same (≈20 µs per read either way).** With one read at a time,
the guest spends each read waiting. ioeventfd saves one short trip to QEMU per
read, but adds a hand-off from KVM to QEMU's event loop, which costs about as
much. Each read costs about 8 µs more than on PCI, consistent with the 2 extra MMIO
round trips to QEMU per interrupt that MSI-X doesn't need.

**The vCPU hardly ever really sleeps.** `halt_successful_poll` (not in the table
above) came out at about 20 000: after each `hlt`, KVM spins for a short while
before putting the vCPU thread to sleep ("halt polling"), and the disk's interrupt
nearly always arrived during that spin. So each halt usually ended without the
thread ever going to sleep.

## Try next

These need root or extra set-up:

* **vhost:** `-netdev tap,id=n0,vhost=on -device virtio-net-pci,netdev=n0`. A kernel
  thread (`vhost-<qemu pid>` in `ps`) then waits on the ioeventfd and signals the
  irqfd itself, so QEMU is out of the data path entirely. Watch QEMU's CPU use
  under network load.
* **Per-address tracing:** `perf kvm stat record` / `perf kvm stat report`, or
  `trace-cmd record -e kvm:kvm_mmio -e kvm:kvm_ioeventfd`, to see which addresses
  exit and which ones an ioeventfd takes.
* **A dedicated I/O thread:** `-object iothread,id=io0 -device
  virtio-blk-pci,drive=d0,iothread=io0`. QEMU then waits on the ioeventfd in that
  thread instead of its main loop.
* **Firecracker** does the same with virtio-mmio: one ioeventfd per queue on the
  device's `QueueNotify` register (told apart by datamatch, lab 07) and one irqfd
  per device.

## The files

| File | What it is |
| --- | --- |
| [observe.py](observe.py) | The host side: builds the initramfs, starts QEMU twice, reads KVM's counters over QMP, prints the comparison. Also the `--strace` mode. |
| [guest_init.c](guest_init.c) | The guest's only program (`/init`): mounts `/proc` and `/dev`, then does the 20 000 disk reads and reports START/DONE on the serial console. |
| [observe_mmio.py](observe_mmio.py) | The virtio-mmio version: imports `observe.py` and swaps in a microvm command line and an initramfs that carries the `virtio_mmio` module. |
| [guest_init_virt_mmio.c](guest_init_virt_mmio.c) | `guest_init.c` plus loading the `virtio_mmio` module with `finit_module()`, passing it any `virtio_mmio.*` parameters from the kernel command line, as `modprobe` would. |
