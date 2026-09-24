// What the bootstrap's units share: the layout of the file they are part of,
// the structs that describe it, and the few functions one unit calls in
// another. Everything else stays static in the unit that owns it.
//
// The layout itself - trailers v2 to v4, the table of contents and its
// records - is written down in docs/file-format.md.
//
// Every function here is prefixed boot_. The bootstrap links statically
// against musl's libc.a, and a plain name such as join or run is one a libc
// could one day define.

#ifndef KRETRO_BOOT_H
#define KRETRO_BOOT_H

#include <stddef.h>
#include <sys/types.h>

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

/* clang-format would put each enumerator on a line of its own. */
/* clang-format off */
enum kind { K_TOOLS = 1, K_RUNTIME = 2, K_APP = 3, K_META = 4, K_PACK = 5, K_PLAYER_BASE = 6 };
/* clang-format on */

#define KEY_HEX 16 /* 8 bytes of the image hash, enough to name a cache dir */

/* Written into an unpacked runtime last, before it is renamed into place: an
 * unpack without it did not finish. Inside the tree rather than beside it, so
 * the rename that puts the tree in place is the one step that makes it whole. */
#define UNPACKED_MARK ".kretro-unpacked"

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

/* diag.c: stopping, and saying what is going on under KRETRO_DEBUG. */
void boot_die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void boot_damaged(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
int boot_verbose(void);
void boot_trace(const char *fmt, ...);

/* util.c: paths, little-endian fields, hex, and reading our own file. */
void boot_join(char *dst, size_t cap, const char *fmt, ...);
unsigned long long boot_rd_u64(const unsigned char *p);
unsigned int boot_rd_u32(const unsigned char *p);
void boot_hex(const unsigned char *in, int n, char *out);
int boot_mkdir_p(const char *path);
int boot_exists(const char *p);
char *boot_self_path(void);
void boot_read_exact(int fd, void *buf, size_t n, unsigned long long off);

/* layout.c: the trailer and the table of contents. */
void boot_read_layout(const char *self, struct layout *l);

/* io.c: copying payloads out of our own file. */
void boot_copy_range(const char *self, unsigned long long off, unsigned long long len, int fd,
                     const char *what);
void boot_extract_range(const char *self, unsigned long long off, unsigned long long len,
                        const char *out, mode_t mode);

/* proc.c: running another program and waiting for it. */
int boot_run(const char *file, char *const argv[], int *exec_err);

/* dirs.c: where things may be put, and run from. */
const char *boot_runtime_dir(void);
const char *boot_cache_dir(void);
int boot_noexec(const char *dir);
void boot_exec_dir(const char *const bases[], int n, const char *leaf, const char *what,
                   char *out, size_t cap);

/* unpack.c: the runtime unpacked rather than mounted. */
int boot_mounted_at(const char *dir);
int boot_unpacked_whole(const char *dir);
void boot_unpack_runtime(const char *self, const struct layout *l, const char *key,
                         struct tool *tool, char *root, size_t cap);

/* tool.c: the DwarFS tool, in memory or on disk. */
void boot_place_tool(const char *self, const struct payload *p, struct tool *t);
int boot_run_tool(const char *self, const struct payload *p, struct tool *t, char *argv[]);

/* userns.c: the fallback rung that is looked at and not taken. */
void boot_userns_probe(const char *where);

#endif
