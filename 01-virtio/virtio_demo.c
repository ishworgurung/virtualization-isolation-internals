/*
 * virtio_demo.c -- a complete, runnable model of a virtio-mmio block device
 *                  and the split virtqueue it is driven through.
 *
 * WHAT THIS IS
 * ------------
 * A real virtio device lives across a VM boundary: the *driver* runs inside the
 * guest kernel, the *device* runs in the VMM (Firecracker, QEMU, ...).  They
 * share nothing except (a) guest RAM, which the VMM has mmap()ed and can see,
 * and (b) a few trapping "doorbell" registers.
 *
 * WHY VIRTIO EXISTS (the one-paragraph version)
 * ---------------------------------------------
 * If a hypervisor pretends to be a real, physical disk controller, the guest's
 * ordinary driver for that controller pokes dozens of hardware registers per
 * disk request.  Every one of those pokes is an access to memory that is not
 * really RAM, so the CPU must stop running the guest and hand control to the
 * hypervisor (a "VM exit") so that software can pretend to be the hardware.
 * Exits are expensive (hundreds of nanoseconds to microseconds each; see
 * 02-hypercalls).  virtio instead defines a *made-up* device whose driver knows
 * it is virtualised: the guest writes whole requests into ordinary RAM (which
 * costs nothing extra) and then touches ONE register -- the doorbell -- to say
 * "go look".  One exit per batch instead of dozens per request.
 *
 * HOW THIS DEMO MODELS THE BOUNDARY
 * ---------------------------------
 * We cannot boot a guest kernel in a few hundred lines, so we model the
 * boundary with two threads in one process:
 *
 *      main thread   == the GUEST DRIVER   (touches guest RAM + MMIO registers)
 *      device thread == the HOST  DEVICE   (the code you would find in a VMM)
 *
 * Every mechanism that crosses the boundary in real life is mapped 1:1:
 *
 *   real world                               this demo
 *   ---------------------------------------  ---------------------------------
 *   guest RAM (KVM memslot, mmap'ed by VMM)  anonymous mmap(), `guest_ram`
 *   guest-physical address (GPA)             byte offset into `guest_ram`
 *   MMIO store to QueueNotify -> VM exit     mmio_write(QUEUE_NOTIFY)
 *   KVM ioeventfd (exit handled in-kernel)   notify_fd (an eventfd)
 *   KVM irqfd (interrupt injection)          irq_fd    (an eventfd)
 *   VMM device emulation thread              device_thread()
 *
 * Glossary for readers who normally work higher up the stack:
 *   GPA      "guest-physical address": what the guest believes is a physical
 *            RAM address.  The VMM maps guest RAM somewhere in its own virtual
 *            address space, so a GPA is really just an offset into that mapping.
 *   HVA      "host virtual address": an ordinary pointer in the VMM process.
 *   MMIO     "memory-mapped I/O": device registers that look like memory
 *            addresses.  Loads/stores to them talk to the device, not to RAM.
 *   eventfd  a Linux file descriptor that holds a 64-bit counter.  write(fd, n)
 *            adds n and wakes readers; read(fd) blocks until the counter is
 *            non-zero, returns it, and resets it to 0.  It is the cheapest
 *            "wake up another thread (or the kernel)" primitive Linux has.
 *
 * The virtqueue memory layout and the barriers follow the virtio 1.x
 * specification, "Split Virtqueues" chapter (section numbers differ between
 * spec versions 1.0 / 1.1 / 1.2, so we cite chapters by name below).
 * virtio 1.x structures are little-endian; we use native integers because x86
 * and (typically) ARM hosts are little-endian too.
 *
 * BUILD & RUN:   make && ./virtio_demo
 */

#define _GNU_SOURCE
#include <pthread.h>     /* pthread_create/join: the "device" thread         */
#include <stdatomic.h>   /* _Atomic, atomic_load/store/fetch_*               */
#include <stdint.h>      /* fixed-width integers matching the spec's layouts */
#include <stdio.h>       /* printf                                           */
#include <string.h>      /* memcpy/memset                                    */
#include <sys/eventfd.h> /* eventfd(), eventfd_read(), eventfd_write()       */
#include <sys/mman.h>    /* mmap() for the pretend guest RAM                 */
#include <unistd.h>

/* ========================================================================== *
 * 1. THE SPLIT VIRTQUEUE (the shared-memory protocol)
 * ========================================================================== *
 *
 * A virtqueue is three arrays in guest memory:
 *
 *   descriptor table : who/what/where -- array of {addr,len,flags,next}
 *                      written by the DRIVER, read by the DEVICE
 *   available ring   : "these descriptor chains are ready"
 *                      written by the DRIVER, read by the DEVICE
 *   used ring        : "I have finished these chains"
 *                      written by the DEVICE, read by the DRIVER
 *
 * Each ring has exactly ONE writer -> no locks needed.  That is the SPSC
 * (single-producer / single-consumer) lock-free ring from 05-lockfree, living
 * across a VM boundary.
 *
 * Life of one request:
 *   1. driver fills buffers in guest RAM and describes them with descriptors
 *      (possibly several, linked with `next` into a "chain");
 *   2. driver writes the index of the chain's FIRST descriptor (its "head")
 *      into avail.ring[avail.idx % size] and then increments avail.idx;
 *   3. driver rings the doorbell (optional -- see notification suppression);
 *   4. device sees avail.idx moved, walks the chain, does the I/O;
 *   5. device writes {head, bytes_written} into used.ring[used.idx % size]
 *      and increments used.idx;
 *   6. device interrupts the guest; driver sees used.idx moved and reaps.
 */

