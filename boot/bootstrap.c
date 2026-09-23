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

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "blake3.h"

/* v2 named three payloads and left sixteen bytes of the trailer unused. v3
 * spends exactly those sixteen on a fourth: an offset and a length for a game
 * carried inside the binary, so a copy of this file can be one file a friend
 * runs and plays. The trailer is the same size, and v2 trailers are still read
 * - binaries built before this change are still binaries. */
#define TRAILER_MAGIC_V2 "KRETROv2"
#define TRAILER_MAGIC_V3 "KRETROv3"
#define TRAILER_SIZE 176

/* v4 stops counting slots. A player carries any number of games, so the
 * trailer no longer lists payloads at all: it is 64 bytes that say where a
 * table of contents is and what that table hashes to, and the table lists the
 * payloads. A v4 trailer is looked for first, in the last 64 bytes; only when
 * it is not there are the last 176 read as v2 or v3. */
#define TRAILER_MAGIC_V4 "KRETROv4"
#define TRAILER_V4_SIZE 64
#define TOC_MAGIC "KTOC"
#define TOC_HEADER 16
#define TOC_RECORD 128
/* A table this long would list a hundred thousand payloads. Anything past it
 * is a damaged length field, and reading it would mean allocating whatever
 * the damage says. */
#define TOC_MAX (16u << 20)
#define PAYLOAD_ALIGN 4096

enum kind { K_TOOLS = 1, K_RUNTIME = 2, K_APP = 3, K_META = 4, K_PACK = 5, K_PLAYER_BASE = 6 };

#define KEY_HEX 16 /* 8 bytes of the image hash, enough to name a cache dir */

/* Linux 6.3 added vm.memfd_noexec, and with it a flag asking for an
 * executable memfd explicitly. musl's headers predate it. */
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

struct payload {
  unsigned long long off, len;
  unsigned char hash[32];
};

struct layout {
  unsigned int version; /* 2, 3 or 4 */
  struct payload tools, image;
  /* kretro's own binary rides as a separate payload rather than inside the
   * runtime image. It is a few megabytes against the runtime's hundreds, so
   * changing it relinks in seconds instead of repacking 1.7 GB - and shipping
   * an update to kretro need not move the runtime at all. */
  struct payload app;
  /* Zero unless this binary carries a game (v3 only). */
  unsigned long long game_off, game_len;
  /* Where the table is (v4 only), handed down so the app reads the same
   * table this code checked rather than finding and checking it again. */
  unsigned long long toc_off, toc_len;
  int is_player; /* v4 with a meta entry */
};

/* The DwarFS tool, wherever it ended up. `path` is what gets executed and what
 * the app is told; `fd` is the memfd behind it, or -1 when it is a file. */
