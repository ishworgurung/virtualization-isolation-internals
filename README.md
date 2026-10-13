# Linux virtualisation & isolation internals — runnable notes

A self-directed study of the kernel mechanisms behind fast, safe microVM
sandboxes (Firecracker style): one mechanism per directory, each with a runnable
experiment. Every directory has a `README.md` (the explanation, real output from
this machine, and a "Try next" list of threads still open) and heavily commented
code you can run.

| Dir | Topic | Language | Run | Needs |
|---|---|---|---|---|
| [01-virtio](01-virtio/) | Split virtqueue + virtio-mmio block device, guest driver ↔ host device | C | `make run` | nothing |
| [02-hypercalls](02-hypercalls/) | A real KVM VMM: port I/O, MMIO, hypercalls, exit-cost benchmark | C | `make run` | `/dev/kvm` access |
| [03-uffd](03-uffd/) | userfaultfd: lazy snapshot restore vs eager vs mmap; write-protect dirty tracking | C | `make run` | kernel ≥ 5.11 |
| [04-io_uring](04-io_uring/) | Raw io_uring (no liburing), SQPOLL, CQ busy-polling, seccomp bypass + mitigations | C | `make run` | io_uring enabled |
| [05-lockfree](05-lockfree/) | SPSC ring, ABA replay, false sharing, memory-ordering litmus | Rust | `cargo test --release` | nothing |
| [06-cgroups](06-cgroups/) | cgroup v2 limits, PSI, weights, no-internal-process rule, jailer-style confinement | Rust | `cargo run --release -- all` | delegated cgroup v2 subtree |
| [07-ioeventfd](07-ioeventfd/) | `KVM_IOEVENTFD`: guest doorbells handled in-kernel, datamatch per queue | C | `make run` | `/dev/kvm` access |
| [08-irqfd](08-irqfd/) | `KVM_IRQFD`: guest interrupts from `write(eventfd)`, coalescing, vs `KVM_IRQ_LINE` | C | `make run` | `/dev/kvm` access |
| [09-kick-roundtrip](09-kick-roundtrip/) | ioeventfd + irqfd datapath vs classic exit-and-inject: throughput and latency | C | `make run` | `/dev/kvm` access |
| [10-qemu-observe](10-qemu-observe/) | Real QEMU guest: `ioeventfd=on/off` A/B via KVM exit counters; strace of registrations | Python + C | `./observe.py` | qemu, `/dev/kvm` |
| [11-scm-rights](11-scm-rights/) | Passing an open fd (an eventfd) to another process over a Unix socket, as vhost-user does | C | `make run` | nothing |

**Suggested path.** 01, 02 and 05 stand alone and make good starting points.
07 → 08 → 09 build on 02's VMM and form one thread; 10 checks that thread against
real QEMU, and 11 answers a question 07 leaves open. 03, 04 and 06 can be taken
in any order. Each lab's README says at the top what it builds on.

Tuning the host so these benchmarks are fast and reproducible (microcode, core
pinning and isolation, frequency/C-states, SMT, mitigations, memory, waiting
strategies): [Optimizing a Linux system for ultra performance](optimizing-linux-for-ultra-performance.md).

## Who is who: KVM, the VMM, QEMU, Firecracker

| Piece | Where it runs | Job |
|---|---|---|
| **KVM** | kernel module | Runs the guest on the CPU (VT-x/AMD-V). Handles exits it can resolve itself: CPUID, EPT faults on RAM, in-kernel PIC/APIC, ioeventfd/irqfd routing, kernel-handled hypercalls. |
| **VMM** (virtual machine monitor) | userspace process | Creates the VM (`KVM_CREATE_VM`), supplies guest memory, creates vCPU threads that loop on `KVM_RUN`, and **emulates devices** whenever an exit needs userspace (`KVM_EXIT_MMIO`/`IO`). |
| **QEMU** | userspace | A full-featured VMM: many machine types and devices. Can also run without KVM (`-accel tcg`, pure software CPU emulation); with `accel=kvm` it is only the VMM and the CPU work goes to KVM. |
| **Firecracker / crosvm / cloud-hypervisor** | userspace | Minimal VMMs. Firecracker emulates only virtio-net, virtio-block, vsock, balloon, entropy and a serial console: less code, smaller attack surface. |

So "run a VM with QEMU" means QEMU is the VMM and KVM is the hypervisor engine underneath.
`02-hypercalls/kvm_exits.c` is a ~300-line VMM; `10-qemu-observe` shows the same
structure inside a real QEMU (its process holds the `kvm-vm` and `kvm-vcpu:0` fds and
issues `KVM_IOEVENTFD`/`KVM_IRQFD`). "Hypervisor" is used loosely: sometimes KVM
alone, sometimes KVM + VMM together.

With **vhost**, the datapath leaves the VMM too: a kernel worker waits on the
ioeventfd and signals the irqfd, so QEMU stays only on the control path.

## The thread connecting them

* **virtio ⇄ io_uring ⇄ lock-free** are the *same idea*: single-producer /
  single-consumer rings in shared memory, published with release/acquire. The
  boundary differs (guest↔host, user↔kernel, thread↔thread).
* **virtio ⇄ hypercalls**: the whole point of virtio is to turn many expensive
  VM exits into one doorbell; `02` measures what an exit costs.
* **uffd ⇄ snapshots**: the VMM owns page-fault handling, so a microVM restores in
  microseconds and pages in only what it touches.
* **io_uring ⇄ seccomp**: the same performance trick (ops as data, not syscalls)
  is what makes it hard to sandbox — a performance/security trade-off to reason about.
* **cgroups ⇄ Firecracker jailer**: confinement applied before the first instruction.

## Machine these results were captured on

Linux 7.1 (Manjaro), Intel VT-x, 24 logical CPUs, cgroup v2 with systemd delegation.
Absolute numbers will differ on your hardware; ratios and behaviours are the point.
Where a demo cannot prove something on this hardware (e.g. memory-ordering bugs on
x86's strong memory model) the README says so.

## Cleaning build artefacts

```
for d in 01-virtio 02-hypercalls 03-uffd 04-io_uring 07-ioeventfd 08-irqfd 09-kick-roundtrip; do make -C $d clean; done
(cd 05-lockfree && cargo clean); (cd 06-cgroups && cargo clean)
```