#define QUEUE_SIZE 16 /* must be a power of two; spec allows up to 32768 */

/* Descriptor flags ("The Virtqueue Descriptor Table" in the spec).  The spec
 * also defines VIRTQ_DESC_F_INDIRECT (4): the buffer holds a *further* table of
 * descriptors.  We do not model it. */
#define VQ_DESC_F_NEXT 1  /* this buffer continues in desc[next]            */
#define VQ_DESC_F_WRITE 2 /* DEVICE writes this buffer (else device reads)  */

/* One descriptor: 16 bytes.  It does not contain data, it POINTS at data. */
struct vq_desc {
  uint64_t addr;  /* guest-physical address of the buffer */
  uint32_t len;   /* length of the buffer in bytes        */
  uint16_t flags; /* VQ_DESC_F_*                          */
  uint16_t next;  /* next descriptor index if F_NEXT      */
};

/* The available ring: the driver's "to-do list" for the device. */
struct vq_avail {
  uint16_t flags; /* VIRTQ_AVAIL_F_NO_INTERRUPT (1): "don't interrupt me".
                     We leave it 0, i.e. always interrupt.                  */
  uint16_t idx;              /* free-running counter; only ever incremented */
  uint16_t ring[QUEUE_SIZE]; /* heads of descriptor chains                  */
  /* uint16_t used_event;      only with VIRTIO_F_EVENT_IDX, not modelled   */
};

/* One completion record. `id` is u32 in the spec even though descriptor
 * indices fit in u16 -- that is just how the layout was defined. */
struct vq_used_elem {
  uint32_t id;  /* head index of the chain that was completed  */
  uint32_t len; /* bytes the device WROTE into the chain       */
};

/* The used ring: the device's "done list" for the driver. */
struct vq_used {
  uint16_t flags; /* VIRTQ_USED_F_NO_NOTIFY (1): "don't kick me, I'm polling".
                     We leave it 0, i.e. the driver always kicks.            */
  uint16_t idx; /* free-running counter, incremented by the device */
  struct vq_used_elem ring[QUEUE_SIZE];
  /* uint16_t avail_event;     only with VIRTIO_F_EVENT_IDX, not modelled   */
};

/* ========================================================================== *
 * 2. "GUEST RAM" and address translation
 * ========================================================================== */

#define GUEST_RAM_SIZE (1u << 20) /* 1 MiB of pretend guest RAM */

/* Where the driver decides to place things inside guest RAM (GPAs).  The spec
 * requires 16-byte alignment for the descriptor table, 2 for the available
 * ring and 4 for the used ring; page-aligned addresses satisfy all three. */
#define GPA_DESC_TABLE 0x1000
#define GPA_AVAIL_RING 0x2000
#define GPA_USED_RING 0x3000
#define GPA_REQ_AREA 0x10000 /* request buffers: one 4 KiB slot per request */

/* Host pointer to the start of guest RAM.  In a real VMM this is the address
 * the VMM passed to KVM as `userspace_addr` of a memslot. */
static uint8_t *guest_ram;

/*
 * gpa_to_hva -- translate a guest-physical range to a host pointer.
 *
 * SECURITY: the guest is UNTRUSTED.  Every address/length that comes out of a
 * descriptor MUST be range-checked before the VMM dereferences it, otherwise
 * a malicious guest reads/writes arbitrary VMM memory.  This function is the
 * moral equivalent of the bounds-checked accessors (e.g. `get_slice()`) in the
 * rust-vmm `vm-memory` crate that Firecracker uses.
 *
 * Note the overflow-safe form: `len > size - gpa`, NOT `gpa + len > size`.
 * With 64-bit unsigned arithmetic, gpa + len can wrap past 2^64 back to a small
 * number and slip under the limit; `size - gpa` cannot underflow because we
 * already know gpa < size.
 *
 * Returns NULL if any byte of [gpa, gpa+len) lies outside guest RAM.
 */
static void *gpa_to_hva(uint64_t gpa, uint64_t len) {
  if (gpa >= GUEST_RAM_SIZE || len > GUEST_RAM_SIZE - gpa)
    return NULL;
  return guest_ram + gpa;
}

/* ========================================================================== *
 * 3. virtio-mmio register file (what the guest driver pokes)
 * ========================================================================== *
 * Offsets from the spec's "MMIO Device Register Layout" (virtio-mmio,
 * register-layout version 2 == virtio 1.x; version 1 is the pre-1.0 "legacy"
 * layout).  Firecracker uses this transport instead of virtio-pci: a flat page
 * of registers at a fixed address announced on the kernel command line, no PCI
 * bus to enumerate or emulate -- far less code to get wrong.
 *
 * Every register is 32 bits wide.  R = guest reads it, W = guest writes it.
 * "...Sel" registers are selectors: they choose WHICH 32-bit half (features) or
 * WHICH queue the neighbouring registers refer to.
 */
