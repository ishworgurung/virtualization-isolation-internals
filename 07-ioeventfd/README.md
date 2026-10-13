# 07 — ioeventfd: guest → host doorbells that skip the VMM

**Builds on:** [02-hypercalls](../02-hypercalls/) (the KVM run loop and VM exits) and [01-virtio](../01-virtio/) (what a doorbell kick is).

## The problem

A guest "kicks" a device by storing to a device register. That address has no RAM
behind it, so the CPU takes an EPT fault and **exits to KVM**. KVM cannot
resolve it, so by default `ioctl(KVM_RUN)` *returns to the userspace VMM* with
`KVM_EXIT_MMIO`; the VMM's vCPU thread decodes the access, emulates the device,
and re-enters the guest (see `02-hypercalls/`).

## The fix

```c
struct kvm_ioeventfd io = { .addr = 0xd0000, .len = 4, .fd = efd };   // + optional datamatch
ioctl(vm_fd, KVM_IOEVENTFD, &io);
```

"When the guest writes `len` bytes to `addr`, add 1 to `efd` and resume the guest."

```
default:      guest store ─▶ VM exit ─▶ KVM ─▶ return from KVM_RUN ─▶ VMM vCPU thread emulates ─▶ KVM_RUN
ioeventfd:    guest store ─▶ VM exit ─▶ KVM ─▶ eventfd += 1 ─▶ re-enter guest
                                                    └─▶ (another thread / vhost wakes on efd)
```

The **hardware exit still happens**; what disappears is the *return to userspace*
and the vCPU thread doing device work. Whoever waits on the fd — a device thread,
an iothread, or an in-kernel backend such as vhost-net — handles the request on
another CPU.

## Run it

```
make run            # needs /dev/kvm; boots a 16-bit guest, no kernel or disk image
```

One guest program, three experiments (result from this machine):

```
EXPERIMENT 1: 200000 guest writes to A (ioeventfd registered)
   eventfd counter after the run   : 200000
   KVM_EXIT_MMIO seen by userspace : 0
   guest time per write            : 825 ns

EXPERIMENT 2: 200000 guest writes to B (NO ioeventfd)
   KVM_EXIT_MMIO seen by userspace : 200000
   guest time per write            : 1485 ns
   => ioeventfd is 1.80x faster per kick for the vCPU

EXPERIMENT 3: datamatch on one address (guest wrote 0,1,2,3 to C)
   eventfd(value==1) counter : 1
   eventfd(value==2) counter : 1
   fell through to userspace as KVM_EXIT_MMIO, values: 0 3
```

1. **Zero userspace exits for 200 000 kicks**, yet the eventfd counted every one
   (`eventfd` is a counter, so nothing is lost even with no reader running).
2. **Same guest loop without an ioeventfd** returns to userspace on every write,
   and each write costs ~1.8× more vCPU time.
3. **Datamatch**: several ioeventfds on the *same address*, each matching a
   different written *value*. Unmatched values fall back to `KVM_EXIT_MMIO`. This
   is precisely how virtio distinguishes queues: virtio-mmio's `QueueNotify`
   register receives the **queue index**, so QEMU/Firecracker register one
   datamatch ioeventfd per virtqueue (virtio-pci uses one address per queue).

## API notes

| Field | Meaning |
|---|---|
| `addr`, `len` | GPA (MMIO) or port (PIO) and access width to match. Width must equal the guest's store width or it won't match. |
| `KVM_IOEVENTFD_FLAG_PIO` | interpret `addr` as an I/O port instead of MMIO |
| `KVM_IOEVENTFD_FLAG_DATAMATCH` | only fire when the written value == `datamatch` |
| `KVM_IOEVENTFD_FLAG_DEASSIGN` | unregister |
| `KVM_IOEVENTFD_FLAG_VIRTIO_CCW_NOTIFY` | s390 only |

* Works on addresses **not** covered by a memslot (the normal MMIO case). It
  cannot attach to guest RAM.
* The eventfd is a plain 64-bit counter. Several kicks before the reader runs
  are merged into one wake-up with a count > 1 — the device must drain the whole
  virtqueue per wake-up, not assume one request per kick.
* Combine with virtqueue notification suppression (`VRING_USED_F_NO_NOTIFY`,
  `EVENT_IDX`): the device tells the guest "don't kick me while I'm already
  polling", removing even the light exits.

