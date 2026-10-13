# 01 — virtio: paravirtualised devices and the virtqueue

**Builds on:** nothing; a good place to start. The ring here is the same one [05-lockfree](../05-lockfree/) builds between threads.

## The idea

Emulating real hardware (an e1000 NIC, an IDE disk) makes the guest do dozens of
tiny register reads/writes per operation. Each one is a **VM exit**. virtio
replaces that with a device the guest *knows* is fake: a shared-memory protocol
where the guest queues work in RAM and rings a single doorbell.

```
   GUEST (driver)                                    HOST (device, in the VMM)
   ──────────────                                    ─────────────────────────
   fill descriptors  ─┐
   fill avail ring    ├─ plain stores to guest RAM ─▶  (VMM has the same RAM mapped)
   release-store idx ─┘
   MMIO write QueueNotify ═══ VM exit / ioeventfd ═══▶  wake device thread
                                                        acquire-load avail.idx
                                                        walk chains, do the I/O
                                                        write used ring, release-store idx
   ◀══════════ irqfd / interrupt injection ═════════    "used buffer notification"
   acquire-load used.idx, reap
```

## The split virtqueue (virtio 1.x §2.7)

| Structure        | Writer | Reader | Contents |
|------------------|--------|--------|----------|
| descriptor table | driver | device | `{addr, len, flags(NEXT/WRITE), next}` — pointers to buffers |
| available ring   | driver | device | `idx` + `ring[]` of chain heads: "ready for you" |
| used ring        | device | driver | `idx` + `ring[]` of `{head, bytes_written}`: "done" |

Every ring has **exactly one writer**, so no locks are needed; ordering comes
from acquire/release on the two `idx` fields. It is a single-producer /
single-consumer ring — the same structure as `05-lockfree/` and io_uring
(`04-io_uring/`).

A block request is a *chain* of three descriptors:

```
 desc[h]   header {type, sector}   device-READS
 desc[h+1] 512-byte data           device-READS (write) or device-WRITES (read)
 desc[h+2] 1-byte status           device-WRITES
```

## What `virtio_demo.c` does

It models the guest/host boundary with two threads (see the big comment at the
top of the file for the exact mapping table: eventfd ≙ ioeventfd/irqfd, mmap ≙
KVM memslot, offset ≙ GPA). It then runs, with logging:

1. **virtio-mmio handshake** — magic/version/device-id, status bits
   `ACKNOWLEDGE → DRIVER → FEATURES_OK → DRIVER_OK`, feature negotiation
   (`VIRTIO_F_VERSION_1`), and programming the three ring addresses.
2. **Batching** — 4 writes queued, *one* kick.
3. **Read-back** — 4 reads, *one* kick, data verified.
4. **A hostile guest** — a descriptor pointing outside guest RAM. The device
   answers `IOERR` instead of crashing.
5. **Statistics** — 9 requests took 3 kicks: the amortisation argument.

```
make run
```

Sample tail:

```
requests processed : 9
doorbell kicks     : 3  (each = 1 VM exit)
=> 0.33 exits per request
```

## Security lessons embedded in the code

* **Bounds-check every GPA** (`gpa_to_hva`). Note the overflow-safe comparison.
* **Bound the chain walk** so `next` loops cannot hang the VMM.
* **Copy each descriptor once** (double-fetch/TOCTOU): the guest can rewrite
  guest RAM while you read it.
* **Keep `last_avail_idx` in host memory** — never trust the guest with your
  cursor.

## Firecracker specifics

* Transport is **virtio-mmio**: a flat register page, no PCI bus emulation → much
  smaller attack surface than virtio-pci. (Firecracker's newer releases have added
  optional PCI support, so verify against the version you use.)
* Device set is deliberately tiny: `virtio-net`, `virtio-block`, `vsock`,
  `balloon`, `rng`(entropy).
* On the host, the kick is wired with **ioeventfd** and interrupts with **irqfd**,
  so the fast path avoids a round trip through the vCPU thread — exactly the
  eventfd pair used here.
* Firecracker's `vcpu` loop sees `VcpuExit::MmioWrite(addr, data)` and looks up
  which device owns `addr` on the MMIO bus — that is the `mmio_write()` switch above.
  See `02-hypercalls/` for a real KVM loop.

## Things deliberately not modelled

* `VIRTIO_F_EVENT_IDX` / `VRING_USED_F_NO_NOTIFY` (notification suppression:
  fewer exits/interrupts under load).
* Indirect descriptors, packed virtqueues (virtio 1.1), multiple queues.
* Descriptor free-list management in the driver (we recycle per batch).

## Try next

* **Notification suppression.** Add `VIRTIO_F_EVENT_IDX` (or
  `VRING_USED_F_NO_NOTIFY`) and count how many kicks and interrupts disappear
  under load.
* **Packed virtqueue (virtio 1.1).** Replace the three split-ring areas with one
  descriptor ring and see what changes in the driver and device code.
* **A real descriptor free list.** The driver recycles descriptors per batch;
  a proper free list would let requests complete out of order.
* **More than one queue.** Add a second queue and tell the kicks apart with
  datamatch, as in [07-ioeventfd](../07-ioeventfd/).

## In short

virtio turns N trapping register accesses into one doorbell: the guest fills
lock-free rings in shared memory, the host drains them, and the only exits are
the kick and the completion interrupt — and both can be bypassed into the kernel
with ioeventfd/irqfd.
