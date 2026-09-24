// The runtime unpacked rather than mounted: telling a finished unpack from a
// cut-short one, unpacking somewhere of this run's own, and putting the
// result in place whole.

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "boot.h"

/* Whether a runtime at `dir` is a mount - a different device from the
 * directory it sits in - rather than a tree unpacked there. */
int boot_mounted_at(const char *dir) {
  char parent[4096];
  boot_join(parent, sizeof(parent), "%s/..", dir);
  struct stat rs, ps;
  return stat(dir, &rs) == 0 && stat(parent, &ps) == 0 && rs.st_dev != ps.st_dev;
}

int boot_unpacked_whole(const char *dir) {
  char m[4096];
  boot_join(m, sizeof(m), "%s/" UNPACKED_MARK, dir);
  return boot_exists(m);
}

static void mark_unpacked(const char *dir) {
  char m[4096];
  boot_join(m, sizeof(m), "%s/" UNPACKED_MARK, dir);
  int fd = open(m, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0 || close(fd) != 0) boot_die("cannot write %s: %s", m, strerror(errno));
}

static int writable_for_removal(const char *p, const struct stat *st, int type, struct FTW *f) {
  (void)f;
  /* A directory the image made read-only would keep its entries. */
  if (type == FTW_D && (st->st_mode & 0700) != 0700) chmod(p, (st->st_mode & 07777) | 0700);
  return 0;
}

static int remove_entry(const char *p, const struct stat *st, int type, struct FTW *f) {
  (void)st;
  (void)f;
  if (type == FTW_DP) rmdir(p);
  else unlink(p);
  return 0;
}

/* rm -rf, never following a link out of the tree. */
static void remove_tree(const char *p) {
  struct stat st;
  if (lstat(p, &st) != 0) return;
  if (S_ISDIR(st.st_mode)) nftw(p, writable_for_removal, 16, FTW_PHYS);
  nftw(p, remove_entry, 16, FTW_PHYS | FTW_DEPTH);
}

/* What unpacks of this runtime that were cut short left beside `root`:
 * <leaf>.<pid>.partial trees, <leaf>.<pid>.image copies and their temporaries,
 * and <leaf>.<pid>.stale trees that were being removed. Each is its run's
 * alone, so it is gone once that run is: a pid that no longer runs. */
static void sweep_leftovers(const char *root) {
  char dir[4096];
  boot_join(dir, sizeof(dir), "%s", root);
  char *slash = strrchr(dir, '/');
  if (!slash || slash == dir) return;
  *slash = 0;
  const char *leaf = slash + 1;
  size_t n = strlen(leaf);
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strncmp(de->d_name, leaf, n) != 0 || de->d_name[n] != '.') continue;
    char *end = NULL;
    long pid = strtol(de->d_name + n + 1, &end, 10);
    if (pid <= 0 || !end || *end != '.') continue;
    if (!(kill((pid_t)pid, 0) != 0 && errno == ESRCH)) continue;
    char p[4096];
    boot_join(p, sizeof(p), "%s/%s", dir, de->d_name);
    boot_trace("removing %s, left by an unpack that did not finish", p);
    remove_tree(p);
  }
  closedir(d);
}

/* Renames the finished unpack at `part` to `root`. Another run may have got
 * there first, which is as good; or `root` may hold an unpack from before
 * they were marked, whole or not, which is moved aside and removed. */
static void commit_tree(const char *part, const char *root) {
  for (int tries = 0; tries < 3; ++tries) {
    if (rename(part, root) == 0) return;
    int e = errno;
    if (e != ENOTEMPTY && e != EEXIST) {
      remove_tree(part);
      boot_die("cannot put the runtime at %s: %s", root, strerror(e));
    }
    if (boot_unpacked_whole(root)) {
      remove_tree(part);
      return;
    }
    char stale[4096];
    boot_join(stale, sizeof(stale), "%s.%d.stale", root, (int)getpid());
    if (rename(root, stale) == 0) remove_tree(stale);
  }
  remove_tree(part);
  boot_die("cannot put the runtime at %s: something keeps taking its place", root);
}

/* Finds or makes an unpacked runtime and writes where it is to `root`. Called
 * when the mount did not work; the runtime directory is used as well as the
 * cache, so an unpack already sitting in either is taken. */
void boot_unpack_runtime(const char *self, const struct layout *l, const char *key,
                         struct tool *tool, char *root, size_t cap) {
  /* The cache first, because it outlives a reboot. The runtime directory
   * after it, because a runtime extracted to a noexec cache is a runtime
   * whose loader cannot be run - and it is worth trying RAM before
   * giving up. An extraction already sitting in either is used. */
  const char *bases[] = {boot_cache_dir(), boot_runtime_dir()};
  char leaf[128], cache_root[4096];
  boot_join(leaf, sizeof(leaf), "rt-%s-%llx", key, l->image.off);
  cache_root[0] = 0;
  for (int i = 0; i < 2 && !cache_root[0]; ++i) {
    char probe[4096];
    boot_join(probe, sizeof(probe), "%s/kretro/%s", bases[i], leaf);
    char dir[4096];
    boot_join(dir, sizeof(dir), "%s/kretro", bases[i]);
    if (boot_unpacked_whole(probe) && !boot_noexec(dir)) boot_join(cache_root, sizeof(cache_root), "%s", probe);
  }
  if (!cache_root[0]) boot_exec_dir(bases, 2, leaf, "the runtime", cache_root, sizeof(cache_root));

  if (!boot_unpacked_whole(cache_root)) {
    if (!tool->path[0]) boot_die("no runtime tool available to unpack with");
    fprintf(stderr, "kretro: unpacking the runtime (first run only)...\n");
    sweep_leftovers(cache_root);

    /* Unpacked whole somewhere of this run's own, marked finished, and
     * only then renamed into place: the name appearing is the unpack
     * finishing. Unpacked straight into place, a Ctrl-C, a full disk or
     * a second player starting in the minute it takes left a tree whose
     * loader was there and whose libraries were not, and every run after
     * took the loader being there to mean the whole of it was. */
    char part[4096], image_tmp[4096];
    boot_join(part, sizeof(part), "%s.%d.partial", cache_root, (int)getpid());
    remove_tree(part);
    if (mkdir(part, 0755) != 0) boot_die("cannot create %s: %s", part, strerror(errno));

    /* dwarfsextract has no counterpart to the mount tool's imagesize
     * option, so it reads on past the end of the image and parses
     * whatever follows as a section header. Give it a file that is only
     * the image, this run's own: one shared name was removed by the
     * first run to finish while a second was still reading it. This
     * copy happens on the no-FUSE path alone, and once per machine. */
    boot_join(image_tmp, sizeof(image_tmp), "%s.%d.image", cache_root, (int)getpid());
    boot_extract_range(self, l->image.off, l->image.len, image_tmp, 0644);

    char *ex_argv[] = {NULL,         (char *)"--tool=dwarfsextract",
                       (char *)"-i", image_tmp,
                       (char *)"-O", (char *)"0",
                       (char *)"-o", part,
                       NULL};
    int erc = boot_run_tool(self, &l->tools, tool, ex_argv);
    unlink(image_tmp);
    char part_marker[4096];
    boot_join(part_marker, sizeof(part_marker), "%s/lib/ld-linux-x86-64.so.2", part);
    if (erc != 0 || !boot_exists(part_marker)) {
      remove_tree(part);
      boot_die("could not unpack the runtime into %s", cache_root);
    }
    mark_unpacked(part);
    commit_tree(part, cache_root);
  }
  boot_join(root, cap, "%s", cache_root);
}
