# 11 — SCM_RIGHTS: handing an open file to another process

**Builds on:** [07-ioeventfd](../07-ioeventfd/), whose open question this lab answers, and [08-irqfd](../08-irqfd/).

Lab 07 ends with a question: if two processes each call `eventfd(0, 0)`, how does
the kernel keep them apart? The answer was that an fd number is only a process's
private handle to an open file, and that two processes share one file only if the
file itself is handed over. One way to hand it over is to **send it through a Unix
socket, using `SCM_RIGHTS`**.

This lab does exactly that, in C, and shows the receiver getting the *same* open
file under a *different* number.

## Why a VMM cares

A VMM such as QEMU often runs a device in a **separate process**. A vhost-user
backend is the usual example: virtiofsd for shared folders, or a DPDK switch for
networking. To do its job, that process needs things that are open files inside
the VMM:

* the "kick" eventfd that KVM bumps when the guest rings a doorbell
  (`KVM_IOEVENTFD`, lab 07);
* the "call" eventfd that raises a guest interrupt when written (`KVM_IRQFD`,
  lab 08);
* the guest's memory (a memfd), so it can read and write the request queues.

Sending the fd *number* would be useless, because `3` in the VMM means nothing in the
backend. So QEMU sends the files themselves over a Unix socket with `SCM_RIGHTS`, in
vhost-user messages such as `VHOST_USER_SET_VRING_KICK`,
`VHOST_USER_SET_VRING_CALL` and `VHOST_USER_SET_MEM_TABLE`.

## Words you'll need

| Term | Meaning |
| --- | --- |
| **open file** (`struct file`) | The kernel's object for one `open()`/`eventfd()`/`socket()` call: what the file is, the current offset, the access mode (read/write). |
| **fd table** | A per-process array. Slot *n* holds a pointer to an open file. An **fd** is just an index into it. |
| **fd number** | Only meaningful in its own process. Two processes can both have fd `3` pointing at completely different files. |
| **eventfd** | A kernel counter behind an fd. `write(v)` adds *v*; `read()` returns the total and resets it to 0, or sleeps while it's 0. |
| **Unix socket** (`AF_UNIX`) | A socket that only connects processes on the same machine, through the kernel. |
| **ancillary data** / **cmsg** | Extra typed records that `sendmsg()`/`recvmsg()` can carry alongside the normal bytes. Also called *control messages*. |
| **`SCM_RIGHTS`** | The cmsg type that carries open files. "SCM" = socket-level control message; "rights" because holding an open file means having the right to use it. |

## The picture

```text
BEFORE: the vmm has an eventfd; the backend has nothing that points at it.

  vmm      fd 3 ──▶ eventfd #873
  backend  fd 3 ──▶ eventfd #878   (its own, unrelated)

vmm:      sendmsg(SCM_RIGHTS, fd 3)
          kernel: look up fd 3 in the SENDER's fd table, attach a reference
                  to that open file to the message
backend:  recvmsg()
          kernel: put the file in the lowest free slot of the RECEIVER's
                  fd table (4), and report that number

AFTER: two numbers, two fd tables, one eventfd.

  vmm      fd 3 ──▶ eventfd #873 ◀── fd 4  backend
  backend  fd 3 ──▶ eventfd #878
```

The number is never sent. What travels is a pointer to the kernel's open-file
object. In the kernel, the message keeps these in a `struct scm_fp_list`, an array
of `struct file *`.

## Sending and receiving, in code

The full versions, with every line commented, are `send_fd()` and `recv_fd()` in
[fd_pass.c](fd_pass.c). The core of each:

```c
/* send: one normal byte, plus a control record holding the fd */
char byte = 'F';
struct iovec iov = {.iov_base = &byte, .iov_len = 1};
union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } ctrl = {0};
struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                     .msg_control = ctrl.buf, .msg_controllen = sizeof ctrl.buf};

struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
c->cmsg_level = SOL_SOCKET;
c->cmsg_type  = SCM_RIGHTS;
c->cmsg_len   = CMSG_LEN(sizeof(int));
memcpy(CMSG_DATA(c), &fd, sizeof(int));
sendmsg(sock, &msg, 0);

/* receive: same msghdr setup, then walk the control records */
recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
for (c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
  if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
    memcpy(&newfd, CMSG_DATA(c), sizeof(int));   /* OUR number for it */
```

The `CMSG_*` macros only do size and alignment arithmetic:

| Macro | Gives you |
| --- | --- |
| `CMSG_SPACE(n)` | bytes to allocate for a record with an *n*-byte payload (with padding) |
| `CMSG_LEN(n)` | the value to put in `cmsg_len` |
| `CMSG_FIRSTHDR` / `CMSG_NXTHDR` | the first / next record in the buffer |
| `CMSG_DATA` | where a record's payload (here, the `int` fds) starts |

## What the program does

`fd_pass` forks into two processes named after the roles above: the **vmm**
(parent) and the **backend** (child). The fork happens *before* any eventfd exists,
so the backend can't inherit one. Anything they share has to come through the
socket. They take turns over that same socket so the output reads in order, and
both keep their socket at fd 10, so the fd numbers below are the same on every run.

## Run it

```sh
make run
```

Output from this machine:

