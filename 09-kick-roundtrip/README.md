# 09 — The whole loop: kick → device → interrupt

**Builds on:** [07-ioeventfd](../07-ioeventfd/) and [08-irqfd](../08-irqfd/); [01-virtio](../01-virtio/) for request queues.

Labs `07` (ioeventfd) and `08` (irqfd) each showed one half of how a virtual device
talks to a virtual machine. This lab puts the two halves together into the full
request/response loop, runs it on real KVM, and races it against the older way of
doing the same thing.

## What this lab is about

A virtual machine's disk or network card is really a piece of software running on
the host. Every time the guest (the OS inside the VM) wants something from that
device, the same three-step loop happens:

1. **Kick.** The guest tells the device "I've got work for you" by writing to a
   special address, the device's *doorbell*.
2. **Work.** The device does the job: reads from disk, sends a packet, and so on.
3. **Interrupt.** The device tells the guest "done" by raising an *interrupt*,
   which makes the guest's CPU stop what it's doing and handle the result.

How quickly and cheaply this loop runs largely decides how fast a VM's I/O is.
KVM offers two ways to build it, and this lab times both.

## Words you'll need

| Term | Meaning |
| --- | --- |
| **guest** | The software running *inside* the VM. Here it's a tiny C program, [guest.c](guest.c), playing the part of a device driver. |
| **host** | The real machine and the programs on it, including [kick_roundtrip.c](kick_roundtrip.c). |
| **VMM** | "Virtual machine monitor": the host program that creates and runs a VM. QEMU and Firecracker are VMMs; `kick_roundtrip` is a tiny one. |
| **vCPU** | A virtual CPU. On the host it's just a thread that calls `ioctl(KVM_RUN)`: while that call is running, the thread *is* the guest's CPU. |
| **VM exit** | The guest stops and the CPU switches to KVM. If KVM can handle the reason itself, it resumes the guest quickly. If not, `KVM_RUN` returns to the VMM's code with a reason (an "exit to userspace"), and the guest stays frozen until the VMM calls `KVM_RUN` again. That second kind is the expensive one, often a few microseconds. |
| **MMIO** | "Memory-mapped I/O": talking to a device through ordinary memory reads and writes at special addresses. Explained [below](#mmio-talking-to-a-device-with-ordinary-memory-writes). |
| **doorbell / kick** | A special MMIO address. Writing to it doesn't store anything in memory; it tells the device there's work. The write is called a "kick". |
| **interrupt** | A signal that makes the guest's CPU drop what it's doing and run a handler. A sleeping CPU (one that ran `hlt`, "halt until an interrupt") wakes up. |
| **eventfd** | Linux's simplest file descriptor: a counter. `write()` adds to it; `read()` returns the total and resets it to 0, or waits if it's 0. |
| **ioeventfd** | A KVM feature (lab 07): "when the guest writes to this doorbell, just add 1 to this eventfd". KVM handles the write itself and resumes the guest; `KVM_RUN` doesn't return to the VMM. |
| **irqfd** | A KVM feature (lab 08): "when this eventfd is written, raise an interrupt in the guest". Any thread can do it with a plain `write()`. |

## MMIO: talking to a device with ordinary memory writes

The guest kicks the device with one line of C, in [guest.c](guest.c):

```c
*doorbell = id;          /* doorbell is a pointer to address 0xd0000 */
```

That looks like an ordinary store to memory. So why does it reach a device, and
not RAM?

### On a real computer

The CPU reads and writes *physical addresses*. Most of that address range is
backed by RAM chips, but the platform reserves some ranges for devices. When the
CPU writes to one of those, the write isn't stored anywhere. It's delivered to the
device, which reacts to it:

```text
physical address space (simplified)

  0x0000_0000 ┌──────────────────────┐
              │ RAM                  │  loads/stores go to memory chips
              ├──────────────────────┤
              │ device registers     │  loads/stores go to a device instead:
              │   e.g. a NIC's       │    write "start transmit" -> it sends
              │   doorbell           │    read "status"          -> it answers
              ├──────────────────────┤
              │ more RAM ...         │
              └──────────────────────┘
```

This is **memory-mapped I/O (MMIO)**: the device's control registers are "mapped"
into the address space, and the driver talks to the device with plain loads and
stores. Who owns which range is set up by the firmware and the OS, for example
through PCI device "BARs", or through a list of addresses passed to the kernel.

A device register behaves differently from memory:

* **Accesses have side effects.** Writing a register can start an operation, and
  reading one can clear a status flag. Every single access matters.
* **Reading back doesn't necessarily return what you wrote.** The value comes from
  the device, not from storage.

That's why driver code accesses registers through `volatile` pointers, as
`doorbell` is in [guest.c](guest.c). `volatile` stops the compiler from dropping,
merging or reordering the accesses as it could with normal memory. (The CPU is also
set up not to cache these addresses.)

x86 has a second, older way to reach devices: **port I/O**. That's a separate,
small address space (65,536 "ports") used with the special `in` and `out`
instructions instead of normal loads and stores. This lab's timing markers use
port I/O (`outb` in [guest.c](guest.c), which shows up in the host as
`KVM_EXIT_IO`). Modern devices, including virtio, mostly use MMIO.

### Inside a VM

The guest believes it has physical addresses too, but they're *guest* physical
addresses, and KVM decides what backs each one. The VMM tells KVM which ranges are
RAM by registering **memslots**. This lab registers exactly one, in
[kick_roundtrip.c](kick_roundtrip.c):

```c
/* guest addresses 0 .. 64 KiB are backed by this host memory */
struct kvm_userspace_memory_region rg = {.slot = 0, .memory_size = GUEST_MEM_SIZE,
                                         .userspace_addr = (uintptr_t)c.guest_mem};
ioctl(c.vm, KVM_SET_USER_MEMORY_REGION, &rg);
```

Guest accesses inside a memslot go straight to that host memory at full speed. The
CPU's second-level page tables (EPT on Intel, NPT on AMD) translate the address,
and KVM isn't involved at all. That's how the guest's code, stack and the shared
`completed` counter work.

The doorbell at `0xd0000` lies **outside every memslot**, so there's nothing to
translate it to. Here's what happens when the guest stores to it:

```text
guest: *doorbell = id;     (a store to 0xd0000)
  │
  ▼
1. The CPU finds no mapping for 0xd0000, stops the guest, and switches to
   KVM (a VM exit).
  │
  ▼
2. KVM works out what the guest was doing: it decodes the instruction that
   faulted to get the address, the size (4 bytes) and the value (id).
  │
  ▼
3. KVM asks its in-kernel devices: "does anyone own 4 bytes at 0xd0000?"
  │
  ├── yes: an ioeventfd is registered there (FAST path)
  │     KVM adds 1 to the eventfd and resumes the guest at the next
  │     instruction. The VMM never hears about it.
  │
  └── no (CLASSIC path)
        KVM_RUN returns to the VMM with exit_reason = KVM_EXIT_MMIO, and the
        details in the shared kvm_run page:
            run->mmio.phys_addr   0xd0000
            run->mmio.is_write    1
            run->mmio.len         4
            run->mmio.data        the bytes of id
        The VMM acts as the device, then calls KVM_RUN to resume the guest.
```

The CLASSIC branch is the `case KVM_EXIT_MMIO:` in `run_config()` in
[kick_roundtrip.c](kick_roundtrip.c). It checks `phys_addr` and `is_write`, does
the device work, and raises the interrupt. For a guest **read** from an MMIO
address, the VMM would instead put the answer in `run->mmio.data` before calling
`KVM_RUN`, and KVM would complete the guest's load with that value.

Two things follow from this picture:

* **Steps 1 and 2 happen on both paths.** An ioeventfd doesn't prevent the exit
  into KVM. It prevents the much more expensive trip out to the VMM and back,
  and it frees the vCPU thread from doing the device work. (Lab 07 measures the
  difference.)
* **The address `0xd0000` is arbitrary.** It only has to be outside RAM and agreed
  on by both sides, which [shared.h](shared.h) does. A real VMM picks device
  addresses the same way and tells the guest's OS where they are, through PCI BARs
  or, for virtio-mmio devices such as Firecracker's, in the guest kernel's command
  line.

## The two ways to wire the device

**An analogy.** A waiter (the guest's CPU) takes orders at the tables.

* **Classic:** the waiter takes an order, walks into the kitchen, and cooks it
  personally. While cooking, nobody else is served. When the dish is done, the
  waiter goes back to the tables.
* **Fast:** the waiter drops the order ticket through the kitchen hatch and goes
  straight back to the tables. A separate cook makes the dish and rings a bell
  when it's ready.

In code:

```text
CLASSIC   guest kick ──VM exit──▶ vCPU thread does the work ──KVM_IRQ_LINE──▶ guest interrupt
          (the guest is frozen the whole time: its own thread is busy being the device)

FAST      guest kick ──ioeventfd──▶ device thread does the work ──irqfd──▶ guest interrupt
          (the guest keeps running; its vCPU thread never leaves the kernel for a kick)
```

**Classic, step by step:**

1. The guest writes to the doorbell address.
2. Nothing inside KVM is registered for that address, so KVM stops the guest and
   `KVM_RUN` returns to our code with `KVM_EXIT_MMIO` ("the guest wrote to a device
   address").
3. Our vCPU thread plays the device: it does the work, then raises the interrupt
   with the `KVM_IRQ_LINE` ioctl.
4. It calls `KVM_RUN` again, and the guest resumes and takes the interrupt.

**Fast, step by step:**

1. The guest writes to the doorbell address.
2. KVM sees an ioeventfd registered for it, adds 1 to the "kick" eventfd, and lets
   the guest carry on immediately. `KVM_RUN` doesn't return.
3. A separate **device thread**, asleep in `read()` on that eventfd, wakes up, does
   the work, and records the completion in memory the guest can see.
4. It raises the interrupt with `write()` on the irqfd. KVM delivers it straight to
   the guest.

The fast path is how real virtio devices are wired in QEMU and Firecracker. It's also
what lets the device live inside the kernel instead (vhost), since the device side is
only eventfds.

## What the guest does

[guest.c](guest.c) is a small C program running with no operating system underneath
it. It acts as a device driver and sends N requests in one of two modes:

* **Pipelined:** send all N requests without waiting, then wait for them all. This
  is like a database firing off many asynchronous reads at once.
* **Ping-pong:** send one request, wait for it to finish, then send the next. This
  is like a program doing one blocking read at a time.

The device reports progress through a counter, `completed`, in memory shared with
the guest ([shared.h](shared.h) has the layout). The interrupt only means "go and
check the counter"; the counter is the real answer. Real virtio devices work the
same way, with a "used ring" in shared memory instead of a single counter.

The device's "work" is simulated: it keeps a CPU busy for **W microseconds** per
request.

The guest also writes to a few *marker* ports: when it starts, when it has sent
everything, and when everything has completed. Each marker causes one VM exit on
purpose, so the host can record the time. There are three per run, not one per
request, so they don't affect the results.

## Run it

```sh
make run                     # N=20000 requests, W=10 us of work each
./kick_roundtrip 20000 0     # the same with W=0: no work at all, only the loop
```

## Results (this machine)

```text
N=20000 requests, device work W=10 us per request

1) PIPELINED (deep queue): 'submit' = time the guest vCPU spent issuing kicks
  classic: exit + KVM_IRQ_LINE   submit 257.0 ms | all done 257.0 ms (12.85 us/req) | KVM_EXIT_MMIO 20000 | guest irqs 20000
  fast: ioeventfd + irqfd        submit  19.4 ms | all done 225.1 ms (11.25 us/req) | KVM_EXIT_MMIO     0 | guest irqs 19728

2) PING-PONG (queue depth 1), W=0 -> pure round-trip latency (N/4 requests)
  classic: exit + KVM_IRQ_LINE   submit  13.5 ms | all done  13.5 ms ( 2.69 us/req) | KVM_EXIT_MMIO  5000 | guest irqs  5000
  fast: ioeventfd + irqfd        submit  19.0 ms | all done  19.0 ms ( 3.79 us/req) | KVM_EXIT_MMIO     0 | guest irqs  5000
```

What each column means:

* **submit:** time from the guest's first kick until it had *sent* every request.
* **all done:** time from the first kick until every request was *completed*.
  `us/req` is that divided by the number of requests.
* **KVM_EXIT_MMIO:** how many kicks stopped the guest and came back to the VMM.
* **guest irqs:** how many times the guest's interrupt handler actually ran.

### How to read it, including where the fast path loses

* **Pipelined: the guest finishes sending 13× sooner** (19 ms vs 257 ms). On the
  classic path, each kick freezes the guest while its own thread does 10 µs of
  device work, so sending 20 000 requests takes 20 000 × (10 µs + exit cost). On the
  fast path, a kick costs the guest almost nothing and it moves straight on.
* **But the total time is about the same** (225 ms vs 257 ms). There's still only
  one device doing the work, and 20 000 × 10 µs = 200 ms however you wire it. The
  win is that the guest's CPU is **free** for those 200 ms instead of stuck doing
  device work. The fast path also leaves room to add more device threads, or to
  move the device into the kernel.
* **`KVM_EXIT_MMIO` 0 shows the mechanism working.** 20 000 kicks, and not one of
  them made `KVM_RUN` return. The only exits on the fast path are the timing
  markers.
* **`guest irqs` 19 728, not 20 000:** some interrupts were merged. If the device
  raises a second interrupt before the guest has handled the first, the guest
  sees only one (the interrupt controller keeps a single "pending" flag per line).
  Nothing is lost, because the guest reads the `completed` counter, not the number
  of interrupts. That's why drivers treat an interrupt as "go and check", never
  as "exactly one thing happened".
* **Ping-pong with no work: the classic path is *faster*** (2.69 µs vs 3.79 µs per
  request). With nothing to do, the classic "device" is just a function call in
  the thread that already has the CPU. The fast path has to hand off between
  threads twice for each request: KVM wakes the device thread, then the device
  thread's irqfd wakes the guest's vCPU thread, which went to sleep waiting. Each
  hand-off costs scheduler time. The classic path doesn't even need the guest to
  sleep: by the time the guest resumes after its kick, the request is already
  complete.

The takeaway: **ioeventfd and irqfd are about throughput and keeping the guest's
CPU free** (and about letting the device live in the kernel, as vhost does). They
don't make a single request faster.

For comparison, the same run with `W=0` (no device work):

```text
1) PIPELINED (deep queue): 'submit' = time the guest vCPU spent issuing kicks
  classic: exit + KVM_IRQ_LINE   submit 56.0 ms | all done 56.0 ms (2.80 us/req) | KVM_EXIT_MMIO 20000 | guest irqs 20000
  fast: ioeventfd + irqfd        submit 43.9 ms | all done 43.9 ms (2.20 us/req) | KVM_EXIT_MMIO     0 | guest irqs 17539
```

With no work to hide, pipelining still helps the fast path a little: the classic
path pays a full VM exit for every request, while the fast path's device thread
can pick up several kicks per wake-up (the eventfd counter adds them up), and more
interrupts get merged (17 539 for 20 000 requests).

## Things you'd meet in a real VMM

* **Order of setup:** `KVM_CREATE_IRQCHIP` (the in-kernel interrupt controller) must
  come before `KVM_CREATE_VCPU`. Then `KVM_IOEVENTFD` and `KVM_IRQFD` wire up the
  fast path.
* **Publish first, then interrupt.** `complete_one()` writes the new `completed`
  value to shared memory *before* the interrupt is raised, with "release" ordering,
  so the guest can never see the interrupt but read a stale count. Real devices do
  the same with their used ring.
* **Sleeping safely.** The guest's `wait_for_completion()` checks the counter with
  interrupts off, then turns them on and sleeps in one step (`sti; hlt`). Otherwise
  an interrupt could land between the check and the sleep, and the guest would
  sleep forever. [guest.c](guest.c) explains this "lost wake-up" in detail.
* **One wake-up can carry many kicks.** Reading an eventfd returns how many writes
  happened since the last read, so `device_thread()` loops `while (kicks--)`.
* **The doorbell value doesn't arrive.** An ioeventfd only counts writes; the
  number the guest wrote (its request id) is lost. A real device learns what to do
  by reading the request queue in guest memory, not from the doorbell.
* **Marker ports must avoid `0x20`/`0x21`.** Those belong to the in-kernel interrupt
  controller, so a write there never reaches the VMM. An early version of this lab
  used them and got timestamps of 0.

## The files

| File | What it is |
| --- | --- |
| [kick_roundtrip.c](kick_roundtrip.c) | The host: builds a fresh VM per run, wires the classic or fast path, and times it. |
| [guest.c](guest.c) | The guest "driver": sets up its interrupt handler, sends the requests, waits for completions. |
| [shared.h](shared.h) | What both sides agree on: the address map, the doorbell, the marker ports, the shared counters. |
| [guest.ld](guest.ld) | Linker script: places the guest program at address 0x1000, where the host loads it. |

The guest is built separately and embedded in the host program:

```text
guest.c --gcc -m32--> guest.elf --objcopy--> guest.bin --#embed--> kick_roundtrip
```

`make disasm` shows what the guest's C compiled to.

## Try next

Replace the busy-wait "device" with real I/O (for example `io_uring` from
`04-io_uring/`), give the device a real request queue (`01-virtio/`), and you have
Firecracker's block-device path in miniature.
