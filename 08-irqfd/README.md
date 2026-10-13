# 08 — irqfd: host → guest interrupts from a plain `write()`

**Builds on:** [02-hypercalls](../02-hypercalls/) and [07-ioeventfd](../07-ioeventfd/), which covers the other direction.

## The problem

A device thread has finished a request and must interrupt the guest. The classic
call is `ioctl(vm_fd, KVM_IRQ_LINE, …)`, which needs the **VM file descriptor** —
so only the VMM process can do it. A kernel backend like vhost-net has no such fd,
so it would have to wake the VMM just to inject an interrupt.

## The fix

### The idea in one sentence

Install a **doorbell** that is wired straight to the guest's interrupt, then hand
the doorbell's button to whoever needs it. Pressing the button makes KVM interrupt
the guest itself. The VMM (the program that owns the VM, e.g. QEMU) doesn't have to
be asked, or even be awake.

An office analogy: a courier (the device) has a parcel for an employee (the
guest). **Before**, the courier had to find the office manager (the VMM), and the
manager walked over and tapped the employee on the shoulder. **After**, there's a
buzzer on the employee's desk and the courier holds the button. The manager is no
longer involved.

### The pieces, in plain terms

* **Interrupt.** A signal that makes a CPU drop what it's doing and run a
  specific piece of code (the "interrupt handler"), then carry on. Devices use
  interrupts to say "I've finished something, come and look."
* **Interrupt line (IRQ), and GSI.** Interrupts arrive on numbered input lines,
  like numbered wires into a switchboard. KVM calls its line numbers **GSIs**
  ("global system interrupts"). In this lab, the guest's device uses line 5.
* **eventfd.** The doorbell. It's the simplest kind of file descriptor Linux
  has: it holds a counter. `write()` to it adds to the counter, and anything
  waiting on it is woken up. Because it's a file descriptor, it can be handed to
  another thread, sent to another process, or given to code inside the kernel.
  That's how the button gets "handed out".
* **`KVM_IRQFD`.** The wiring. A one-time request to KVM: "watch this eventfd,
  and every time it's rung, raise interrupt line N in this VM." ("irqfd" is short
  for "IRQ file descriptor".)

### Setting it up (once, in the VMM)

```c
int efd = eventfd(0, 0);                         /* 1. make the doorbell           */
struct kvm_irqfd ifd = { .fd = efd, .gsi = 5 };  /* 2. "this doorbell -> line 5"   */
ioctl(vm_fd, KVM_IRQFD, &ifd);                   /* 3. hand that wiring to KVM     */
/* 4. give efd to whoever produces the events: a worker thread, another
 *    process, or a kernel driver such as vhost-net. */
```

Only step 3 needs the VM's file descriptor, and it happens once, at setup time.
After that, the VM fd is never needed again to deliver interrupts.

### Delivering an interrupt (every time)

```c
uint64_t one = 1;
write(efd, &one, sizeof one);   /* ring the doorbell */
```

That's all. Kernel code can do the same without even a syscall, by calling
`eventfd_signal()`, which is what vhost-net does.

### What happens after the write, step by step

1. **The device rings the doorbell:** it writes to the eventfd.
2. **Linux wakes everything waiting on that eventfd.** During `KVM_IRQFD`, KVM
   signed itself up as one of the waiters, so KVM's code now runs, inside the
   kernel.
3. **KVM raises line 5** on the guest's virtual interrupt controller. (For the
   old PC-style controller used in this lab, KVM hands this step to a kernel
   worker that runs a moment later.)
4. **The interrupt controller delivers the interrupt to the guest's virtual
   CPU.** If that CPU was asleep waiting for work, this wakes it.
5. **The guest runs its interrupt handler.**

The VMM process appears nowhere in that list. It isn't woken up, doesn't run any
code, and the virtual CPU never has to stop and return to it.

### Before and after

```
Before (KVM_IRQ_LINE):
  device ──"please interrupt the guest"──▶ VMM process ──ioctl(vm_fd, KVM_IRQ_LINE)──▶ KVM ──▶ guest
                                           (must be woken up and scheduled first)

After (irqfd):
  device ──write(eventfd)──▶ KVM ──▶ guest
```

This matters most for **vhost-net**, the kernel's fast path for virtual network
cards. vhost-net runs *inside the kernel* and has no VM file descriptor, so it
could never call `KVM_IRQ_LINE` itself. With irqfd, a packet can arrive, be put in
the guest's memory, and trigger the guest's interrupt without ever leaving the
kernel or touching QEMU.

### What irqfd does *not* do

* **It isn't faster for a normal program.** A userspace thread calling
  `write()` pays for one syscall, much like `KVM_IRQ_LINE` does. The results
  below show the two with the same median latency. The benefit is **who can
  deliver the interrupt**, not how quickly.