```text
STEP 1: each process calls eventfd(0, 0)
  [vmm    ] fd 3 -> anon_inode:[eventfd]  eventfd-id 873  count 0
  [backend] fd 3 -> anon_inode:[eventfd]  eventfd-id 878  count 5
  [vmm    ] fd 3 -> anon_inode:[eventfd]  eventfd-id 873  count 0
  => both got fd 3, but the ids differ and the backend's write(5) didn't
     touch the vmm's counter: two separate eventfds.

STEP 2: the vmm sends its fd 3 with SCM_RIGHTS
  [backend] fd 4 -> anon_inode:[eventfd]  eventfd-id 873  count 0
  => a different number in the backend, the same eventfd-id: one eventfd,
     now reachable from two fd tables.

STEP 3: signal across processes through the shared eventfd
  [backend] read(4): nothing written yet, so I sleep...
  [vmm    ] 200 ms later: write(3, 42)
  [backend] ...woke up, read() returned 42

STEP 4: the backend sends an eventfd, closes its copy, and exits
  [backend] fd 5 -> anon_inode:[eventfd]  eventfd-id 881  count 7
  [backend] sent fd 5, closed it, exiting
  [vmm    ] backend process has exited; now calling recvmsg()
  [vmm    ] fd 4 -> anon_inode:[eventfd]  eventfd-id 881  count 7
  [vmm    ] read(4) -> 7
  => the sender closed it and is gone, yet the eventfd and its count
     survived: the message itself held a reference while in flight.
```

The ids change from run to run. What matters is which ones match.

### How to read it

Every eventfd shows up as `anon_inode:[eventfd]`, and they all share one inode, so
neither the link nor the inode number tells you which eventfd an fd refers to. The
kernel's **`eventfd-id`** (from `/proc/<pid>/fdinfo/<fd>`) does, so the program
prints it. It also prints the counter, read from fdinfo rather than with `read()`,
because `read()` would reset it.

1. **Same number, different eventfds.** Both processes got fd 3, with ids 873 and
   878. The backend wrote 5 to its fd 3, and the vmm's fd 3 still reads 0. This is
   lab 07's question, answered by experiment.
2. **Different number, same eventfd.** The vmm sent fd 3. The backend's fd 3 was
   taken (by its own eventfd), so the kernel used its lowest free slot, 4. The id
   is 873 on both sides, so they are the same eventfd. The receiver never chooses
   the number, and never needs the sender's.
3. **It really is shared.** The backend sleeps in `read(4)`. The vmm's
   `write(3, 42)` wakes it with 42. Two processes, two fd numbers, one counter and
   one wait queue. This is the vhost-user setup: the VMM keeps its copy registered
   with KVM, and the backend waits on its own.
4. **A file in flight is kept alive.** The backend created eventfd 881, wrote 7,
   sent it, closed its own fd and exited. At that moment no process has it in an
   fd table, but the queued message holds a reference, so it isn't freed. The vmm
   receives it afterwards with the count of 7 intact. An open file is freed only
   when the last reference goes: fd table entries, messages in flight, and KVM's
   own references (lab 07) all count.

## Things to know when you use it

* **Send at least one normal byte.** The fds ride along with data. A `sendmsg()`
  with no data bytes carries nothing on a stream socket.
* **Check `MSG_CTRUNC`.** If the receiver's control buffer is too small, the kernel
  **closes** the fds that don't fit and sets this flag. If you don't check it, the
  fds silently go missing.
* **Use `MSG_CMSG_CLOEXEC`** on `recvmsg()`. Without it, received fds survive
  `exec()` and leak into child programs. It's the same idea as `O_CLOEXEC`.
* **Limit: 253 fds per message** (`SCM_MAX_FD` in the kernel's `include/net/scm.h`).
  Size the buffer with `CMSG_SPACE(n * sizeof(int))` to send several fds at once.
* **Unix sockets only.** The receiver gets a pointer to a kernel object, so both
  ends must be on the same machine. Named sockets (`bind()`/`connect()` to a path)
  work the same as the `socketpair()` used here. vhost-user uses a named socket
  between two unrelated programs.
* **Sending an fd grants access.** The receiver can use the file with whatever
  access it was opened with, even if the receiver itself couldn't have opened it:
  there's no permission check on receipt. That's a feature. A privileged broker can
  open `/dev/kvm` or a disk image and hand only that fd to a locked-down process.
  It also means you should know what you're passing.
* **Other ways to share an open file:** `fork()` (the child inherits the parent's
  fd table) and `pidfd_getfd()` (take a copy of an fd from another process, with
  ptrace-level permission). `SCM_RIGHTS` is the one that works between unrelated,
  cooperating processes.

## The files

| File | What it is |
| --- | --- |
| [fd_pass.c](fd_pass.c) | The demo: `send_fd()`/`recv_fd()`, the fdinfo helper, and the vmm/backend steps. |
| [Makefile](Makefile) | `make run` builds and runs it. No special access needed. |

## Try next

* [07-ioeventfd](../07-ioeventfd/) and [08-irqfd](../08-irqfd/) create the eventfds
  that a vhost-user VMM would pass this way.
* [09-kick-roundtrip](../09-kick-roundtrip/) runs the device on a thread in the same
  process. Moving that thread into a separate process takes exactly this lab's
  `send_fd()` for the kick and call eventfds, plus a memfd for guest memory.