#define MMIO_MAGIC 0x000        /* R  "virt" = 0x74726976                     */
#define MMIO_VERSION 0x004      /* R  2                                       */
#define MMIO_DEVICE_ID 0x008    /* R  2 == block device                       */
#define MMIO_VENDOR_ID 0x00c    /* R                                          */
#define MMIO_DEV_FEATURES 0x010 /* R  32 feature bits selected by ...Sel */
#define MMIO_DEV_FEAT_SEL 0x014 /* W */
#define MMIO_DRV_FEATURES 0x020 /* W  what the driver accepts */
#define MMIO_DRV_FEAT_SEL 0x024 /* W */
#define MMIO_QUEUE_SEL 0x030    /* W  which queue the next regs talk about    */
#define MMIO_QUEUE_NUM_MAX 0x034 /* R  device's max queue size */
#define MMIO_QUEUE_NUM 0x038    /* W  driver's chosen queue size              */
#define MMIO_QUEUE_READY 0x044  /* RW  */
#define MMIO_QUEUE_NOTIFY 0x050 /* W  THE DOORBELL -> causes a VM exit */
#define MMIO_INT_STATUS 0x060   /* R  why was I interrupted                   */
#define MMIO_INT_ACK 0x064      /* W                                          */
#define MMIO_STATUS 0x070       /* RW device status handshake                 */
#define MMIO_QUEUE_DESC_LO 0x080 /* W  GPA of descriptor table */
#define MMIO_QUEUE_DESC_HI 0x084
#define MMIO_QUEUE_DRV_LO 0x090 /* W  GPA of available ring ("driver area") */
#define MMIO_QUEUE_DRV_HI 0x094
#define MMIO_QUEUE_DEV_LO 0x0a0 /* W  GPA of used ring ("device area") */
#define MMIO_QUEUE_DEV_HI 0x0a4
#define MMIO_CONFIG 0x100 /* R  device-specific config (blk: capacity)  */

/* Device status bits (spec: "Device Status Field") -- the initialisation
 * handshake.  The driver sets them cumulatively, in this order:
 * ACKNOWLEDGE -> DRIVER -> FEATURES_OK -> DRIVER_OK.  Writing 0 resets the
 * device.  (The spec also defines FAILED = 128 and DEVICE_NEEDS_RESET = 64.)
 * Note the numeric values are NOT in handshake order: FEATURES_OK (8) was added
 * in virtio 1.0 after DRIVER_OK (4) already existed. */
#define ST_ACKNOWLEDGE 1 /* guest noticed the device                        */
#define ST_DRIVER 2      /* guest knows how to drive it                     */
#define ST_DRIVER_OK 4   /* driver is ready, device may start processing    */
#define ST_FEATURES_OK 8 /* feature negotiation finished                    */

/* Feature bit 32.  A driver that does not accept it is a "legacy" (pre-1.0)
 * driver; a modern device may refuse to work with it. */
#define VIRTIO_F_VERSION_1 (1ull << 32) /* "I am a modern (1.x) device" */

/* virtio-blk request types and status codes (spec: "Block Device" chapter,
 * "Device Operation"). */
#define BLK_T_IN                                                               \
  0 /* read  from disk into guest (device WRITES the data buf)   */
#define BLK_T_OUT                                                              \
  1 /* write from guest to disk   (device READS  the data buf)   */
#define BLK_S_OK 0     /* request succeeded                                  */
#define BLK_S_IOERR 1  /* device or driver error                             */
#define BLK_S_UNSUPP 2 /* request type not supported                         */

/* virtio-blk always counts in 512-byte sectors, whatever the real disk's
 * block size is. */
#define SECTOR_SIZE 512
#define DISK_SECTORS 64 /* our in-memory "disk" is 64 * 512 = 32 KiB */

/* virtio-blk request header: first descriptor of every request chain.
 * Exactly 16 bytes: le32 type, le32 reserved, le64 sector. */
struct blk_req_hdr {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
};

/* All the state the *host side* keeps for one device. */
struct device {
  /* --- registers written by the guest via MMIO --- */
  uint32_t status;
  uint32_t dev_feat_sel, drv_feat_sel;
  uint64_t drv_features;
  uint32_t queue_sel, queue_num, queue_ready;
  uint64_t desc_gpa, avail_gpa, used_gpa;
  _Atomic uint32_t int_status; /* written by device thread, read by guest */

  /* --- private device state --- */
  uint16_t last_avail_idx; /* how far *we* have consumed the avail ring.
                              Lives in host memory: the guest must never
                              be able to rewind it.                       */
  int notify_fd;           /* "ioeventfd": guest kick arrives here         */
  int irq_fd;              /* "irqfd": we signal the guest here            */
  atomic_bool stop;        /* set by main() to shut the device thread down */
  uint8_t disk[DISK_SECTORS * SECTOR_SIZE]; /* the backing "disk image"    */

  /* --- statistics (atomic: bumped on one thread, printed on another) --- */
  _Atomic unsigned kicks, irqs, requests;
};

/* One global device instance.  Static storage => zero-initialised. */
static struct device dev;

/* ========================================================================== *
 * 4. HOST SIDE: MMIO emulation + the queue-processing engine
 * ========================================================================== */

/*
 * mmio_read / mmio_write -- called on the *guest's* thread in this demo, but
 * conceptually they run on the host: in real life each call is a VM exit ->
 * KVM_EXIT_MMIO -> VMM vCPU loop -> this function.  (02-hypercalls shows that
 * loop on real KVM; 07-ioeventfd shows how the doorbell skips it.)
 *
 * `off` is the offset of the accessed register inside the device's MMIO page.
 * A real VMM first finds WHICH device owns the faulting address (Firecracker
 * keeps a sorted "MMIO bus" of address ranges), then calls that device's
 * read/write handler with the offset -- which is what these functions are.
 *
 * Threading note: registers such as desc_gpa/queue_ready are plain (non-atomic)
 * fields written here on the guest thread and read later on the device thread.
 * That is safe in practice because they are all written BEFORE the first kick,
 * and the kick is an eventfd write/read pair (two system calls), which orders
 * the earlier writes before the device thread's later reads.
 */
