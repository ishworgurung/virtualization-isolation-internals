/*
 * guest_init_virt_mmio.c -- the guest's only program for observe_mmio.py.
 *
 * It's guest_init.c (read that first) plus one extra step: loading a kernel
 * module before opening the disk.
 *
 * Why the extra step
 * ------------------
 * observe_mmio.py attaches the disk with the virtio-mmio transport instead of
 * virtio-pci.  The guest runs your host's own kernel, and distribution
 * kernels usually build the virtio-pci driver in (=y) but ship the
 * virtio-mmio driver as a loadable module, virtio_mmio.ko (=m), because
 * ordinary PCs never need it.  Without that driver the guest can't see the
 * disk, so /dev/vda never appears.
 *
 * On a normal system, udev or an initramfs script would run `modprobe
 * virtio_mmio`.  Our initramfs has neither, so this program loads the module
 * itself with the finit_module() system call, which is what modprobe does in
 * the end.  observe_mmio.py copies the module file into the archive under
 * /modules.
 *
 * Talking to observe_mmio.py: as in guest_init.c, "START" and
 * "DONE elapsed_ms=<n>" on the serial console.  It also prints a
 * "loaded module ..." line, which shows up in the serial log.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* finit_module() flag: "the file is compressed (e.g. .ko.zst); decompress it
 * in the kernel".  Distribution kernels ship their modules compressed, and
 * the kernel can unpack them itself if built with CONFIG_MODULE_DECOMPRESS.
 * Defined in <linux/module.h>; repeated here in case the headers are old. */
#ifndef MODULE_INIT_COMPRESSED_FILE
#define MODULE_INIT_COMPRESSED_FILE 4
#endif

/* The guest's clock, in milliseconds.  CLOCK_MONOTONIC never jumps backwards
 * (unlike the wall clock), so it's the right one for measuring durations. */
static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Process 1 must never exit: if it does, the kernel panics.  So instead of
 * returning, sleep forever.  observe_mmio.py kills QEMU when it's done. */
static void sleep_forever(void) {
  for (;;)
    pause();
}

/* Load one module file, the way modprobe would.
 *
 * Module parameters: a module's settings can be given on the kernel command
 * line as "<module>.<param>=<value>", e.g. "virtio_mmio.device=512@0xfeb00e00:12".
 * For a module built into the kernel, the kernel applies these itself.  For
 * a loadable module it doesn't: modprobe reads them from /proc/cmdline and
 * hands them over when loading.  We do the same, collecting every
 * "<name>.x=y" word into "x=y ...".  (QEMU's microvm machine may put the
 * virtio-mmio device addresses there; see observe_mmio.py.) */
static void load_module(const char *path, const char *name, const char *cmdline) {
  char params[1024] = "";
  size_t n = strlen(name);
  char words[4096];
  snprintf(words, sizeof words, "%s", cmdline);
  for (char *w = strtok(words, " \n"); w; w = strtok(NULL, " \n")) {
    if (strncmp(w, name, n) == 0 && w[n] == '.' && strlen(params) + strlen(w) + 2 < sizeof params) {
      strcat(params, w + n + 1); /* skip "<name>." */
      strcat(params, " ");
    }
  }

  int fd = open(path, O_RDONLY | O_CLOEXEC);
  int flags = strstr(path, ".ko.") ? MODULE_INIT_COMPRESSED_FILE : 0; /* "x.ko.zst" etc. */
  /* glibc has no finit_module() wrapper, so call the system call directly. */
  if (fd < 0 || syscall(SYS_finit_module, fd, params, flags) != 0)
    printf("ERROR loading module %s: %m\n", path);
  else
    printf("loaded module %s %s\n", name, params);
  if (fd >= 0)
    close(fd);
}

static void list_dir(const char *path) {
  DIR *d = opendir(path);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char subpath[512];
    snprintf(subpath, sizeof subpath, "%s/%s", path, e->d_name);
    printf("=> %s, type=%d, size=%lld\n", subpath, e->d_type, (long long)e->d_reclen);
    if (e->d_type == DT_DIR)
      list_dir(subpath);
  }
  closedir(d);
}

/* List running process by reading /proc and printing each PID and it's process name */
static void list_running_process() {
  DIR *d = opendir("/proc");
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char subpath[512];
    char comm[256] = "";
    // open the /proc/<pid>/comm file to read the process
    if (!atol(e->d_name)) {
      continue;
    }
    snprintf(subpath, sizeof subpath, "/proc/%s/comm", e->d_name);
    FILE *f = fopen(subpath, "r");
    if (f) {
      if (fgets(comm, sizeof comm, f)) {
        size_t len = strlen(comm);
        if (len > 0 && comm[len - 1] == '\n')
          comm[len - 1] = '\0';
      }
      fclose(f);
    }
    printf("PID=%s, Name=%s\n", e->d_name, comm);
  }
  closedir(d);
}

/* Load every module file in /modules.  The module's name is its file name up
 * to the first '.': "virtio_mmio.ko.zst" -> "virtio_mmio". */
static void load_modules(const char *cmdline) {
  DIR *d = opendir("/modules");
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') /* skip "." and ".." */
      continue;
    char name[256], path[512];
    snprintf(name, sizeof name, "%.*s", (int)strcspn(e->d_name, "."), e->d_name);
    snprintf(path, sizeof path, "/modules/%s", e->d_name);
    load_module(path, name, cmdline);
  }
  closedir(d);
}

int main(void) {
  /* 1. Mount /proc and /dev (see guest_init.c). */
  mount("proc", "/proc", "proc", 0, NULL);
  mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

  /* 2. stdout and stderr to the serial console (see guest_init.c). */
  int console = open("/dev/console", O_WRONLY);
  if (console >= 0) {
    dup2(console, 1);
    dup2(console, 2);
  }
  setvbuf(stdout, NULL, _IONBF, 0);

  puts("Hello from the guest's init!");
  // list_dir("/");
  // list_running_process();

  /* 3. The kernel command line: "iters=N", plus any module parameters. */
  char cmdline[4096] = {0};
  FILE *f = fopen("/proc/cmdline", "r");
  if (f) {
    if (!fgets(cmdline, sizeof cmdline, f))
      cmdline[0] = '\0';
    fclose(f);
  }
  printf("kernel cmdline=%s\n", cmdline);

  long iters = 20000;
  char *p = strstr(cmdline, "iters=");
  if (p)
    iters = atol(p + strlen("iters="));

  /* 4. NEW: load the virtio-mmio driver.  As soon as it's loaded it finds the
   *    virtio-mmio device, hands it to the virtio-blk driver (built in), and
   *    /dev/vda appears. */
  load_modules(cmdline);

  /* 5. Open the disk, retrying for up to 10 s while the driver starts up.
   *    O_DIRECT: skip the guest's page cache, so every read reaches the
   *    device (see guest_init.c). */
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

  off_t size = lseek(fd, 0, SEEK_END);
  void *buf = aligned_alloc(4096, 4096);

  puts("START");
  sleep(1); /* give observe_mmio.py time to take its "before" snapshot */

  /* 6. The workload: one 4 KiB read at a time, so one kick per read. */
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
