// kretrogame bootstrap.
//
// This is the only code in the project that runs against an unknown libc
// environment, so it is static musl, plain POSIX, and deliberately dull. Its
// whole job is to make the bundled runtime reachable and then get out of the
// way:
//
//   1. find ourselves, and read the table of what our own file carries
//   2. load the bundled static DwarFS tool into memory and run it from there
//   3. mount the runtime image straight out of our own file, at an offset
//   4. exec the runtime's loader against the runtime's own libraries
//
// Step 4 is the point of the exercise: from there on nothing links the host's
// libc, which is what lets the same binary run on Ubuntu and on Alpine.
//
// When FUSE is unavailable the image is extracted to the cache instead. The
// user-visible behaviour is identical; only the first run costs disk.
//
// One source serves two programs. kretro, the builder, is this bootstrap with
// a runtime and kretro's app behind it. A player - what kretro builds and
// what gets shipped - is the same bootstrap with a player app, a bundle.meta
// and packs behind it. The bootstrap tells them apart only by whether the
// table lists a meta entry, and treats them differently in exactly one way:
// who decides where state lives.
//
// main() below is that sequence, one named step at a time; the units beside
// this file hold the machinery, and boot.h is all they share.

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boot.h"

/* Who decides where state lives: the one place kretro and a player differ. */
static void choose_state(const char *self, const struct layout *l) {
  char selfdir[4096];
  boot_join(selfdir, sizeof(selfdir), "%s", self);
  char *slash = strrchr(selfdir, '/');
  if (slash) *slash = 0;
  if (l->is_player) {
    /* A player's state is keyed by its bundle id, which is in bundle.meta and
     * not something this code reads. So the app decides - and an inherited
     * KRETRO_STATE is dropped, because the likeliest parent is kretro itself
     * previewing a player it built, and a player writing into the builder's
     * own state is precisely what a preview must not do. */
    unsetenv("KRETRO_STATE");
  } else {
    /* Portable mode: a kretro-data directory beside the binary wins over the
     * user's home, so a USB stick carrying the binary and a collection is
     * self-contained. */
    char portable[4096];
    boot_join(portable, sizeof(portable), "%s/kretro-data", selfdir);
    if (boot_exists(portable) && !getenv("KRETRO_STATE")) {
      setenv("KRETRO_STATE", portable, 1);
      boot_trace("portable mode: state in %s", portable);
    }
  }
}

/* Makes the runtime reachable - mounted, or unpacked when it will not mount -
 * writes where it is to `root`, and returns how it got there. */
static const char *find_runtime(char *self, const struct layout *l, const char *key,
                                struct tool *tool, char *root, size_t cap) {
  /* The mount point is under the session's runtime directory and never
   * beside the executable: Ubuntu 25.04 confines fusermount3 to mount points
   * under $HOME, /tmp and /run/user, and a player is as likely as not to be
   * run off a USB stick. */
  boot_join(root, cap, "%s/kretro/rt-%s-%llx", boot_runtime_dir(), key, l->image.off);
  char marker[4096];
  boot_join(marker, sizeof(marker), "%s/lib/ld-linux-x86-64.so.2", root);
  const char *mode = "fusermount";

  /* A runtime already there is not always a mount. When the cache is noexec
   * the runtime is extracted under the runtime directory - to this very path,
   * since the extraction and the mount point are named alike - and every later
   * run finds it here. Called a mount, it would tell the app that FUSE works on
   * a machine where a game's pack cannot mount. A mount is a different device
   * from the directory it sits in; an extraction is not. */
  int ready = 0;
  if (boot_exists(marker)) {
    if (boot_mounted_at(root)) {
      ready = 1;
    } else if (boot_unpacked_whole(root)) {
      ready = 1;
      mode = "extract";
    }
    /* Neither: an unpack that never finished, by a bootstrap from before
     * unpacks were made whole elsewhere and renamed into place. Its loader
     * being there says nothing about the rest; it is done again below. */
  }

  if (!ready) {
    /* A mount whose daemon died - killed, or the machine suspended badly -
     * answers every stat with ENOTCONN and every mount over it with EBUSY.
     * Lazily unmounted, the directory is a directory again. */
    struct stat st;
    if (stat(root, &st) != 0 && errno == ENOTCONN) {
      boot_trace("clearing a dead mount at %s", root);
      char *um_argv[] = {(char *)"fusermount3", (char *)"-u", (char *)"-z", root, NULL};
      boot_run("fusermount3", um_argv, NULL);
    }
    if (boot_mkdir_p(root) != 0) boot_die("cannot create %s: %s", root, strerror(errno));

    char opts[192];
    snprintf(opts, sizeof(opts), "offset=%llu,imagesize=%llu,ro", l->image.off, l->image.len);

    /* Mount the runtime straight out of this binary. No copy, no temp file. */
    char *mount_argv[] = {NULL, (char *)"--tool=dwarfs", self, root, (char *)"-o", opts, NULL};
    int rc = boot_run_tool(self, &l->tools, tool, mount_argv);

    if (rc != 0 || !boot_exists(marker) || !boot_mounted_at(root)) {
      /* No FUSE, or it refused. Fall back to extracting once into the cache and
       * running from there: slower to start the first time, identical after. */
      boot_trace("mount failed (rc=%d); extracting instead", rc);
      boot_userns_probe(root);
      mode = "extract";
      boot_unpack_runtime(self, l, key, tool, root, cap);
    }
  }
  return mode;
}

