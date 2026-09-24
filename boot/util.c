// Small helpers every other unit leans on: building paths, reading
// little-endian fields, hex, directories, and reading our own file.

#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "boot.h"

/* Composes a path, refusing to continue if it would be truncated. Silently
 * shortening a path is how a bootstrap ends up execing the wrong file. */
void boot_join(char *dst, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(dst, cap, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= cap) boot_die("path too long to build (%d bytes)", n);
}

unsigned long long boot_rd_u64(const unsigned char *p) {
  unsigned long long v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

unsigned int boot_rd_u32(const unsigned char *p) {
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
         ((unsigned int)p[3] << 24);
}

void boot_hex(const unsigned char *in, int n, char *out) {
  static const char *d = "0123456789abcdef";
  for (int i = 0; i < n; ++i) {
    out[i * 2] = d[in[i] >> 4];
    out[i * 2 + 1] = d[in[i] & 15];
  }
  out[n * 2] = 0;
}

/* mkdir -p, tolerating races with another kretro starting at the same moment */
int boot_mkdir_p(const char *path) {
  char buf[4096];
  size_t n = strlen(path);
  if (n >= sizeof(buf)) return -1;
  memcpy(buf, path, n + 1);
  for (char *p = buf + 1; *p; ++p) {
    if (*p != '/') continue;
    *p = 0;
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    *p = '/';
  }
  if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
}

int boot_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

/* The buffer is static: callers keep the pointer for the whole run. */
char *boot_self_path(void) {
  static char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) boot_die("cannot read /proc/self/exe: %s", strerror(errno));
  buf[n] = 0;
  return buf;
}

void boot_read_exact(int fd, void *buf, size_t n, unsigned long long off) {
  size_t done = 0;
  while (done < n) {
    ssize_t r = pread(fd, (char *)buf + done, n - done, (off_t)(off + done));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) boot_die("cannot read this file: %s", r < 0 ? strerror(errno) : "it ended early");
    done += (size_t)r;
  }
}