struct tool {
  char path[4096];
  int fd;
};

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static int mkdir_p(const char *path);

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("kretro: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

/* The one message a damaged download gets, in the words the design gave it.
 * Everything that finds the file inconsistent ends here, before anything is
 * mounted or unpacked: a file that is missing its tail must not get as far as
 * running half of itself. */
static void damaged(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void damaged(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("kretro: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputs(" Download it again.\n", stderr);
  va_end(ap);
  exit(1);
}

static int verbose(void) { return getenv("KRETRO_DEBUG") != NULL; }

static void trace(const char *fmt, ...) {
  if (!verbose()) return;
  va_list ap;
  va_start(ap, fmt);
  fputs("kretro[boot]: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

/* Composes a path, refusing to continue if it would be truncated. Silently
 * shortening a path is how a bootstrap ends up execing the wrong file. */
static void join(char *dst, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(dst, cap, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= cap) die("path too long to build (%d bytes)", n);
}

static unsigned long long rd_u64(const unsigned char *p) {
  unsigned long long v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

static unsigned int rd_u32(const unsigned char *p) {
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
         ((unsigned int)p[3] << 24);
}

static void hex(const unsigned char *in, int n, char *out) {
  static const char *d = "0123456789abcdef";
  for (int i = 0; i < n; ++i) {
    out[i * 2] = d[in[i] >> 4];
    out[i * 2 + 1] = d[in[i] & 15];
  }
  out[n * 2] = 0;
}

/* mkdir -p, tolerating races with another kretro starting at the same moment */
static int mkdir_p(const char *path) {
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

static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

static char *self_path(void) {
  static char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) die("cannot read /proc/self/exe: %s", strerror(errno));
  buf[n] = 0;
  return buf;
}

static void read_exact(int fd, void *buf, size_t n, unsigned long long off) {
  size_t done = 0;
  while (done < n) {
    ssize_t r = pread(fd, (char *)buf + done, n - done, (off_t)(off + done));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) die("cannot read this file: %s", r < 0 ? strerror(errno) : "it ended early");
    done += (size_t)r;
  }
}

static const char *kind_name(unsigned int k) {
  switch (k) {
    case K_TOOLS: return "DwarFS tool";
    case K_RUNTIME: return "runtime";
    case K_APP: return "program";
    case K_META: return "bundle description";
    case K_PACK: return "game";
    case K_PLAYER_BASE: return "player";
    default: return "part";
  }
}

/* Checks the v4 table and fills `l` from it. The table's own hash is checked
 * and every entry must lie inside the file; the payloads themselves are not
 * hashed here. That is the app's job, done lazily and remembered per bundle
 * version - hashing a multi-gigabyte bundle on every start would make the
 * player take a minute to open, to catch damage the table's bounds and the
 * pack's own checks catch anyway. */
static void read_v4(int fd, unsigned long long size, const unsigned char *raw,
                    struct layout *l) {
  l->version = rd_u32(raw + 8);
  if (l->version != 4) {
    die("this file was made by a newer kretro (layout %u); this one reads up to 4",
        l->version);
  }
  l->toc_off = rd_u64(raw + 16);
  l->toc_len = rd_u64(raw + 24);

  /* The table sits immediately before the trailer, so the trailer alone says
   * how long the whole file was when it was written. A download that lost its
   * middle, or a file with something appended, is caught here with both
   * numbers. A download that lost its end never gets this far: its trailer
   * went with it. */
  unsigned long long end = size - TRAILER_V4_SIZE;
  if (l->toc_off > ULLONG_MAX - TRAILER_V4_SIZE || l->toc_len > ULLONG_MAX - TRAILER_V4_SIZE -
                                                                   l->toc_off) {
    damaged("This file is damaged: its table of contents is out of range.");
  }
  if (l->toc_off + l->toc_len != end) {
    damaged("This file is damaged or incomplete (it is %llu bytes, it should be %llu).", size,
            l->toc_off + l->toc_len + TRAILER_V4_SIZE);
  }
  if (l->toc_len < TOC_HEADER || l->toc_len > TOC_MAX ||
      (l->toc_len - TOC_HEADER) % TOC_RECORD != 0) {
    damaged("This file is damaged: its table of contents is %llu bytes long, which no table is.",
            l->toc_len);
  }

  unsigned char *toc = malloc((size_t)l->toc_len);
  if (!toc) die("out of memory");
  read_exact(fd, toc, (size_t)l->toc_len, l->toc_off);

  unsigned char sum[BLAKE3_OUT_LEN];
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, toc, (size_t)l->toc_len);
  blake3_hasher_finalize(&h, sum, sizeof(sum));
  if (memcmp(sum, raw + 32, 32) != 0) {
    damaged("This file is damaged: its table of contents does not match its checksum.");
  }

  /* From here on the table is exactly what kretro wrote. What is wrong with it
   * now was written wrong, not damaged in transit - but the person holding it
   * can do the same thing about both. */
  if (memcmp(toc, TOC_MAGIC, 4) != 0) {
    damaged("This file is damaged: its table of contents is not one.");
  }
  unsigned int tv = rd_u32(toc + 4);
  if (tv != 1) die("this file was made by a newer kretro (table %u); this one reads 1", tv);
  unsigned int count = rd_u32(toc + 8);
  if ((unsigned long long)count * TOC_RECORD + TOC_HEADER != l->toc_len) {
    damaged("This file is damaged: its table of contents lists %u entries in %llu bytes.", count,
            l->toc_len);
  }

  int seen[K_PLAYER_BASE + 1] = {0};
  for (unsigned int i = 0; i < count; ++i) {
    const unsigned char *r = toc + TOC_HEADER + (size_t)i * TOC_RECORD;
    unsigned int k = rd_u32(r);
    unsigned long long off = rd_u64(r + 8), len = rd_u64(r + 16);
    /* Payloads come after the bootstrap and before the table, each on a page
     * boundary: DwarFS mounts an image in place only when it is aligned. */
    if (off == 0 || off % PAYLOAD_ALIGN != 0 || off > l->toc_off || len > l->toc_off - off) {
      damaged("This file is damaged: its %s (entry %u, %llu bytes at %llu) lies outside it.",
              kind_name(k), i, len, off);
    }
    /* Kinds this bootstrap does not know are a newer kretro's business; they
     * were bounds-checked, which is all a reader that ignores them needs. */
    if (k < K_TOOLS || k > K_PLAYER_BASE || k == K_PACK) continue;
    if (seen[k]++) damaged("This file is damaged: it lists its %s twice.", kind_name(k));
    struct payload *p = k == K_TOOLS ? &l->tools : k == K_RUNTIME ? &l->image
                      : k == K_APP   ? &l->app   : NULL;
    if (p) {
      p->off = off;
      p->len = len;
      memcpy(p->hash, r + 24, 32);
    }
    if (k == K_META) l->is_player = 1;
  }
  free(toc);

  if (l->tools.len == 0) damaged("This file is damaged: it carries no DwarFS tool.");
  if (l->image.len == 0) damaged("This file is damaged: it carries no runtime.");
  if (l->app.len == 0) damaged("This file is damaged: it carries no program to run.");
}

static void read_layout(const char *self, struct layout *l) {
  memset(l, 0, sizeof(*l));
  int fd = open(self, O_RDONLY);
  if (fd < 0) die("cannot open %s: %s", self, strerror(errno));
  struct stat st;
  if (fstat(fd, &st) != 0) die("cannot stat %s: %s", self, strerror(errno));
  unsigned long long size = (unsigned long long)st.st_size;

  unsigned char raw[TRAILER_SIZE];
  if (size >= TRAILER_V4_SIZE) {
    read_exact(fd, raw, TRAILER_V4_SIZE, size - TRAILER_V4_SIZE);
    if (memcmp(raw, TRAILER_MAGIC_V4, 8) == 0) {
      read_v4(fd, size, raw, l);
      close(fd);
      return;
    }
  }

  int v2 = 0, v3 = 0;
  if (size >= TRAILER_SIZE) {
    read_exact(fd, raw, TRAILER_SIZE, size - TRAILER_SIZE);
    v2 = memcmp(raw, TRAILER_MAGIC_V2, 8) == 0;
    v3 = memcmp(raw, TRAILER_MAGIC_V3, 8) == 0;
  }
  close(fd);
  if (!v2 && !v3) {
    /* Every file this bootstrap is ever part of ends in a trailer, so a file
     * without one has lost its end - and with it the only record of how long
     * it should have been. A bare bootstrap straight out of the build lands
     * here too; it carries nothing until kretro-link appends to it. */
    trace("no trailer in the last %d bytes", TRAILER_SIZE);
    damaged("This file is damaged or incomplete (it is %llu bytes, and the end of it, which "
            "says what it carries, is missing).", size);
  }

  l->version = rd_u32(raw + 8);
  if (l->version != 2 && l->version != 3) {
    die("runtime trailer version %u is not understood", l->version);
  }
  l->tools.off = rd_u64(raw + 16);
  l->tools.len = rd_u64(raw + 24);
  memcpy(l->tools.hash, raw + 32, 32);
  l->image.off = rd_u64(raw + 64);
  l->image.len = rd_u64(raw + 72);
  memcpy(l->image.hash, raw + 80, 32);
  l->app.off = rd_u64(raw + 112);
  l->app.len = rd_u64(raw + 120);
  memcpy(l->app.hash, raw + 128, 32);
  l->game_off = v3 ? rd_u64(raw + 160) : 0;
  l->game_len = v3 ? rd_u64(raw + 168) : 0;

  if (l->image.len == 0) die("the runtime image is empty");
  if (l->app.len == 0) die("this binary carries no kretro");
}

/* Copies a range of our own file into an open descriptor. */
static void copy_range(const char *self, unsigned long long off, unsigned long long len, int fd,
                       const char *what) {
  int in = open(self, O_RDONLY);
  if (in < 0) die("cannot open %s: %s", self, strerror(errno));
  char buf[1 << 16];
  unsigned long long done = 0;
  while (done < len) {
    size_t want = sizeof(buf);
    if (len - done < want) want = (size_t)(len - done);
    ssize_t n = pread(in, buf, want, (off_t)(off + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) die("short read while unpacking: %s", n < 0 ? strerror(errno) : "end of file");
    ssize_t w = write(fd, buf, (size_t)n);
    if (w != n) die("short write to %s: %s", what, strerror(errno));
    done += (unsigned long long)n;
  }
  close(in);
}

/* Copies a range of our own file out to `out`, atomically enough that two
 * kretros starting together cannot see a half-written tool. */
static void extract_range(const char *self, unsigned long long off, unsigned long long len,
                          const char *out, mode_t mode) {
  char tmp[4096];
  join(tmp, sizeof(tmp), "%s.%d.tmp", out, (int)getpid());

  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) die("cannot write %s: %s", tmp, strerror(errno));
  copy_range(self, off, len, fd, tmp);
  if (fchmod(fd, mode) != 0) die("cannot chmod %s: %s", tmp, strerror(errno));
  if (close(fd) != 0) die("cannot close %s: %s", tmp, strerror(errno));
  if (rename(tmp, out) != 0) {
    unlink(tmp);
    /* Another process got there first; that is a success, not a failure. */
    if (!exists(out)) die("cannot place %s: %s", out, strerror(errno));
  }
}

/* Runs `file` with `argv` and waits. Returns its exit status, or -1 if it died
 * by a signal. When the exec itself failed, *exec_err says why, so a caller
 * can tell "the program ran and said no" from "the program could not be run"
 * - which for a tool held in memory is the difference between a missing FUSE
 * and a kernel that will not execute memory, and the two fall back
 * differently. The pipe carries that errno out of the child: it is closed by
 * a successful exec and written to by a failed one. */
static int run(const char *file, char *const argv[], int *exec_err) {
  int pfd[2];
  if (exec_err) *exec_err = 0;
  if (pipe2(pfd, O_CLOEXEC) != 0) die("pipe failed: %s", strerror(errno));
  /* Started with stdin or stdout closed, the pipe is handed those numbers -
   * and the child's dup2 of /dev/null over stdout would then close its end of
   * the pipe before the exec is tried. A failed exec would look like a
   * program that ran, and the tool would never move out of memory. */
  for (int i = 0; i < 2; ++i) {
    if (pfd[i] > STDERR_FILENO) continue;
    int moved = fcntl(pfd[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) die("pipe failed: %s", strerror(errno));
    close(pfd[i]);
    pfd[i] = moved;
  }
  pid_t pid = fork();
  if (pid < 0) die("fork failed: %s", strerror(errno));
  if (pid == 0) {
    close(pfd[0]);
    if (!verbose()) {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
    }
    /* execvp so a bare "fusermount3" is found on PATH; a path is used as given. */
    execvp(file, argv);
    int e = errno;
    ssize_t ignored = write(pfd[1], &e, sizeof(e));
    (void)ignored;
    _exit(127);
  }
  close(pfd[1]);
  int e = 0;
  ssize_t got;
  do got = read(pfd[0], &e, sizeof(e));
  while (got < 0 && errno == EINTR);
  close(pfd[0]);
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  if (got == (ssize_t)sizeof(e)) {
    if (exec_err) *exec_err = e ? e : EIO;
    return 127;
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *runtime_dir(void) {
  const char *x = getenv("XDG_RUNTIME_DIR");
  if (x && *x) return x;
  /* No session runtime directory - a container, or a bare login. A per-user
   * directory under /tmp, because a plain /tmp/kretro collides with anything
   * else of that name, including the binary itself. It is private, and it must
   * be ours: kretro's own program is unpacked into it and then run, so a
   * directory another user made first is a directory another user can put a
   * program in. */
  static char buf[64];
  join(buf, sizeof(buf), "/tmp/.kretro-%u", (unsigned)getuid());
  if (mkdir(buf, 0700) != 0 && errno != EEXIST) die("cannot create %s: %s", buf, strerror(errno));
  struct stat st;
  if (lstat(buf, &st) != 0) die("cannot stat %s: %s", buf, strerror(errno));
  if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
    die("%s belongs to someone else; remove it, or set XDG_RUNTIME_DIR", buf);
  }
  if ((st.st_mode & 077) != 0 && chmod(buf, 0700) != 0) {
    die("cannot make %s private: %s", buf, strerror(errno));
  }
  return buf;
}

static const char *cache_dir(void) {
  const char *c = getenv("XDG_CACHE_HOME");
  if (c && *c) return c;
  static char buf[4096];
  const char *h = getenv("HOME");
  /* No home - env -i, a service, a bare container. /tmp/.cache was the
   * stand-in, and what is found there is run: the DwarFS tool when memory
   * will not run it, the runtime's loader when FUSE will not mount it. Any
   * user can make /tmp/.cache first, and the key naming what is in it is in
   * every copy of the download. The runtime directory is ours, and checked. */
  if (!h || !*h) return runtime_dir();
  snprintf(buf, sizeof(buf), "%s/.cache", h);
  return buf;
}

/* The mount point `dir` lives on: the highest ancestor still on the same
 * device. A person told "/home/me/.cache/kretro/tools-1234 is noexec" goes
 * looking for that path in fstab and does not find it; told "/home", they do. */
static void mount_of(const char *dir, char *out, size_t cap) {
  join(out, cap, "%s", dir);
  struct stat st, up;
  if (stat(out, &st) != 0) return;
  char parent[4096];
  while (strcmp(out, "/") != 0) {
    join(parent, sizeof(parent), "%s", out);
    char *s = strrchr(parent, '/');
    if (!s) return;
    if (s == parent) s[1] = 0;
    else *s = 0;
    if (stat(parent, &up) != 0 || up.st_dev != st.st_dev) return;
    join(out, cap, "%s", parent);
  }
}

static int noexec(const char *dir) {
  struct statvfs sv;
  return statvfs(dir, &sv) == 0 && (sv.f_flag & ST_NOEXEC);
}

/* Picks the first of `bases` whose filesystem lets programs run, creates
 * `<base>/kretro/<leaf>` there, and writes that to `out`. With none left it
 * stops and names each noexec mount, because that is the one thing the person
 * reading can change and the only thing that would make this work. */
static void exec_dir(const char *const bases[], int n, const char *leaf, const char *what,
                     char *out, size_t cap) {
  char refused[2][4096];
  int nref = 0;
  for (int i = 0; i < n; ++i) {
    join(out, cap, "%s/kretro/%s", bases[i], leaf);
    if (mkdir_p(out) != 0) {
      trace("cannot create %s: %s", out, strerror(errno));
      continue;
    }
    if (!noexec(out)) return;
    trace("%s is on a noexec filesystem", out);
    if (nref == 2) continue;
    mount_of(out, refused[nref], sizeof(refused[0]));
    /* The cache and the runtime dir on one mount is one mount to name. */
    if (nref == 0 || strcmp(refused[0], refused[nref]) != 0) ++nref;
  }
  if (nref == 0) die("cannot place %s: none of its directories could be created", what);
  die("cannot place %s anywhere it may run: %s%s%s %s mounted noexec. Remount %s without noexec, "
      "or point XDG_CACHE_HOME at a directory on a filesystem that allows programs.",
      what, refused[0], nref == 2 ? " and " : "", nref == 2 ? refused[1] : "",
      nref == 2 ? "are" : "is", nref == 2 ? "one of them" : "it");
}

/* ---- the unpacked runtime ------------------------------------------------ */

/* Written into an unpacked runtime last, before it is renamed into place: an
 * unpack without it did not finish. Inside the tree rather than beside it, so
 * the rename that puts the tree in place is the one step that makes it whole. */
#define UNPACKED_MARK ".kretro-unpacked"

/* Whether a runtime at `dir` is a mount - a different device from the
 * directory it sits in - rather than a tree unpacked there. */
static int mounted_at(const char *dir) {
  char parent[4096];
  join(parent, sizeof(parent), "%s/..", dir);
  struct stat rs, ps;
  return stat(dir, &rs) == 0 && stat(parent, &ps) == 0 && rs.st_dev != ps.st_dev;
}

static int unpacked_whole(const char *dir) {
  char m[4096];
  join(m, sizeof(m), "%s/" UNPACKED_MARK, dir);
  return exists(m);
}

static void mark_unpacked(const char *dir) {
  char m[4096];
  join(m, sizeof(m), "%s/" UNPACKED_MARK, dir);
  int fd = open(m, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0 || close(fd) != 0) die("cannot write %s: %s", m, strerror(errno));
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
  join(dir, sizeof(dir), "%s", root);
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
    join(p, sizeof(p), "%s/%s", dir, de->d_name);
    trace("removing %s, left by an unpack that did not finish", p);
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
      die("cannot put the runtime at %s: %s", root, strerror(e));
    }
    if (unpacked_whole(root)) {
      remove_tree(part);
      return;
    }
    char stale[4096];
    join(stale, sizeof(stale), "%s.%d.stale", root, (int)getpid());
    if (rename(root, stale) == 0) remove_tree(stale);
  }
  remove_tree(part);
  die("cannot put the runtime at %s: something keeps taking its place", root);
}

/* Loads the DwarFS tool into an anonymous memory file. Nothing is written to
 * disk, so a noexec /tmp or cache stops nothing - and a noexec cache used to
 * stop the mount and the extraction together, since both need this tool. The
 * descriptor is deliberately inheritable: the app runs the same tool, to
 * mount games and to pack them, as /proc/self/fd/N, which only works in a
 * process that has fd N. Returns -1 when the kernel will not hand out an
 * executable memfd (vm.memfd_noexec=2, or a seal that forbids exec). */
static int tool_in_memory(const char *self, const struct payload *p, struct tool *t) {
  if (getenv("KRETRO_BOOT_NO_MEMFD")) {
    /* For tests, and for telling apart a problem with memfds from any other
     * on a machine that is misbehaving. */
    trace("memfd disabled by KRETRO_BOOT_NO_MEMFD");
    return -1;
  }
  int fd = memfd_create("dwarfs-universal", MFD_ALLOW_SEALING | MFD_EXEC);
  /* A kernel older than 6.3 does not know MFD_EXEC and says EINVAL; on those
   * every memfd is executable, so asking without it is asking for the same. */
  if (fd < 0 && errno == EINVAL) fd = memfd_create("dwarfs-universal", MFD_ALLOW_SEALING);
  if (fd < 0) {
    trace("memfd_create refused: %s", strerror(errno));
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
  copy_range(self, p->off, p->len, fd, "memory");
  /* Every process from here down holds this descriptor. Sealed, none of them
   * can rewrite the tool under the others. */
  if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
    trace("could not seal the tool's memfd: %s", strerror(errno));
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !(st.st_mode & S_IXUSR)) {
    trace("the memfd came back without exec permission");
    close(fd);
    return -1;
  }
  t->fd = fd;
  join(t->path, sizeof(t->path), "/proc/self/fd/%d", fd);
  trace("the dwarfs tool is in memory at %s", t->path);
  return 0;
}

/* The fallback for a kernel that will not run memory: a real file, in the
 * cache first because it survives a reboot, then the session runtime dir. */
static void tool_on_disk(const char *self, const struct payload *p, struct tool *t) {
  char key[KEY_HEX * 2 + 1], leaf[64], dir[4096];
  hex(p->hash, KEY_HEX / 2, key);
  join(leaf, sizeof(leaf), "tools-%s", key);
  const char *bases[] = {cache_dir(), runtime_dir()};
  exec_dir(bases, 2, leaf, "the DwarFS tool", dir, sizeof(dir));
  join(t->path, sizeof(t->path), "%s/dwarfs-universal", dir);
  if (!exists(t->path)) {
    trace("unpacking the dwarfs tool to %s", t->path);
    extract_range(self, p->off, p->len, t->path, 0755);
  }
  t->fd = -1;
}

static void place_tool(const char *self, const struct payload *p, struct tool *t) {
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
static int run_tool(const char *self, const struct payload *p, struct tool *t, char *argv[]) {
  if (!t->path[0]) return 127;
  argv[0] = (char *)"dwarfs-universal";
  int err = 0;
  int rc = run(t->path, argv, &err);
  if (err && t->fd >= 0) {
    trace("the kernel would not run the tool from memory (%s); using a file", strerror(err));
    close(t->fd);
    tool_on_disk(self, p, t);
    rc = run(t->path, argv, &err);
  }
  if (err) trace("cannot run %s: %s", t->path, strerror(err));
  return rc;
}

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
static void userns_probe(const char *where) {
  if (!verbose()) return;
  if (access("/dev/fuse", R_OK | W_OK) != 0) {
    trace("no usable /dev/fuse (%s): no user-namespace mount either", strerror(errno));
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
  trace("a user namespace %s", c < 4 ? why[c] : "probe failed");
}

int main(int argc, char **argv) {
  char *self = self_path();
  struct layout l;
  read_layout(self, &l);

  char key[KEY_HEX * 2 + 1];
  hex(l.image.hash, KEY_HEX / 2, key);

  char selfdir[4096];
  join(selfdir, sizeof(selfdir), "%s", self);
  char *slash = strrchr(selfdir, '/');
  if (slash) *slash = 0;
  if (l.is_player) {
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
    join(portable, sizeof(portable), "%s/kretro-data", selfdir);
    if (exists(portable) && !getenv("KRETRO_STATE")) {
      setenv("KRETRO_STATE", portable, 1);
      trace("portable mode: state in %s", portable);
    }
  }

  struct tool tool;
  place_tool(self, &l.tools, &tool);

  /* The mount point is under the session's runtime directory and never
   * beside the executable: Ubuntu 25.04 confines fusermount3 to mount points
   * under $HOME, /tmp and /run/user, and a player is as likely as not to be
   * run off a USB stick. */
  char root[4096];
  join(root, sizeof(root), "%s/kretro/rt-%s-%llx", runtime_dir(), key, l.image.off);
  char marker[4096];
  join(marker, sizeof(marker), "%s/lib/ld-linux-x86-64.so.2", root);
  const char *mode = "fusermount";

  /* A runtime already there is not always a mount. When the cache is noexec
   * the runtime is extracted under the runtime directory - to this very path,
   * since the extraction and the mount point are named alike - and every later
   * run finds it here. Called a mount, it would tell the app that FUSE works on
   * a machine where a game's pack cannot mount. A mount is a different device
   * from the directory it sits in; an extraction is not. */
  int ready = 0;
  if (exists(marker)) {
    if (mounted_at(root)) {
      ready = 1;
    } else if (unpacked_whole(root)) {
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
      trace("clearing a dead mount at %s", root);
      char *um_argv[] = {(char *)"fusermount3", (char *)"-u", (char *)"-z", root, NULL};
      run("fusermount3", um_argv, NULL);
    }
    if (mkdir_p(root) != 0) die("cannot create %s: %s", root, strerror(errno));

    char opts[192];
    snprintf(opts, sizeof(opts), "offset=%llu,imagesize=%llu,ro", l.image.off, l.image.len);

    /* Mount the runtime straight out of this binary. No copy, no temp file. */
    char *mount_argv[] = {NULL, (char *)"--tool=dwarfs", self, root, (char *)"-o", opts, NULL};
    int rc = run_tool(self, &l.tools, &tool, mount_argv);

    if (rc != 0 || !exists(marker) || !mounted_at(root)) {
      /* No FUSE, or it refused. Fall back to extracting once into the cache and
       * running from there: slower to start the first time, identical after. */
      trace("mount failed (rc=%d); extracting instead", rc);
      userns_probe(root);
      mode = "extract";

      /* The cache first, because it outlives a reboot. The runtime directory
       * after it, because a runtime extracted to a noexec cache is a runtime
       * whose loader cannot be run - and it is worth trying RAM before
       * giving up. An extraction already sitting in either is used. */
      const char *bases[] = {cache_dir(), runtime_dir()};
      char leaf[128], cache_root[4096];
      join(leaf, sizeof(leaf), "rt-%s-%llx", key, l.image.off);
      cache_root[0] = 0;
      for (int i = 0; i < 2 && !cache_root[0]; ++i) {
        char probe[4096];
        join(probe, sizeof(probe), "%s/kretro/%s", bases[i], leaf);
        char dir[4096];
        join(dir, sizeof(dir), "%s/kretro", bases[i]);
        if (unpacked_whole(probe) && !noexec(dir)) join(cache_root, sizeof(cache_root), "%s", probe);
      }
      if (!cache_root[0]) exec_dir(bases, 2, leaf, "the runtime", cache_root, sizeof(cache_root));

      if (!unpacked_whole(cache_root)) {
        if (!tool.path[0]) die("no runtime tool available to unpack with");
        fprintf(stderr, "kretro: unpacking the runtime (first run only)...\n");
        sweep_leftovers(cache_root);

        /* Unpacked whole somewhere of this run's own, marked finished, and
         * only then renamed into place: the name appearing is the unpack
         * finishing. Unpacked straight into place, a Ctrl-C, a full disk or
         * a second player starting in the minute it takes left a tree whose
         * loader was there and whose libraries were not, and every run after
         * took the loader being there to mean the whole of it was. */
        char part[4096], image_tmp[4096];
        join(part, sizeof(part), "%s.%d.partial", cache_root, (int)getpid());
        remove_tree(part);
        if (mkdir(part, 0755) != 0) die("cannot create %s: %s", part, strerror(errno));

        /* dwarfsextract has no counterpart to the mount tool's imagesize
         * option, so it reads on past the end of the image and parses
         * whatever follows as a section header. Give it a file that is only
         * the image, this run's own: one shared name was removed by the
         * first run to finish while a second was still reading it. This
         * copy happens on the no-FUSE path alone, and once per machine. */
        join(image_tmp, sizeof(image_tmp), "%s.%d.image", cache_root, (int)getpid());
        extract_range(self, l.image.off, l.image.len, image_tmp, 0644);

        char *ex_argv[] = {NULL,         (char *)"--tool=dwarfsextract",
                           (char *)"-i", image_tmp,
                           (char *)"-O", (char *)"0",
                           (char *)"-o", part,
                           NULL};
        int erc = run_tool(self, &l.tools, &tool, ex_argv);
        unlink(image_tmp);
        char part_marker[4096];
        join(part_marker, sizeof(part_marker), "%s/lib/ld-linux-x86-64.so.2", part);
        if (erc != 0 || !exists(part_marker)) {
          remove_tree(part);
          die("could not unpack the runtime into %s", cache_root);
        }
        mark_unpacked(part);
        commit_tree(part, cache_root);
      }
      join(root, sizeof(root), "%s", cache_root);
    }
  }

  trace("runtime at %s (%s)", root, mode);

  char loader[4096], libdir[8192], target[4096];
  join(loader, sizeof(loader), "%s/lib/ld-linux-x86-64.so.2", root);
  /* Several libraries keep their real payload in a private directory that is
   * on no standard search path: pulseaudio does it with libpulsecommon, Weston
   * with libexec_weston. Each one that is missed surfaces far away as a
   * "cannot open shared object file" from something unrelated, so they are
   * listed here rather than discovered one incident at a time. */
  join(libdir, sizeof(libdir),
       "%s/lib:%s/lib/x86_64-linux-gnu:%s/usr/lib/x86_64-linux-gnu:%s/usr/lib:%s/lib64:"
       "%s/usr/lib/x86_64-linux-gnu/pulseaudio:%s/usr/lib/x86_64-linux-gnu/weston:"
       "%s/usr/lib/x86_64-linux-gnu/libweston-14",
       root, root, root, root, root, root, root, root);
  /* Unpack the app, keyed by its own hash so a new build never reuses the
   * old one. It goes to disk, not to a memfd like the tool: it is tens of
   * megabytes held for the whole session, and helpers are started from it by
   * path. The runtime directory first, so it is gone after a reboot rather
   * than piling up one copy per build in the cache. */
  char app_key[KEY_HEX * 2 + 1], leaf[64], app_dir[4096];
  hex(l.app.hash, KEY_HEX / 2, app_key);
  join(leaf, sizeof(leaf), "app-%s", app_key);
  const char *app_bases[] = {runtime_dir(), cache_dir()};
  exec_dir(app_bases, 2, leaf, "the program", app_dir, sizeof(app_dir));
  join(target, sizeof(target), "%s/%s", app_dir, l.is_player ? "player" : "kretro");
  if (!exists(target)) extract_range(self, l.app.off, l.app.len, target, 0755);
  if (!exists(loader)) die("the runtime has no loader at %s", loader);

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
  if (!l.is_player && dev && *dev && exists(dev)) {
    join(target, sizeof(target), "%s", dev);
    trace("development override: %s", target);
  } else if (dev) {
    trace("KRETRO_GUI ignored: a player takes no development override");
  }
  unsetenv("KRETRO_GUI");

  setenv("KRETRO_RUNTIME", root, 1);
  /* How the runtime got there, for --doctor and for the app's own mounts:
   * a machine where the runtime had to be extracted is one where a game's
   * pack will not mount either. */
  setenv("KRETRO_MOUNT_MODE", mode, 1);
  /* Only the bootstrap knows where it put the DwarFS tool, and install needs
   * it to pack a game. Usually /proc/self/fd/N: callers must pass --tool=. */
  if (tool.path[0]) setenv("KRETRO_DWARFS", tool.path, 1);
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
  if (l.version == 4) {
    char toc[64];
    snprintf(toc, sizeof(toc), "%llu:%llu", l.toc_off, l.toc_len);
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

  /* The runtime's own libraries and nothing else. Host GPU directories are
   * appended later by the GPU probe, which needs the runtime to run at all. */
  char **args = calloc((size_t)argc + 4, sizeof(char *));
  if (!args) die("out of memory");
  int n = 0;
  args[n++] = loader;
  args[n++] = "--library-path";
  args[n++] = libdir;
  args[n++] = target;
  for (int i = 1; i < argc; ++i) args[n++] = argv[i];
  args[n] = NULL;

  execv(loader, args);
  die("cannot exec the runtime loader: %s", strerror(errno));
}