static uint32_t mmio_read(uint32_t off) {
  switch (off) {
  case MMIO_MAGIC:
    return 0x74726976; /* "virt" little endian */
  case MMIO_VERSION:
    return 2;
  case MMIO_DEVICE_ID:
    return 2; /* block */
  case MMIO_VENDOR_ID:
    return 0x554d4551; /* ASCII "QEMU" read little-endian; any value is legal */
  case MMIO_DEV_FEATURES: /* 64 feature bits, exposed 32 at a time */
    /* Selector 0 = bits 0..31 (we offer none), selector 1 = bits 32..63,
     * where bit 32 (VERSION_1) becomes bit 0 of the returned word. */
    return dev.dev_feat_sel == 1 ? (uint32_t)(VIRTIO_F_VERSION_1 >> 32) : 0;
  case MMIO_QUEUE_NUM_MAX:
    return QUEUE_SIZE;
  case MMIO_QUEUE_READY:
    return dev.queue_ready;
  case MMIO_INT_STATUS:
    return atomic_load(&dev.int_status);
  case MMIO_STATUS:
    return dev.status;
  case MMIO_CONFIG:
    /* virtio-blk config space starts with `le64 capacity` in 512-byte
     * sectors; a 32-bit read at offset 0x100 returns its low half. */
    return DISK_SECTORS; /* capacity (low 32 bits) */
  default:
    return 0; /* unknown/unimplemented registers read as zero */
  }
}

static void mmio_write(uint32_t off, uint32_t val) {
  switch (off) {
  case MMIO_DEV_FEAT_SEL:
    dev.dev_feat_sel = val;
    break;
  case MMIO_DRV_FEAT_SEL:
    dev.drv_feat_sel = val;
    break;
  case MMIO_DRV_FEATURES:
    /* Two 32-bit writes assemble the 64-bit accepted-feature mask:
     * selector 0 -> bits 0..31, selector 1 -> bits 32..63. */
    if (dev.drv_feat_sel == 0) {
      dev.drv_features |= val;
    } else {
      dev.drv_features |= (uint64_t)val << 32;
    }
    break;
  case MMIO_QUEUE_SEL:
    dev.queue_sel = val;
    break; /* we only have queue 0 */
  case MMIO_QUEUE_NUM:
    dev.queue_num = val;
    break;
  /* The six address registers: each 64-bit GPA arrives as two 32-bit writes.
   * `(x & ~0xffffffff) | val` replaces the low half and keeps the high half;
   * `(x & 0xffffffff) | val << 32` does the opposite. */
  case MMIO_QUEUE_DESC_LO:
    dev.desc_gpa = (dev.desc_gpa & ~0xffffffffull) | val;
    break;
  case MMIO_QUEUE_DESC_HI:
    dev.desc_gpa = (dev.desc_gpa & 0xffffffffull) | (uint64_t)val << 32;
    break;
  case MMIO_QUEUE_DRV_LO:
    dev.avail_gpa = (dev.avail_gpa & ~0xffffffffull) | val;
    break;
  case MMIO_QUEUE_DRV_HI:
    dev.avail_gpa = (dev.avail_gpa & 0xffffffffull) | (uint64_t)val << 32;
    break;
  case MMIO_QUEUE_DEV_LO:
    dev.used_gpa = (dev.used_gpa & ~0xffffffffull) | val;
    break;
  case MMIO_QUEUE_DEV_HI:
    dev.used_gpa = (dev.used_gpa & 0xffffffffull) | (uint64_t)val << 32;
    break;
  case MMIO_QUEUE_READY:
    dev.queue_ready = val;
    break;
  case MMIO_INT_ACK:
    /* The guest acknowledges interrupt causes by writing the same bits back;
     * clear exactly those bits (write-1-to-clear). */
    atomic_fetch_and(&dev.int_status, ~val);
    break;
  case MMIO_STATUS:
    /* Real devices validate the transition order; we just latch it. */
    dev.status = val;
    break;

  case MMIO_QUEUE_NOTIFY:
    /*
     * THE DOORBELL.  This is the single most important line in the file.
     *
     * In a real VM this store hits a GPA that has no RAM behind it, so the
     * CPU's second-level page tables (Intel EPT / AMD NPT) cannot translate
     * it -> VM exit -> KVM.  Without further setup KVM would return to the
     * VMM with KVM_EXIT_MMIO.  With an *ioeventfd* registered on that
     * address, KVM does not even bounce up to the VMM's vCPU thread: it just
     * bumps an eventfd counter in-kernel and re-enters the guest.  A separate
     * device thread wakes on the eventfd.  That is what we do here.
     *
     * The value written (`val`) is the index of the queue being notified;
     * we only have queue 0, so we ignore it.
     */
    atomic_fetch_add(&dev.kicks, 1);
    eventfd_write(dev.notify_fd, 1);
    break;
  default:
    break; /* writes to read-only/unknown registers are ignored */
  }
}

/* Upper bound on descriptors per chain that we are willing to process.  Our
 * requests use exactly 3; anything longer is treated as malformed. */
#define MAX_CHAIN 8