## How does the kernel tell two eventfds apart?

Suppose two processes each call `eventfd(0, 0)` and both get back fd `3`. How does
a `write(3, ...)` in one process not bump the other's counter?

The answer: the fd number is not the eventfd. Each `eventfd()` call makes a
brand-new object, and the number is only the calling process's private handle
to it. Step by step, `eventfd(0, 0)`:

1. allocates a new `struct eventfd_ctx`: the eventfd itself, holding a 64-bit
   `count` and a wait queue of threads sleeping in `read()`;
2. wraps it in a new `struct file` (via `anon_inode_getfile("[eventfd]", ...)`)
   whose `private_data` points at that ctx;
3. puts the file into the **calling process's** fd table at the lowest free slot,
   and returns that slot number.

```text
Process A                                    Process B
fd table                                     fd table
┌───┬────────┐                               ┌───┬────────┐
│ 3 │ ───────┼──▶ struct file ──▶ ctx #1     │ 3 │ ───────┼──▶ struct file ──▶ ctx #2
└───┴────────┘    (count, waitq)             └───┴────────┘    (count, waitq)
```

Every syscall looks the fd up in the caller's own table (`current->files`), so A's
`3` and B's `3` reach different contexts. A `write()` in A adds to ctx #1 and wakes
only ctx #1's sleepers. It's the same reason two processes can both have a file open
as fd 3 without mixing them up.

You can see this yourself. All eventfds share one anonymous inode, so the inode
number doesn't tell them apart. The per-context `eventfd-id` does:

```sh
# make two eventfds, then print each one's link and fdinfo
python3 -c '
import os
for fd in (os.eventfd(0), os.eventfd(0)):
    print(fd, os.readlink(f"/proc/self/fd/{fd}"))
    print(open(f"/proc/self/fdinfo/{fd}").read())'
```

Trimmed output from this machine:

```text
3 anon_inode:[eventfd]     ino: 3136   eventfd-id: 963
4 anon_inode:[eventfd]     ino: 3136   eventfd-id: 964
```

(`ls -l /proc/<pid>/fd` and `cat /proc/<pid>/fdinfo/<fd>` show the same for any
process.)

Two processes end up sharing **one** eventfd only if the file itself is handed
over:

* `fork()`: the child's fd table points at the parent's `struct file`s;
* sending the fd over a Unix socket (`SCM_RIGHTS`): the receiver gets a new
  number for the same file. This is how a VMM hands eventfds to vhost-user
  backends. [11-scm-rights](../11-scm-rights/) demonstrates it;
* `pidfd_getfd()`: copies an fd out of another process.

**What this means for KVM.** When this lab calls `ioctl(vm_fd, KVM_IOEVENTFD, &io)`,
KVM immediately turns `io.fd` into a context pointer. `eventfd_ctx_fdget(fd)` looks
the number up in the calling process's fd table and takes a reference. From then on
KVM keeps the `struct eventfd_ctx *` and never looks at the number again. A guest
write to the doorbell calls `eventfd_signal(ctx)` on exactly that context. So the fd
number only matters at registration time, in the process that registered it. The
same is true of `KVM_IRQFD` (lab 08) and of vhost.

## See it in real QEMU

`../10-qemu-observe/` boots a real guest kernel under QEMU and compares
`ioeventfd=on/off` using KVM's own exit counters.

## Try next

* **Suppress the kicks too.** Combine this with `EVENT_IDX` from
  [01-virtio](../01-virtio/) and count how many light exits remain under load.
* **PIO vs MMIO doorbells.** Register the same doorbell with
  `KVM_IOEVENTFD_FLAG_PIO` and compare the two exit paths.
* **Width mismatch.** Register `len = 4`, have the guest store 2 bytes, and
  confirm the write falls back to `KVM_EXIT_MMIO`.
* **`pidfd_getfd()`.** Pull the eventfd out of another process, the third way
  to share one besides `fork()` and [11-scm-rights](../11-scm-rights/).

## In short

ioeventfd lets KVM turn a guest's MMIO/PIO doorbell write into an eventfd signal
inside the kernel — the exit still happens, but KVM_RUN doesn't return to the VMM,
so device work moves off the vCPU thread (or into vhost). Datamatch on the written
value is how one doorbell register serves many virtqueues.