* **It doesn't count rings.** Ring the doorbell 10 times quickly and the guest
  may see only one interrupt. The results below show this (10 000 rings, about
  3 000 interrupts). So an interrupt must mean "go and check what's new", never
  "exactly one new thing".

### One requirement

KVM must own the guest's interrupt controller, so the VMM creates it inside the
kernel with `KVM_CREATE_IRQCHIP`, before creating any virtual CPUs. irqfd needs
something inside the kernel to raise line 5 on. If the VMM emulated the
interrupt controller itself in userspace, every interrupt would have to go
through the VMM again, which is exactly what irqfd is meant to avoid.

(Modern devices usually signal with **MSI**, "message-signalled interrupts",
instead of numbered wires. The same mechanism covers them: QEMU gives each MSI
its own GSI number with `KVM_SET_GSI_ROUTING`, then attaches one irqfd to each.)

## Run it

```
make run
```

The guest is a small freestanding C program, [guest.c](guest.c). The host starts
the vCPU directly in 32-bit protected mode with flat segments, so `gcc -m32` output
runs as-is. The guest:

1. prints a hello message on the serial port (COM1, 0x3f8),
2. fills in an IDT entry for its handler,
3. programs the 8259 PIC (vector base 0x20, unmask IRQ5),
4. sleeps in `sti; hlt`.

Its handler (`__attribute__((interrupt))`, so plain C) bumps a counter in guest RAM
and issues EOI. Everything the guest does with the PIC and `hlt` is emulated
in-kernel too, so **apart from the hello message (one exit per character), the only
userspace exit in the entire run is the guest's final "done" port write**.

How the guest gets into the VM:

```
guest.c --gcc -m32, guest.ld--> guest.elf --objcopy -O binary--> guest.bin --#embed--> irqfd_demo
```

[shared.h](shared.h) is the contract both sides include: the memory map, port
numbers, and `struct shared`, the mailbox in guest RAM. Assembly is limited to
one-instruction inline helpers (`outb`, `cli`, `sti; hlt`, `lidt`), which have no C
equivalent. `make disasm` shows what the C compiled to.

Output on this machine:

```
hello from the guest! (via OUT to port 0x3f8)
guest is up: PIC programmed, handler installed, sleeping in HLT

A. PACED delivery latency, host write() -> guest handler has run
   irqfd: write(eventfd)              n=20000  min   1.2 us   p50   1.5 us   p99    1.8 us

C. same, via the classic ioctl(KVM_IRQ_LINE) path
   KVM_IRQ_LINE: 2 ioctls             n=20000  min   0.7 us   p50   1.5 us   p99    1.6 us

B. BURST: 10000 back-to-back write(irqfd) with no pacing
   host signalled 10000 times, guest handler ran 3070 times (69.3% coalesced)

Totals: guest took 43071 interrupts in this run.
Serial port: 46 exit(s), one per character of the hello message (not counted below).
KVM_RUN returned to userspace 1 time(s)
```

### What to take from this

* **Latency is the same as `KVM_IRQ_LINE`** (~1.5 µs median, mostly the vCPU
  waking from HLT and running the handler). irqfd is *not* a latency
  optimisation. Its value is **who can call it**: any thread holding the eventfd,
  including the kernel's vhost workers, with no VMM in the path.
* **Interrupts coalesce.** 10 000 signals produced 3 070 handler runs. Edge-triggered
  lines have one pending bit; a signal that arrives while one is pending merges.
  Therefore an interrupt must mean *"go look at the used ring"*, never *"exactly one
  event"* — a driver that increments per-interrupt loses completions. This is
  why virtio drivers loop until the ring is drained.
* **No userspace exit ≠ no exit.** Each injection costs the guest a real
  interrupt entry/EOI; posted interrupts (APICv/AVIC) reduce those further.

### Why is `KVM_IRQ_LINE`'s *minimum* lower?

A typical run:

```text
irqfd: write(eventfd)     min 1.3 us   p50 1.7 us   p99 1.9 us
KVM_IRQ_LINE: 2 ioctls    min 0.8 us   p50 1.6 us   p99 1.9 us
```

The typical cost (p50, p99) is the same. Only the **best case** differs, by about
half a microsecond. The reason is *where* the interrupt gets raised.

**`KVM_IRQ_LINE` does everything in the calling thread.** The ioctl raises line 5 on
the emulated interrupt controller right away, then wakes the vCPU. Nothing else has
to be scheduled in between.

**irqfd takes one extra step for this lab's kind of interrupt line:**

1. `write(eventfd)` runs KVM's callback immediately, inside the `write`.
2. That callback runs at a point where the kernel is not allowed to wait for
   anything. Raising a line on the old PC-style interrupt controllers (PIC/IOAPIC)
   can wait on locks, so KVM doesn't do it there. It queues the job for a kernel
   worker thread (a "kworker") instead.
3. The kworker has to be woken and scheduled. Only then does it raise the line
   and wake the vCPU.