/*
 * walk_chain -- copy a descriptor chain out of guest memory.
 *
 * `table` points at the guest's descriptor table, `head` is the first index
 * of the chain (taken from the available ring), `out` receives local copies.
 * Returns the number of descriptors (>0) or -1 if the guest handed us garbage.
 *
 * Two classic guest-attack defences are visible here:
 *  1. LOOP DEFENCE: a malicious guest can make desc[3].next = 3.  We bound the
 *     walk (MAX_CHAIN, and never more than the queue size) so we can never
 *     spin forever.  Every index is also checked against QUEUE_SIZE so the
 *     guest cannot make us read past the end of the table.
 *  2. TOCTOU DEFENCE ("time of check to time of use"): descriptors live in
 *     memory the guest can modify AT ANY TIME, concurrently with us (another
 *     guest vCPU can run while the device works).  We copy each descriptor
 *     ONCE into local storage and only ever look at the copy.  Reading
 *     `desc[i].len` twice (once to validate, once to use) would be a
 *     "double-fetch" bug: the guest could change it between the two reads.
 */
static int walk_chain(const struct vq_desc *table, uint16_t head,
                      struct vq_desc out[MAX_CHAIN]) {
  uint16_t idx = head;
  int n = 0;
  for (;;) {
    if (idx >= QUEUE_SIZE || n >= MAX_CHAIN || n >= QUEUE_SIZE)
      return -1;
    out[n] = table[idx]; /* single fetch into a local copy */
    n++;
    if (!(out[n - 1].flags & VQ_DESC_F_NEXT))
      return n; /* last descriptor of the chain */
    idx = out[n - 1].next;
  }
}

/*
 * handle_blk_request -- interpret one virtio-blk chain:
 *
 *      desc0 [device-readable ]  struct blk_req_hdr    (16 bytes)
 *      desc1 [readable/writable] data buffer           (512 bytes)
 *      desc2 [device-writable ]  1-byte status
 *
 * ("device-readable" = the device may only read it: no VQ_DESC_F_WRITE flag.)
 * Real virtio-blk allows any number of data descriptors of any multiple of 512
 * bytes; this demo accepts exactly one 512-byte data buffer to stay short.
 *
 * `ch` is the validated LOCAL copy produced by walk_chain(); `n` its length.
 * Returns how many bytes the device wrote into guest memory (for used.len).
 * Any malformed input yields BLK_S_IOERR / BLK_S_UNSUPP -- never a crash.
 */
static uint32_t handle_blk_request(const struct vq_desc *ch, int n) {
  /* The status byte is the last descriptor; we need it even on error. */
  uint8_t *status = NULL;
  if (n >= 2 && (ch[n - 1].flags & VQ_DESC_F_WRITE) && ch[n - 1].len >= 1)
    status = gpa_to_hva(ch[n - 1].addr, 1);
  if (!status)
    return 0; /* can't even report the error; drop the request */

  uint32_t written = 1; /* the status byte itself */

  /* Header must be device-readable and exactly big enough. */
  struct blk_req_hdr hdr;
  void *hp = gpa_to_hva(ch[0].addr, sizeof hdr);
  if (n != 3 || (ch[0].flags & VQ_DESC_F_WRITE) || ch[0].len < sizeof hdr ||
      !hp) {
    *status = BLK_S_IOERR;
    return written;
  }
  /* Copy the header out of guest RAM ONCE (same double-fetch reasoning as in
   * walk_chain): from here on we only look at our private `hdr`. */
  memcpy(&hdr, hp, sizeof hdr);

  const struct vq_desc *data_descriptor = &ch[1]; /* data descriptor */
  /* Validate the guest-chosen sector BEFORE doing arithmetic with it.
   * Checking `sector * 512 + len > disk size` instead would be unsafe: for a
   * huge sector number the multiplication/addition wraps around 2^64 to a
   * small value, passes the check, and indexes outside dev.disk. */
  if (data_descriptor->len != SECTOR_SIZE || hdr.sector >= DISK_SECTORS) {
    *status = BLK_S_IOERR;
    return written;
  }
  uint64_t byte_off = hdr.sector * SECTOR_SIZE; /* now < sizeof dev.disk */
  void *data = gpa_to_hva(
      data_descriptor->addr,
      data_descriptor->len); /* <- the bounds check that stops evil GPAs */
  if (!data) {
    *status = BLK_S_IOERR;
    return written;
  }

  switch (hdr.type) {
  case BLK_T_IN:
    /* Read from disk: the device must be allowed to WRITE the data buffer. */
    if (!(data_descriptor->flags & VQ_DESC_F_WRITE)) {
      *status = BLK_S_IOERR;
      break;
    }
    memcpy(data, dev.disk + byte_off, SECTOR_SIZE);
    written += SECTOR_SIZE; /* data bytes + status byte go into used.len */
    *status = BLK_S_OK;
    break;
  case BLK_T_OUT:
    /* Write to disk: the data buffer must be device-READABLE (no WRITE flag). */
    if (data_descriptor->flags & VQ_DESC_F_WRITE) {
      *status = BLK_S_IOERR;
      break;
    }
    memcpy(dev.disk + byte_off, data, SECTOR_SIZE);
    *status = BLK_S_OK; /* only the status byte was written into guest RAM */
    break;
  default:
    *status = BLK_S_UNSUPP; /* e.g. FLUSH, GET_ID, DISCARD: not implemented */
  }
  return written;
}

/*
 * process_queue -- drain the available ring.  Called when the guest kicks us.
 *
 * MEMORY ORDERING (the part everyone gets wrong):
 *   The driver did:   write descriptors ; write avail.ring[] ;
 *                     RELEASE-store avail.idx
 *   We therefore do:  ACQUIRE-load avail.idx ; then read ring[] and
 *                     descriptors.
 * The acquire/release pair guarantees that if we observe the new idx we also
 * observe every descriptor byte written before it.  Without it, a weakly
 * ordered CPU (ARM!) could show us the new idx but stale descriptors.  On
 * x86 (TSO, "total store order") the hardware never reorders two loads or two
 * stores, which hides this bug; the compiler can still reorder plain accesses
 * and bite you, which is why the atomics are needed even on x86.
 */
