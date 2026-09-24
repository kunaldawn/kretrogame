// Whether an unprivileged user namespace could have mounted the runtime,
// found out only to say so under KRETRO_DEBUG.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "boot.h"

/* The second rung of the fallback chain, looked at and not taken.
 *
 * uruntime's order is FUSE, then an unprivileged user namespace with a direct
 * /dev/fuse mount, then extraction. The middle rung mounts the runtime fine;
 * it is everything after that it cannot carry:
 *
 *   - The mount exists only inside the namespace, so the app and all its
 *     children must run in it too - as a uid that holds CAP_SYS_ADMIN there,
 *     or the app's own pack mounts go back to fusermount3 and fail.
 *   - The app unmounts with `fusermount3 -u`. Inside the namespace the setuid
 *     bit does nothing (root is not mapped), so the host's fusermount3 runs
 *     as an ordinary user against a mount it did not make. Whether that
 *     unmounts depends on which fusermount3 the host has, which is exactly
 *     what cannot be relied on.
 *   - Every DwarFS daemon the app starts daemonizes, and a daemon keeps its
 *     namespace alive. When the app exits nothing is left to reap them: they
 *     would run on, invisible, holding mounts nobody can see. Stopping that
 *     needs the bootstrap to stay resident as a subreaper, or a PID namespace
 *     with the app as its init - which ignores SIGINT unless it installs a
 *     handler, and so changes what Ctrl-C means to every game.
 *   - Ubuntu 24.04 and later, where FUSE-less machines are likeliest to be
 *     met, create the namespace and then grant it no capabilities
 *     (kernel.apparmor_restrict_unprivileged_userns), so the rung fails
 *     there anyway.
 *
 * Doing it properly means an app that mounts and unmounts through the
 * bootstrap rather than through fusermount3, and a bootstrap that stays
 * resident. Until then, this only finds out whether the rung would have been
 * open, so KRETRO_DEBUG can say so, and extraction follows. */
void boot_userns_probe(const char *where) {
  if (!boot_verbose()) return;
  if (access("/dev/fuse", R_OK | W_OK) != 0) {
    boot_trace("no usable /dev/fuse (%s): no user-namespace mount either", strerror(errno));
    return;
  }
  pid_t pid = fork();
  if (pid < 0) return;
  if (pid == 0) {
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) _exit(1);
    char map[64];
    int fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) { ssize_t w = write(fd, "deny", 4); (void)w; close(fd); }
    int k = snprintf(map, sizeof(map), "%u %u 1\n", (unsigned)uid, (unsigned)uid);
    fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd < 0 || write(fd, map, (size_t)k) != k) _exit(2);
    close(fd);
    k = snprintf(map, sizeof(map), "%u %u 1\n", (unsigned)gid, (unsigned)gid);
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd < 0 || write(fd, map, (size_t)k) != k) _exit(2);
    close(fd);
    /* A tmpfs over our own mount point, in a namespace that dies with this
     * process: the cheapest mount that proves the capability is real. */
    if (mount("none", where, "tmpfs", 0, NULL) != 0) _exit(3);
    _exit(0);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) return;
  static const char *why[] = {"would work, but is not used (see userns_probe)",
                              "cannot be created", "cannot map our uid",
                              "is created without the right to mount (AppArmor?)"};
  int c = WEXITSTATUS(st);
  boot_trace("a user namespace %s", c < 4 ? why[c] : "probe failed");
}
