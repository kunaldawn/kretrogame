// The bundled DwarFS tool: held in memory where the kernel allows it, on disk
// where it does not, and run from wherever it ended up.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boot.h"

/* Linux 6.3 added vm.memfd_noexec, and with it a flag asking for an
 * executable memfd explicitly. musl's headers predate it. */
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

/* Loads the DwarFS tool into an anonymous memory file. Nothing is written to
 * disk, so a noexec /tmp or cache stops nothing - and a noexec cache would
 * otherwise stop the mount and the extraction together, since both need this
 * tool. The
 * descriptor is deliberately inheritable: the app runs the same tool, to
 * mount games and to pack them, as /proc/self/fd/N, which only works in a
 * process that has fd N. Returns -1 when the kernel will not hand out an
 * executable memfd (vm.memfd_noexec=2, or a seal that forbids exec). */
static int tool_in_memory(const char *self, const struct payload *p, struct tool *t) {
  if (getenv("KRETRO_BOOT_NO_MEMFD")) {
    /* For tests, and for telling apart a problem with memfds from any other
     * on a machine that is misbehaving. */
    boot_trace("memfd disabled by KRETRO_BOOT_NO_MEMFD");
    return -1;
  }
  int fd = memfd_create("dwarfs-universal", MFD_ALLOW_SEALING | MFD_EXEC);
  /* A kernel older than 6.3 does not know MFD_EXEC and says EINVAL; on those
   * every memfd is executable, so asking without it is asking for the same. */
  if (fd < 0 && errno == EINVAL) fd = memfd_create("dwarfs-universal", MFD_ALLOW_SEALING);
  if (fd < 0) {
    boot_trace("memfd_create refused: %s", strerror(errno));
    return -1;
  }
  /* Started with stdin closed, the memfd would be fd 0 - and the app would
   * read its input from the DwarFS tool. */
  if (fd < 3) {
    int moved = fcntl(fd, F_DUPFD, 3);
    close(fd);
    if (moved < 0) return -1;
    fd = moved;
  }
  boot_copy_range(self, p->off, p->len, fd, "memory");
  /* Every process from here down holds this descriptor. Sealed, none of them
   * can rewrite the tool under the others. */
  if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
    boot_trace("could not seal the tool's memfd: %s", strerror(errno));
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !(st.st_mode & S_IXUSR)) {
    boot_trace("the memfd came back without exec permission");
    close(fd);
    return -1;
  }
  t->fd = fd;
  boot_join(t->path, sizeof(t->path), "/proc/self/fd/%d", fd);
  boot_trace("the dwarfs tool is in memory at %s", t->path);
  return 0;
}

/* The fallback for a kernel that will not run memory: a real file, in the
 * cache first because it survives a reboot, then the session runtime dir. */
static void tool_on_disk(const char *self, const struct payload *p, struct tool *t) {
  char key[KEY_HEX * 2 + 1], leaf[64], dir[4096];
  boot_hex(p->hash, KEY_HEX / 2, key);
  boot_join(leaf, sizeof(leaf), "tools-%s", key);
  const char *bases[] = {boot_cache_dir(), boot_runtime_dir()};
  boot_exec_dir(bases, 2, leaf, "the DwarFS tool", dir, sizeof(dir));
  boot_join(t->path, sizeof(t->path), "%s/dwarfs-universal", dir);
  if (!boot_exists(t->path)) {
    boot_trace("unpacking the dwarfs tool to %s", t->path);
    boot_extract_range(self, p->off, p->len, t->path, 0755);
  }
  t->fd = -1;
}

void boot_place_tool(const char *self, const struct payload *p, struct tool *t) {
  t->path[0] = 0;
  t->fd = -1;
  if (p->len == 0) return; /* a v2 or v3 binary may carry none */
  if (tool_in_memory(self, p, t) != 0) tool_on_disk(self, p, t);
}

/* Runs the DwarFS tool. argv[0] is always its own name: dwarfs-universal is a
 * multicall binary, and while --tool= picks the tool whatever it is called,
 * the mount tool also prints argv[0] in its messages, where "5" from
 * /proc/self/fd/5 would mean nothing to anyone. If the kernel refuses to run
 * the in-memory copy after all - an LSM can say no where the memfd flags said
 * yes - the tool moves to disk and the call is made again, and so is every
 * later one, including the app's. */
int boot_run_tool(const char *self, const struct payload *p, struct tool *t, char *argv[]) {
  if (!t->path[0]) return 127;
  argv[0] = (char *)"dwarfs-universal";
  int err = 0;
  int rc = boot_run(t->path, argv, &err);
  if (err && t->fd >= 0) {
    boot_trace("the kernel would not run the tool from memory (%s); using a file", strerror(err));
    close(t->fd);
    tool_on_disk(self, p, t);
    rc = boot_run(t->path, argv, &err);
  }
  if (err) boot_trace("cannot run %s: %s", t->path, strerror(err));
  return rc;
}
