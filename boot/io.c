// Copying a range of our own file somewhere else: into a descriptor, or out
// to a file put in place whole.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "boot.h"

/* Copies a range of our own file into an open descriptor. */
void boot_copy_range(const char *self, unsigned long long off, unsigned long long len, int fd,
                     const char *what) {
  int in = open(self, O_RDONLY);
  if (in < 0) boot_die("cannot open %s: %s", self, strerror(errno));
  char buf[1 << 16];
  unsigned long long done = 0;
  while (done < len) {
    size_t want = sizeof(buf);
    if (len - done < want) want = (size_t)(len - done);
    ssize_t n = pread(in, buf, want, (off_t)(off + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) boot_die("short read while unpacking: %s", n < 0 ? strerror(errno) : "end of file");
    ssize_t w = write(fd, buf, (size_t)n);
    if (w != n) boot_die("short write to %s: %s", what, strerror(errno));
    done += (unsigned long long)n;
  }
  close(in);
}

/* Copies a range of our own file out to `out`, atomically enough that two
 * kretros starting together cannot see a half-written tool. */
void boot_extract_range(const char *self, unsigned long long off, unsigned long long len,
                        const char *out, mode_t mode) {
  char tmp[4096];
  boot_join(tmp, sizeof(tmp), "%s.%d.tmp", out, (int)getpid());

  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) boot_die("cannot write %s: %s", tmp, strerror(errno));
  boot_copy_range(self, off, len, fd, tmp);
  if (fchmod(fd, mode) != 0) boot_die("cannot chmod %s: %s", tmp, strerror(errno));
  if (close(fd) != 0) boot_die("cannot close %s: %s", tmp, strerror(errno));
  if (rename(tmp, out) != 0) {
    unlink(tmp);
    /* Another process got there first; that is a success, not a failure. */
    if (!boot_exists(out)) boot_die("cannot place %s: %s", out, strerror(errno));
  }
}