/* The runtime's loader, and the library path it is run with. */
static void runtime_paths(const char *root, char *loader, size_t loader_cap, char *libdir,
                          size_t libdir_cap) {
  boot_join(loader, loader_cap, "%s/lib/ld-linux-x86-64.so.2", root);
  /* Several libraries keep their real payload in a private directory that is
   * on no standard search path: pulseaudio does it with libpulsecommon, Weston
   * with libexec_weston. Each one that is missed surfaces far away as a
   * "cannot open shared object file" from something unrelated, so they are
   * listed here rather than discovered one incident at a time. */
  boot_join(libdir, libdir_cap,
            "%s/lib:%s/lib/x86_64-linux-gnu:%s/usr/lib/x86_64-linux-gnu:%s/usr/lib:%s/lib64:"
            "%s/usr/lib/x86_64-linux-gnu/pulseaudio:%s/usr/lib/x86_64-linux-gnu/weston:"
            "%s/usr/lib/x86_64-linux-gnu/libweston-14",
            root, root, root, root, root, root, root, root);
}

/* Puts the app where it can run and writes its path to `target`. */
static void place_app(const char *self, const struct layout *l, const char *loader, char *target,
                      size_t cap) {
  /* Unpack the app, keyed by its own hash so a new build never reuses the
   * old one. It goes to disk, not to a memfd like the tool: it is tens of
   * megabytes held for the whole session, and helpers are started from it by
   * path. The runtime directory first, so it is gone after a reboot rather
   * than piling up one copy per build in the cache. */
  char app_key[KEY_HEX * 2 + 1], leaf[64], app_dir[4096];
  boot_hex(l->app.hash, KEY_HEX / 2, app_key);
  boot_join(leaf, sizeof(leaf), "app-%s", app_key);
  const char *app_bases[] = {boot_runtime_dir(), boot_cache_dir()};
  boot_exec_dir(app_bases, 2, leaf, "the program", app_dir, sizeof(app_dir));
  boot_join(target, cap, "%s/%s", app_dir, l->is_player ? "player" : "kretro");
  if (!boot_exists(target)) boot_extract_range(self, l->app.off, l->app.len, target, 0755);
  if (!boot_exists(loader)) boot_die("the runtime has no loader at %s", loader);
}

static void apply_dev_override(const struct layout *l, char *target, size_t cap) {
  /* Development override. A release binary carries kretro-gui inside its
   * runtime image, but repacking 1.7 GB for every edit is not a build loop
   * anyone would use, so KRETRO_GUI points at a freshly built one. It is still
   * run through the runtime's loader against the runtime's libraries, so what
   * is being tested is the real arrangement.
   *
   * kretro's alone. A player is a stranger's download, and a variable left
   * set in a developer's shell - or handed down by the kretro that is
   * previewing the player - would swap its app for kretro-gui. And it is
   * taken out of the environment either way: whatever this starts, a player
   * included, must not see it. */
  const char *dev = getenv("KRETRO_GUI");
  if (!l->is_player && dev && *dev && boot_exists(dev)) {
    boot_join(target, cap, "%s", dev);
    boot_trace("development override: %s", target);
  } else if (dev) {
    boot_trace("KRETRO_GUI ignored: a player takes no development override");
  }
  unsetenv("KRETRO_GUI");
}

