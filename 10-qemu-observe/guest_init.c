/*
 * guest_init.c -- the whole "operating system userspace" of the test VM.
 *
 * The VM in this lab runs a real Linux kernel (your host's own kernel image),
 * but no distribution: no shell, no systemd, no disk image.  After the kernel
 * boots, the only program it runs is this one.
 *
 * How it gets there
 * -----------------
 * observe.py compiles this file and packs it into an *initramfs*: a small
 * archive that QEMU hands to the kernel with `-initrd` (like a bootloader would).
 * At boot the kernel unpacks the archive into a RAM-only filesystem and runs
 * /init from it as process 1, the first user program.  Normally /init is a
 * shell script or systemd; here it's this file, so it has to do the few
 * set-up jobs they would do (mount /proc and /dev, open a console).
 *
 * It's linked with -static because the archive contains no shared libraries
 * (no libc.so): everything it needs is inside the one binary.
 *
 * What it does
 * ------------
 * Reads the VM's disk, /dev/vda, `iters` times: 4 KiB per read, one read at a
 * time.  The disk is a virtio-blk device, and every read is one request to it.
 * To send a request, the guest's virtio driver writes to the device's
 * *doorbell* (a "kick", see 09-kick-roundtrip).  The host then counts those
 * kicks through KVM's statistics, once with QEMU's ioeventfd off and once on.
 *
 * Talking to observe.py
 * ---------------------
 * The only channel is the serial console, which QEMU writes to a file that
 * observe.py watches:
 *      START                 the workload is about to begin
 *                            (observe.py takes its "before" snapshot of KVM's
 *                            counters during the 1 s pause that follows)
 *      DONE elapsed_ms=<n>   finished (observe.py takes its "after" snapshot)
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <time.h>
#include <unistd.h>

/* The guest's clock, in milliseconds.  CLOCK_MONOTONIC never jumps backwards
 * (unlike the wall clock), so it's the right one for measuring durations. */
static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Process 1 must never exit: if it does, the kernel panics.  So instead of
 * returning, sleep forever.  observe.py kills QEMU when it's done. */
static void sleep_forever(void) {
  for (;;)
    pause();
}

int main(void) {
  /* 1. Mount the two special filesystems we need.  Process 1 starts with
   *    neither; the archive only contains the empty directories to mount on.
   *      /proc  the kernel's live information, used here for /proc/cmdline
   *      /dev   devtmpfs: a /dev the kernel fills in itself, with a file for
   *             every device it has found, such as /dev/vda and /dev/console */
  mount("proc", "/proc", "proc", 0, NULL);
  mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

  /* 2. Connect stdout and stderr to the console, so printf() output goes
   *    somewhere.  (The kernel tries to do this itself before starting /init,
   *    but /dev was still empty then.)  The kernel command line says
   *    console=ttyS0, so /dev/console is the VM's serial port, which QEMU
   *    writes into a file on the host. */
  int console = open("/dev/console", O_WRONLY);
  if (console >= 0) {
    dup2(console, 1); /* make fd 1 (stdout) refer to the console */
    dup2(console, 2); /* and fd 2 (stderr) */
  }
  setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered: every line reaches observe.py at once */

  /* 3. Read "iters=N" from the kernel command line.  observe.py adds it to
   *    QEMU's -append option.  The kernel ignores parameters it doesn't know,
   *    but still shows the full line in /proc/cmdline, so it's an easy way to
   *    pass a setting to a program like this one. */
  long iters = 20000;
  FILE *f = fopen("/proc/cmdline", "r");
  if (f) {
    char line[1024] = {0};
    if (fgets(line, sizeof line, f)) {
      char *p = strstr(line, "iters=");
      if (p)
        iters = atol(p + strlen("iters="));
    }
    fclose(f);
  }

  /* 4. Open the disk.  Its driver may still be starting up, so retry for up
   *    to 10 s (100 tries, 100 ms apart).
   *
   *    O_DIRECT means "don't use the page cache".  Normally Linux keeps
   *    recently read disk blocks in RAM and answers repeat reads from there,
   *    without touching the disk.  That would hide the device from the test,
   *    so O_DIRECT makes every read really go to the (virtual) disk.  The
   *    price: the buffer, the file offset and the length must all be
   *    multiples of the block size, which is why everything below is 4096. */
  int fd = -1;
  for (int i = 0; i < 100 && fd < 0; i++) {
    fd = open("/dev/vda", O_RDONLY | O_DIRECT);
    if (fd < 0)
      usleep(100 * 1000);
  }
  if (fd < 0) {
    puts("ERROR no /dev/vda");
    sleep_forever();
  }

  off_t size = lseek(fd, 0, SEEK_END);   /* the disk's size in bytes (64 MiB) */
  void *buf = aligned_alloc(4096, 4096); /* a 4 KiB buffer at a 4096-aligned address, as O_DIRECT requires */

  puts("START");
  sleep(1); /* give observe.py time to take its "before" snapshot */

  /* 5. The workload.  pread() is synchronous: it returns only when the data
   *    has arrived, so there's exactly one request in flight at a time
   *    ("queue depth 1").  Each request therefore costs exactly one kick,
   *    and the host's counters show one kick per iteration.
   *
   *    The offsets step through the disk 4 KiB at a time and wrap before the
   *    end.  (The disk is a 64 MiB multiple of 4096, so every offset stays a
   *    multiple of 4096.)  The disk file on the host is empty ("sparse"), so
   *    the host has almost nothing to read: what's measured is the path from
   *    guest to QEMU and back, not a physical disk. */
  double t0 = now_ms();
  for (long i = 0; i < iters; i++) {
    off_t offset = ((off_t)i * 4096) % (size - 4096);
    if (pread(fd, buf, 4096, offset) != 4096) {
      puts("ERROR read");
      break;
    }
  }
  printf("DONE elapsed_ms=%.1f iters=%ld\n", now_ms() - t0, iters);

  sleep_forever();
}
