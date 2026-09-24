// Where things go: the session's runtime directory, the cache, and which of
// them lets a program placed there run.

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "boot.h"

/* The first call without XDG_RUNTIME_DIR creates /tmp/.kretro-<uid>. The
 * buffer is static, and callers keep the pointer. */
const char *boot_runtime_dir(void) {
  const char *x = getenv("XDG_RUNTIME_DIR");
  if (x && *x) return x;
  /* No session runtime directory - a container, or a bare login. A per-user
   * directory under /tmp, because a plain /tmp/kretro collides with anything
   * else of that name, including the binary itself. It is private, and it must
   * be ours: kretro's own program is unpacked into it and then run, so a
   * directory another user made first is a directory another user can put a
   * program in. */
  static char buf[64];
  boot_join(buf, sizeof(buf), "/tmp/.kretro-%u", (unsigned)getuid());
  if (mkdir(buf, 0700) != 0 && errno != EEXIST) boot_die("cannot create %s: %s", buf, strerror(errno));
  struct stat st;
  if (lstat(buf, &st) != 0) boot_die("cannot stat %s: %s", buf, strerror(errno));
  if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
    boot_die("%s belongs to someone else; remove it, or set XDG_RUNTIME_DIR", buf);
  }
  if ((st.st_mode & 077) != 0 && chmod(buf, 0700) != 0) {
    boot_die("cannot make %s private: %s", buf, strerror(errno));
  }
  return buf;
}

const char *boot_cache_dir(void) {
  const char *c = getenv("XDG_CACHE_HOME");
  if (c && *c) return c;
  static char buf[4096];
  const char *h = getenv("HOME");
  /* No home - env -i, a service, a bare container. /tmp/.cache was the
   * stand-in, and what is found there is run: the DwarFS tool when memory
   * will not run it, the runtime's loader when FUSE will not mount it. Any
   * user can make /tmp/.cache first, and the key naming what is in it is in
   * every copy of the download. The runtime directory is ours, and checked. */
  if (!h || !*h) return boot_runtime_dir();
  snprintf(buf, sizeof(buf), "%s/.cache", h);
  return buf;
}

/* The mount point `dir` lives on: the highest ancestor still on the same
 * device. A person told "/home/me/.cache/kretro/tools-1234 is noexec" goes
 * looking for that path in fstab and does not find it; told "/home", they do. */
static void mount_of(const char *dir, char *out, size_t cap) {
  boot_join(out, cap, "%s", dir);
  struct stat st, up;
  if (stat(out, &st) != 0) return;
  char parent[4096];
  while (strcmp(out, "/") != 0) {
    boot_join(parent, sizeof(parent), "%s", out);
    char *s = strrchr(parent, '/');
    if (!s) return;
    if (s == parent) s[1] = 0;
    else *s = 0;
    if (stat(parent, &up) != 0 || up.st_dev != st.st_dev) return;
    boot_join(out, cap, "%s", parent);
  }
}

int boot_noexec(const char *dir) {
  struct statvfs sv;
  return statvfs(dir, &sv) == 0 && (sv.f_flag & ST_NOEXEC);
}

/* Picks the first of `bases` whose filesystem lets programs run, creates
 * `<base>/kretro/<leaf>` there, and writes that to `out`. With none left it
 * stops and names each noexec mount, because that is the one thing the person
 * reading can change and the only thing that would make this work. */
void boot_exec_dir(const char *const bases[], int n, const char *leaf, const char *what,
                   char *out, size_t cap) {
  char refused[2][4096];
  int nref = 0;
  for (int i = 0; i < n; ++i) {
    boot_join(out, cap, "%s/kretro/%s", bases[i], leaf);
    if (boot_mkdir_p(out) != 0) {
      boot_trace("cannot create %s: %s", out, strerror(errno));
      continue;
    }
    if (!boot_noexec(out)) return;
    boot_trace("%s is on a noexec filesystem", out);
    if (nref == 2) continue;
    mount_of(out, refused[nref], sizeof(refused[0]));
    /* The cache and the runtime dir on one mount is one mount to name. */
    if (nref == 0 || strcmp(refused[0], refused[nref]) != 0) ++nref;
  }
  if (nref == 0) boot_die("cannot place %s: none of its directories could be created", what);
  boot_die("cannot place %s anywhere it may run: %s%s%s %s mounted noexec. Remount %s without noexec, "
           "or point XDG_CACHE_HOME at a directory on a filesystem that allows programs.",
           what, refused[0], nref == 2 ? " and " : "", nref == 2 ? refused[1] : "",
           nref == 2 ? "are" : "is", nref == 2 ? "one of them" : "it");
}