/* What the app is told, through its environment, about what was done here. */
static void hand_down(const char *self, const struct layout *l, const char *root,
                      const char *mode, const struct tool *tool, const char *target) {
  setenv("KRETRO_RUNTIME", root, 1);
  /* How the runtime got there, for --doctor and for the app's own mounts:
   * a machine where the runtime had to be extracted is one where a game's
   * pack will not mount either. */
  setenv("KRETRO_MOUNT_MODE", mode, 1);
  /* Only the bootstrap knows where it put the DwarFS tool, and install needs
   * it to pack a game. Usually /proc/self/fd/N: callers must pass --tool=. */
  if (tool->path[0]) setenv("KRETRO_DWARFS", tool->path, 1);
  else unsetenv("KRETRO_DWARFS");
  /* And where it put the app itself, so a session can start helpers without
   * guessing: /proc/self/exe is the loader here, not us. */
  setenv("KRETRO_APP", target, 1);

  /* Which file the user actually ran. By the time the app's own code runs, it
   * is under the runtime's loader and /proc/self/exe is that loader - so the
   * path has to be handed down. Packs, bundle.meta and the player base are all
   * read back out of it. */
  setenv("KRETRO_SELF", self, 1);

  /* Each of these is set for the file that carries it and cleared otherwise.
   * Cleared matters: a kretro starting a player it built hands down its own
   * environment, and a player must not take its parent's table or its
   * parent's game for its own. */
  if (l->version == 4) {
    char toc[64];
    snprintf(toc, sizeof(toc), "%llu:%llu", l->toc_off, l->toc_len);
    setenv("KRETRO_TOC", toc, 1);
  } else {
    unsetenv("KRETRO_TOC");
  }
  /* A v3 binary may still carry a game in its fourth slot - `kretro export
   * --standalone` made them - and it still runs, as the kretro it is. The game
   * is no longer handed down: the one-game player replaced that, and no app
   * built now looks for it. Cleared, so an old kretro's environment cannot
   * reach one that would. */
  unsetenv("KRETRO_BUNDLED_GAME");
}

static void exec_loader(char *loader, char *libdir, char *target, int argc, char **argv)
    __attribute__((noreturn));
static void exec_loader(char *loader, char *libdir, char *target, int argc, char **argv) {
  /* The runtime's own libraries and nothing else. Host GPU directories are
   * appended later by the GPU probe, which needs the runtime to run at all. */
  char **args = calloc((size_t)argc + 4, sizeof(char *));
  if (!args) boot_die("out of memory");
  int n = 0;
  args[n++] = loader;
  args[n++] = "--library-path";
  args[n++] = libdir;
  args[n++] = target;
  for (int i = 1; i < argc; ++i) args[n++] = argv[i];
  args[n] = NULL;

  execv(loader, args);
  boot_die("cannot exec the runtime loader: %s", strerror(errno));
}

int main(int argc, char **argv) {
  char *self = boot_self_path();
  struct layout l;
  boot_read_layout(self, &l);

  char key[KEY_HEX * 2 + 1];
  boot_hex(l.image.hash, KEY_HEX / 2, key);

  choose_state(self, &l);

  struct tool tool;
  boot_place_tool(self, &l.tools, &tool);

  char root[4096];
  const char *mode = find_runtime(self, &l, key, &tool, root, sizeof(root));
  boot_trace("runtime at %s (%s)", root, mode);

  char loader[4096], libdir[8192], target[4096];
  runtime_paths(root, loader, sizeof(loader), libdir, sizeof(libdir));
  place_app(self, &l, loader, target, sizeof(target));
  apply_dev_override(&l, target, sizeof(target));
  hand_down(self, &l, root, mode, &tool, target);
  exec_loader(loader, libdir, target, argc, argv);
}