static void process_queue(void) {
  /* Re-translate the ring addresses on every call: they are guest-controlled
   * values, and translation includes the bounds check. */
  const struct vq_desc *table =
      gpa_to_hva(dev.desc_gpa, sizeof(struct vq_desc) * QUEUE_SIZE);
  struct vq_avail *avail = gpa_to_hva(dev.avail_gpa, sizeof *avail);
  struct vq_used *used = gpa_to_hva(dev.used_gpa, sizeof *used);
  if (!table || !avail || !used || !dev.queue_ready)
    return;

  int did_work = 0;
  /* Snapshot the producer's index once; anything published after this will be
   * picked up by the next kick. */
  uint16_t avail_idx = __atomic_load_n(&avail->idx, __ATOMIC_ACQUIRE);

  /* Indices are free-running u16: equality (not <) is the right test, and
   * wrap-around at 65536 works automatically thanks to modular arithmetic.
   * (65536 is a multiple of every legal queue size, so `% QUEUE_SIZE` keeps
   * mapping to the right slot across the wrap.) */
  while (dev.last_avail_idx != avail_idx) {
    /* Read the chain head (a single u16 load, so no double-fetch issue);
     * walk_chain() range-checks it before use. */
    uint16_t head = avail->ring[dev.last_avail_idx % QUEUE_SIZE];
    uint32_t written = 0;

    struct vq_desc chain[MAX_CHAIN];
    int n = walk_chain(table, head, chain);
    if (n > 0)
      written = handle_blk_request(chain, n);
    /* n < 0: broken chain.  A production VMM would mark the device
     * NEEDS_RESET.  We just complete it with len 0. */

    /* Publish completion.  We are the ONLY writer of the used ring, so a
     * plain read of used->idx is fine.  The element must be visible
     * before the index that announces it -> RELEASE store. */
    uint16_t uidx = used->idx;
    used->ring[uidx % QUEUE_SIZE] = (struct vq_used_elem){head, written};
    __atomic_store_n(&used->idx, (uint16_t)(uidx + 1), __ATOMIC_RELEASE);

    dev.last_avail_idx++;
    atomic_fetch_add(&dev.requests, 1);
    did_work = 1;
  }

  if (did_work) {
    /* Tell the guest.  Real hardware: KVM irqfd injects a virtual IRQ.
     * One interrupt may cover many completions (coalescing).
     * Bit 0 of InterruptStatus = "used buffer notification"; bit 1 would be
     * "configuration changed". */
    atomic_fetch_or(&dev.int_status, 1 /* "used buffer notification" */);
    atomic_fetch_add(&dev.irqs, 1);
    eventfd_write(dev.irq_fd, 1);
  }
}

/* The device thread == what a VMM device-emulation thread does all day:
 * sleep until kicked, drain the queue, repeat. */
static void *device_thread(void *arg) {
  (void)arg;
  for (;;) {
    eventfd_t v; /* number of kicks since the last read; we don't need it,
                    because process_queue() drains EVERYTHING available */
    if (eventfd_read(dev.notify_fd, &v) < 0) /* sleeps until the guest kicks */
      break;
    if (atomic_load(&dev.stop))
      break;
    process_queue();
  }
  return NULL;
}

/* ========================================================================== *
 * 5. GUEST SIDE: the (very small) virtio-blk driver
 * ========================================================================== */

/* Guest-side views of the three virtqueue arrays (set up in driver_init). */
static struct vq_desc *g_desc;
static struct vq_avail *g_avail;
static struct vq_used *g_used;
static uint16_t g_avail_shadow; /* driver's private copy of avail->idx */
static uint16_t g_last_used;    /* how far the driver consumed the used ring */
static uint16_t g_next_desc;    /* trivial allocator: descs are handed out in
                                   order and recycled when the batch drains.
                                   Real drivers keep a free list.            */
static int g_slot;              /* which 4 KiB request slot to use next       */

/* Tagged logging so the output shows which side of the boundary acted.
 * The do { } while (0) wrapper makes the macro behave like one statement. */
#define GUEST_LOG(...)                                                         \
  do {                                                                         \
    printf("[guest] " __VA_ARGS__);                                            \
  } while (0)
#define HOST_LOG(...)                                                          \
  do {                                                                         \
    printf("[host ] " __VA_ARGS__);                                            \
  } while (0)

/* Guest-side helper: write a register and log the handshake for the reader. */
static void write_register(const char *name, uint32_t off, uint32_t val) {
  GUEST_LOG("MMIO write %-14s (0x%03x) = 0x%x\n", name, off, val);
  mmio_write(off, val);
}

/* Device discovery + init: the virtio "handshake" (spec: "Device
 * Initialization").  A Linux guest's virtio_mmio driver performs essentially
 * this sequence. */
