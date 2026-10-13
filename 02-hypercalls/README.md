# 02 — Hypercalls and VM exits

**Builds on:** nothing. This is the repo's first real KVM VMM; [07-ioeventfd](../07-ioeventfd/), [08-irqfd](../08-irqfd/) and [09-kick-roundtrip](../09-kick-roundtrip/) reuse its setup.

## Hypercall = the guest's syscall into the hypervisor

| | syscall | hypercall |
|---|---|---|
| who → whom | user → kernel | guest kernel → hypervisor |
| instruction | `syscall` / `svc` | `VMCALL` (Intel), `VMMCALL` (AMD) |
| effect | ring 3 → ring 0 | **VM exit** to KVM |
| ABI (KVM x86) | — | `RAX`=number, `RBX,RCX,RDX,RSI`=args, result in `RAX` |

Examples in KVM: `KVM_HC_KICK_CPU` (paravirt spinlocks), `KVM_HC_SEND_IPI`,
`KVM_HC_SCHED_YIELD`, `KVM_HC_CLOCK_PAIRING`, `KVM_HC_MAP_GPA_RANGE`
(confidential VMs: "this range is now shared/private").

## But the real story is VM exits

A hypercall is one of many things that cause an exit:

| Cause | Handled by | Cost class |
|---|---|---|
| `CPUID` (always exits on VMX) | KVM in-kernel | light |
| `VMCALL` to a kernel-handled hypercall | KVM in-kernel | light |
| MSR read/write (many) | KVM in-kernel | light |
| EPT violation on RAM (first touch / swapped) | KVM in-kernel | light–medium |
| External interrupt / timer | KVM in-kernel | light |
| `HLT` (with in-kernel irqchip) | KVM in-kernel | light |
| Port I/O (`IN`/`OUT`) to an emulated device | **userspace VMM** | **heavy** |
| MMIO to a device region (`KVM_EXIT_MMIO`) | **userspace VMM** | **heavy** |
| Hypercall routed to userspace (`KVM_CAP_EXIT_HYPERCALL`) | **userspace VMM** | **heavy** |

*Light* = the CPU exits to KVM and comes straight back. *Heavy* = the exit
surfaces from `ioctl(KVM_RUN)` into the VMM, which emulates the device and calls
`KVM_RUN` again — two extra kernel/user transitions plus VMM work.

This is why the optimisation playbook is "**eliminate exits**":

* **virtio** — 1 doorbell per batch instead of N register pokes (`01-virtio/`)
* **ioeventfd / irqfd / vhost** — make even the doorbell a *light* exit (kernel or
  another process handles it; the vCPU thread never returns to userspace)
* **PV clock, PV TLB flush, PV EOI, PV spinlocks** — the guest cooperates so the
  hypervisor isn't trapped on every clock read / IPI / lock
* **posted interrupts / APICv / AVIC** — hardware delivers interrupts without exits
* **huge pages** — fewer EPT violations

## `kvm_exits.c`

(Terminology: this program is a **VMM**, the same role QEMU or Firecracker plays; KVM is the kernel side. See the "Who is who" table in the top-level README.)

A minimal VMM that boots a hand-assembled 16-bit real-mode guest (no kernel
needed) and provokes each exit type in sequence:

1. **Port I/O** — the guest `OUT`s a string to port `0x3f8`; the VMM prints it
   (`KVM_EXIT_IO`).
2. **MMIO** — the guest reads/writes a GPA with no memslot behind it; the VMM
   emulates a device register (`KVM_EXIT_MMIO`).
3. **Hypercall, kernel-handled** — unknown number → KVM returns `-KVM_ENOSYS`
   (`0xfffffc18`) in `RAX` without ever waking the VMM.
4. **Hypercall, userspace-handled** — `KVM_HC_MAP_GPA_RANGE` is routed to us
   with `KVM_CAP_EXIT_HYPERCALL` (`KVM_EXIT_HYPERCALL`); we set `run->hypercall.ret`.
5. **Benchmark** — 100 000 × each of `OUT`, `CPUID`, `VMCALL`, timed by the VMM.
6. **HLT** — ends the run.

```
make run          # needs access to /dev/kvm (group `kvm`)
```

### Result on the machine this was written on (Intel, kernel 7.1)

```
bench OUT  (heavy exit: returns to userspace)  100000 iters  1243 ns/iter
bench CPUID (light exit: handled in kernel)    100000 iters   448 ns/iter
bench VMCALL(light exit: handled in kernel)    100000 iters   445 ns/iter

Exits that reached USERSPACE: IO 100054, HYPERCALL 1, HLT 1, MMIO 2
```

Reading it: the raw hardware exit+entry costs on the order of 0.4 µs here; going
up to userspace roughly triples it. Absolute numbers depend on CPU and
mitigations; the ratio is the lesson. The 200 000 in-kernel exits never show up
in the userspace count — KVM swallowed them. You can see them from outside with
`perf kvm stat record` / `perf stat -e kvm:kvm_exit` (needs root).

## How this maps onto Firecracker

Firecracker's vCPU thread is this loop. `VcpuExit::IoIn/IoOut` → the PIO bus
(serial console, i8042 reset), `VcpuExit::MmioRead/MmioWrite` → the MMIO bus
(virtio-mmio devices), `VcpuExit::Hlt`/`Shutdown` → stop the VM. The KVM
`kvm_run` page, `KVM_RUN`, memslots and `KVM_SET_CPUID2` are exactly the ioctls
above (via the `kvm-ioctls` crate).

## Notes / gotchas

* Real mode is used only to avoid writing GDT/page-table setup; a 64-bit guest
  triggers the same exits. `VMCALL` worked in real mode here, but if yours
  raises `#UD`, move the guest to protected/long mode.
* `KVM_SET_CPUID2` passes KVM's paravirt CPUID leaves to the guest. KVM only
  *enforces* those PV feature bits (refusing, say, `KVM_HC_MAP_GPA_RANGE` unless
  the guest was told about it) when the VMM opts in with
  `KVM_CAP_ENFORCE_PV_FEATURE_CPUID`; this demo doesn't, so the call is good
  practice rather than a requirement.
* On AMD the demo patches the three `VMCALL` sites to `VMMCALL`.

## Try next

* **A 64-bit guest.** Set up a GDT and page tables, move the guest to long
  mode, and check that the exits and their costs stay the same.
* **Enforced PV CPUID.** Opt in to `KVM_CAP_ENFORCE_PV_FEATURE_CPUID`, hide the
  feature bit, and watch KVM refuse `KVM_HC_MAP_GPA_RANGE`.
* **KVM's own view.** Run `perf kvm stat record` around the benchmark and
  compare KVM's exit counts with the program's.
* **Other hardware.** Run it on an AMD machine (the `VMMCALL` path) and compare
  exit costs.

## In short

Hypercalls are the guest's syscalls into the hypervisor; the real performance
story is minimising VM exits — and especially exits that have to bounce up to a
userspace VMM, which is why virtio, ioeventfd and vhost exist.