That extra thread wake-up is the ~0.5 µs. And it's a bit worse than just "one more
wake-up": Linux queues the job on the worker for the **same CPU** that called
`write()`. In this demo, that CPU is busy running our main thread, which is spinning
while it waits for the guest's counter to move. So the kworker has to **preempt our
own thread** (take the CPU away from it) before it can raise the line. The
measurements below show exactly one such switch per irqfd write.

**Why it only shows in `min`:** most of each sample is spent waking the vCPU out of
HLT. When the vCPU has really gone to sleep, that wake-up is larger than the
difference between the two paths, so both land at the same p50. The minimums come
from the lucky samples where the vCPU was still *halt polling* (KVM spins a halted
vCPU briefly before putting it to sleep). Waking it is then almost free, and what's
left is the injection path itself. That's where irqfd's extra hop becomes visible.

**It doesn't carry over to real devices.** virtio and vhost devices signal with
**MSI** interrupts, not PIC/IOAPIC lines. For MSI, KVM can deliver the interrupt
right inside the callback, with no kworker. So in real deployments irqfd doesn't
pay this cost, and its benefit is still *who* can ring it, not speed.

#### Measured: one kworker job per irqfd write

The demo writes to the irqfd **30 001 times**: 20 000 in experiment A, 10 000 in
experiment B's burst, and 1 more to wake the guest at the end. The
`KVM_IRQ_LINE` experiment (C) makes 20 000 more interrupts, but through ioctls, not
the eventfd.

Counting kernel events for the whole run (needs root, because the kernel's trace
events are root-only by default):

```sh
sudo perf stat -e workqueue:workqueue_queue_work -e sched:sched_wakeup ./irqfd_demo
```

```text
            30,021      workqueue:workqueue_queue_work
            30,006      sched:sched_wakeup
```

and, as an ordinary user:

```sh
perf stat -e context-switches,cpu-migrations ./irqfd_demo
```

```text
            30,027      context-switches
                10      cpu-migrations
```

How to read these:

* **`workqueue_queue_work` = 30 021:** a job was handed to a kernel worker (a
  kworker) once per irqfd write, 30 001 times. The last ~20 are ordinary
  background kernel activity. The 20 000 `KVM_IRQ_LINE` interrupts queued **no**
  work: they were raised directly, as described above.
* **`sched_wakeup` = 30 006:** each of those jobs meant waking a kworker. That's
  the extra hop irqfd pays for this lab's PIC interrupt line.
* **`context-switches` = 30 027:** each time, our own process was switched out
  for the kworker, because the job was queued on the CPU our main thread was
  spinning on. Again, `KVM_IRQ_LINE` adds essentially none.

One more detail from the burst (experiment B): all 10 000 writes queued their own
job, yet the guest's handler ran only about 8 200 times. So in this run, the writes
weren't merged at the kworker step. They were merged later, at the interrupt
controller, which keeps just one "pending" flag per line. (Whether writes also merge
at the kworker step depends on timing: a write that arrives while the previous job
is still waiting is absorbed into it.)

(The explanation of *why* irqfd defers the work comes from KVM's source,
`virt/kvm/eventfd.c` and `arch/x86/kvm/irq_comm.c`. The counts above confirm the
behaviour it predicts.)

## API notes

| Item | Meaning |
|---|---|
| `KVM_IRQFD_FLAG_DEASSIGN` | unregister |
| `KVM_IRQFD_FLAG_RESAMPLE` + `resamplefd` | **level-triggered** support: KVM signals `resamplefd` on guest EOI so the device can re-assert if still pending (used by VFIO INTx) |
| GSI | a routing-table index (`KVM_SET_GSI_ROUTING`); default routing maps GSI 0–15 to PIC pins and IOAPIC pins 1:1 |

## Gotchas hit while writing this

* The guest's timestamp markers must not use ports `0x20/0x21` (in `09-…`): those are the PIC's ports and are claimed in-kernel, so an `OUT` there never reaches userspace.
* Use `cli; test; sti; hlt` when sleeping for a flag: `sti` delays interrupts one
  instruction, closing the window where an interrupt lands *between* the test and
  the `hlt` and the vCPU sleeps forever (lost wake-up).

## Try next

* **MSI routing.** Set up an MSI entry with `KVM_SET_GSI_ROUTING` and attach an
  irqfd to it, as QEMU does for modern devices.
* **Level-triggered interrupts.** Use `KVM_IRQFD_FLAG_RESAMPLE` with a
  `resamplefd` and re-assert the line from the device until the guest clears it.
* **Posted interrupts.** Move the guest from the PIC to the local APIC, check
  whether APICv is on (`/sys/module/kvm_intel/parameters/enable_apicv`), and
  compare delivery latency with it on and off.

## In short

irqfd lets any thread — or a kernel backend like vhost — inject a guest interrupt
with a `write()` to an eventfd, no VM fd and no VMM round trip. It's edge-triggered
and coalesces, so the interrupt only means 'check the ring'.