static void driver_init(void) {
  /* Step 0: identify the device.  A driver must check the magic value and
   * version before touching anything else. */
  GUEST_LOG("magic=0x%x version=%u device_id=%u (2 = block)\n",
            mmio_read(MMIO_MAGIC), mmio_read(MMIO_VERSION),
            mmio_read(MMIO_DEVICE_ID));

  write_register("STATUS", MMIO_STATUS, 0);              /* reset      */
  write_register("STATUS", MMIO_STATUS, ST_ACKNOWLEDGE); /* I see you  */
  write_register("STATUS", MMIO_STATUS,
                 ST_ACKNOWLEDGE | ST_DRIVER); /* I can drive*/

  /* Feature negotiation: read 32 bits at a time, accept only what we know.
   * Features are the virtio versioning mechanism: device offers, driver
   * accepts a subset, and both then behave according to that subset. */
  write_register("DEV_FEAT_SEL", MMIO_DEV_FEAT_SEL, 1);
  uint32_t hi = mmio_read(MMIO_DEV_FEATURES);
  GUEST_LOG("device offers features[63:32]=0x%x (bit0 => VIRTIO_F_VERSION_1)\n",
            hi);
  write_register("DRV_FEAT_SEL", MMIO_DRV_FEAT_SEL, 1);
  write_register("DRV_FEATURES", MMIO_DRV_FEATURES, hi & 1);
  write_register("STATUS", MMIO_STATUS,
                 ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK);
  /* (A spec-compliant driver now re-reads STATUS to confirm the device kept
   * FEATURES_OK set, i.e. accepted our feature subset.  We skip that.) */

  /* Queue setup: tell the device WHERE in guest RAM our three arrays live. */
  write_register("QUEUE_SEL", MMIO_QUEUE_SEL, 0);
  GUEST_LOG("queue 0 max size = %u\n", mmio_read(MMIO_QUEUE_NUM_MAX));
  write_register("QUEUE_NUM", MMIO_QUEUE_NUM, QUEUE_SIZE);
  write_register("QUEUE_DESC_LO", MMIO_QUEUE_DESC_LO, GPA_DESC_TABLE);
  write_register("QUEUE_DESC_HI", MMIO_QUEUE_DESC_HI, 0);
  write_register("QUEUE_DRV_LO", MMIO_QUEUE_DRV_LO, GPA_AVAIL_RING);
  write_register("QUEUE_DRV_HI", MMIO_QUEUE_DRV_HI, 0);
  write_register("QUEUE_DEV_LO", MMIO_QUEUE_DEV_LO, GPA_USED_RING);
  write_register("QUEUE_DEV_HI", MMIO_QUEUE_DEV_HI, 0);
  write_register("QUEUE_READY", MMIO_QUEUE_READY, 1);

  write_register("STATUS", MMIO_STATUS,
                 ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK |
                     ST_DRIVER_OK); /* go live */

  /* The driver keeps ordinary pointers into the SAME memory the device sees. */
  g_desc = (struct vq_desc *)(guest_ram + GPA_DESC_TABLE);
  g_avail = (struct vq_avail *)(guest_ram + GPA_AVAIL_RING);
  g_used = (struct vq_used *)(guest_ram + GPA_USED_RING);
}

/*
 * queue_request -- build ONE 3-descriptor chain and put its head on the avail
 * ring.  It does NOT kick: the caller batches several requests per kick, which
 * is the whole point of virtqueues (amortise the VM exit).
 *
 * type   : BLK_T_OUT (write to disk) or BLK_T_IN (read from disk)
 * sector : which 512-byte sector of the disk
 * fill   : for writes, the byte value the 512-byte payload is filled with
 * `bad_data_gpa`, if nonzero, replaces the data buffer address: used to play a
 * malicious guest.
 *
 * Layout of one 4 KiB request slot in guest RAM:
 *   +0x000 header (16 B)   +0x100 data (512 B)   +0x400 status (1 B)
 * No bounds checks: this is the (trusted-by-itself) guest side, and the demo
 * never queues more than 4 requests (12 descriptors) per batch, which fits the
 * 16-entry queue.
 */
static void queue_request(uint32_t type, uint64_t sector, uint8_t fill,
                          uint64_t bad_data_gpa) {
  uint64_t base = GPA_REQ_AREA + (uint64_t)g_slot++ * 0x1000;
  uint64_t gpa_hdr = base, gpa_data = base + 0x100, gpa_status = base + 0x400;

  struct blk_req_hdr *hdr = (void *)(guest_ram + gpa_hdr);
  hdr->type = type;
  hdr->reserved = 0;
  hdr->sector = sector;
  if (type == BLK_T_OUT) {
    memset(guest_ram + gpa_data, fill, SECTOR_SIZE); /* payload to write */
  } else {
    memset(guest_ram + gpa_data, 0, SECTOR_SIZE); /* to be overwritten */
  }
  guest_ram[gpa_status] = 0xff; /* sentinel: "not yet completed" */

  /* Three descriptors, linked by `next`: header -> data -> status. */
  uint16_t h = g_next_desc; /* head of the chain */
  g_desc[h + 0] = (struct vq_desc){gpa_hdr, sizeof *hdr, VQ_DESC_F_NEXT, h + 1};
  g_desc[h + 1] = (struct vq_desc){
      bad_data_gpa ? bad_data_gpa : gpa_data, SECTOR_SIZE,
      /* A read-from-disk means the DEVICE writes this buffer -> F_WRITE. */
      (uint16_t)(VQ_DESC_F_NEXT | (type == BLK_T_IN ? VQ_DESC_F_WRITE : 0)),
      h + 2};
  g_desc[h + 2] = (struct vq_desc){gpa_status, 1, VQ_DESC_F_WRITE, 0};
  g_next_desc += 3;

  /* Put the chain head in the next avail slot.  NOTE: we have NOT yet
   * published avail->idx, so the device cannot see this entry -- we are
   * still free to build more requests. */
  g_avail->ring[g_avail_shadow % QUEUE_SIZE] = h;
  g_avail_shadow++;
}

/* Publish everything queued so far and ring the doorbell ONCE. */
static void kick(void) {
  /* RELEASE: all descriptor + ring writes above become visible BEFORE the
   * new idx does.  This is the counterpart of the ACQUIRE in process_queue. */
  __atomic_store_n(&g_avail->idx, g_avail_shadow, __ATOMIC_RELEASE);

  /* Real drivers also need a full barrier here before deciding whether the
   * device wants a notification (VRING_USED_F_NO_NOTIFY / EVENT_IDX).  We
   * always notify. */
  GUEST_LOG("KICK  -> MMIO write QUEUE_NOTIFY (this is the VM exit)\n");
  mmio_write(MMIO_QUEUE_NOTIFY, 0); /* value = queue index 0 */
}

/* Wait for `want` completions, reaping the used ring as interrupts arrive.
 * `print_status` != 0 logs each completion. */
static void wait_completions(int want, int print_status) {
  int got = 0;
  while (got < want) {
    /* Sleep until the "interrupt" (irqfd) fires.  Several device-side
     * signals may have merged into one wake-up (eventfd is a counter), which
     * is exactly why we drain the whole used ring below instead of assuming
     * one completion per interrupt. */
    eventfd_t v;
    eventfd_read(dev.irq_fd, &v);

    /* Interrupt handler: read status, ack, then drain the used ring. */
    uint32_t st = mmio_read(MMIO_INT_STATUS);
    mmio_write(MMIO_INT_ACK, st);

    /* ACQUIRE-load pairs with the device's RELEASE store of used->idx. */
    uint16_t used_idx = __atomic_load_n(&g_used->idx, __ATOMIC_ACQUIRE);
    while (g_last_used != used_idx) {
      struct vq_used_elem e = g_used->ring[g_last_used % QUEUE_SIZE];
      if (print_status) {
        /* Find the status byte: it is the 3rd descriptor of the chain
         * (our chains are always 3 consecutive descriptors). */
        uint64_t status_gpa = g_desc[e.id + 2].addr;
        GUEST_LOG("  completed chain head=%u wrote=%u bytes status=%u (%s)\n",
                  e.id, e.len, guest_ram[status_gpa],
                  guest_ram[status_gpa] == BLK_S_OK ? "OK" : "ERROR");
      }
      g_last_used++;
      got++;
    }
  }
  /* Whole batch done: every descriptor is free again. */
  g_next_desc = 0;
  g_slot = 0;
}

/* ========================================================================== *
 * 6. main -- the demo script
 * ========================================================================== */
int main(void) {
  /* "Boot": allocate guest RAM and wire up the eventfds.  Anonymous mmap
   * returns zero-filled memory, just like a freshly booted VM's RAM. */
  guest_ram = mmap(NULL, GUEST_RAM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (guest_ram == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  dev.notify_fd = eventfd(0, 0); /* blocking eventfds, initial count 0 */
  dev.irq_fd = eventfd(0, 0);

  pthread_t th;
  pthread_create(&th, NULL, device_thread, NULL);

  printf("=== 1. virtio-mmio handshake ===\n");
  driver_init();

  printf("\n=== 2. BATCH of 4 writes, ONE kick ===\n");
  /* Sector i gets filled with the byte 0xA0+i, so each sector is distinct. */
  for (int i = 0; i < 4; i++) {
    queue_request(BLK_T_OUT, i, (uint8_t)(0xA0 + i), 0);
  }
  kick();
  wait_completions(4, 1);

  printf("\n=== 3. BATCH of 4 reads back, ONE kick ===\n");
  for (int i = 0; i < 4; i++) {
    queue_request(BLK_T_IN, i, 0, 0);
  }
  /* Remember where the data buffers are so we can verify after completion
   * (slot i starts at GPA_REQ_AREA + i*4 KiB; data sits at +0x100). */
  uint8_t *bufs[4];
  for (int i = 0; i < 4; i++) {
    bufs[i] = guest_ram + GPA_REQ_AREA + (uint64_t)i * 0x1000 + 0x100;
  }
  kick();
  wait_completions(4, 1);
  for (int i = 0; i < 4; i++) {
    int ok = 1;
    for (int j = 0; j < SECTOR_SIZE; j++) {
      ok &= bufs[i][j] == (uint8_t)(0xA0 + i);
    }
    GUEST_LOG("sector %d readback %s (first byte 0x%02x)\n", i,
              ok ? "MATCHES" : "MISMATCH", bufs[i][0]);
  }

  printf("\n=== 4. MALICIOUS guest: data buffer GPA outside RAM ===\n");
  /* 0xFFFFFFFF0000 is far beyond our 1 MiB of guest RAM. */
  queue_request(BLK_T_IN, 0, 0, 0xFFFFFFFF0000ull);
  kick();
  wait_completions(1, 1);
  HOST_LOG("device survived; the bogus GPA was rejected by gpa_to_hva()\n");

  printf("\n=== 5. Why virtqueues exist: exits per request ===\n");
  unsigned kicks = atomic_load(&dev.kicks), reqs = atomic_load(&dev.requests);
  printf("requests processed : %u\n", reqs);
  printf("doorbell kicks     : %u  (each = 1 VM exit)\n", kicks);
  printf("interrupts raised  : %u  (each = 1 interrupt injection)\n",
         atomic_load(&dev.irqs));
  printf("=> %.2f exits per request; a trap-per-register emulated NIC/disk\n"
         "   would need dozens of exits per request.\n",
         (double)kicks / reqs);

  /* Shut the device thread down: set the flag, then kick so it wakes up,
   * sees the flag and returns. */
  atomic_store(&dev.stop, 1);
  eventfd_write(dev.notify_fd, 1);
  pthread_join(th, NULL);
  return 0;
}
