// Reading what our own file carries: the trailer at its end, and for v4 the
// table of contents the trailer points at. The only unit that hashes.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blake3.h"

#include "boot.h"

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
  l->version = boot_rd_u32(raw + 8);
  if (l->version != 4) {
    boot_die("this file was made by a newer kretro (layout %u); this one reads up to 4",
             l->version);
  }
  l->toc_off = boot_rd_u64(raw + 16);
  l->toc_len = boot_rd_u64(raw + 24);

  /* The table sits immediately before the trailer, so the trailer alone says
   * how long the whole file was when it was written. A download that lost its
   * middle, or a file with something appended, is caught here with both
   * numbers. A download that lost its end never gets this far: its trailer
   * went with it. */
  unsigned long long end = size - TRAILER_V4_SIZE;
  if (l->toc_off > ULLONG_MAX - TRAILER_V4_SIZE || l->toc_len > ULLONG_MAX - TRAILER_V4_SIZE -
                                                                   l->toc_off) {
    boot_damaged("This file is damaged: its table of contents is out of range.");
  }
  if (l->toc_off + l->toc_len != end) {
    boot_damaged("This file is damaged or incomplete (it is %llu bytes, it should be %llu).", size,
                 l->toc_off + l->toc_len + TRAILER_V4_SIZE);
  }
  if (l->toc_len < TOC_HEADER || l->toc_len > TOC_MAX ||
      (l->toc_len - TOC_HEADER) % TOC_RECORD != 0) {
    boot_damaged("This file is damaged: its table of contents is %llu bytes long, which no table is.",
                 l->toc_len);
  }

  unsigned char *toc = malloc((size_t)l->toc_len);
  if (!toc) boot_die("out of memory");
  boot_read_exact(fd, toc, (size_t)l->toc_len, l->toc_off);

  unsigned char sum[BLAKE3_OUT_LEN];
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, toc, (size_t)l->toc_len);
  blake3_hasher_finalize(&h, sum, sizeof(sum));
  if (memcmp(sum, raw + 32, 32) != 0) {
    boot_damaged("This file is damaged: its table of contents does not match its checksum.");
  }

  /* From here on the table is exactly what kretro wrote. What is wrong with it
   * now was written wrong, not damaged in transit - but the person holding it
   * can do the same thing about both. */
  if (memcmp(toc, TOC_MAGIC, 4) != 0) {
    boot_damaged("This file is damaged: its table of contents is not one.");
  }
  unsigned int tv = boot_rd_u32(toc + 4);
  if (tv != 1) boot_die("this file was made by a newer kretro (table %u); this one reads 1", tv);
  unsigned int count = boot_rd_u32(toc + 8);
  if ((unsigned long long)count * TOC_RECORD + TOC_HEADER != l->toc_len) {
    boot_damaged("This file is damaged: its table of contents lists %u entries in %llu bytes.", count,
                 l->toc_len);
  }

  int seen[K_PLAYER_BASE + 1] = {0};
  for (unsigned int i = 0; i < count; ++i) {
    const unsigned char *r = toc + TOC_HEADER + (size_t)i * TOC_RECORD;
    unsigned int k = boot_rd_u32(r);
    unsigned long long off = boot_rd_u64(r + 8), len = boot_rd_u64(r + 16);
    /* Payloads come after the bootstrap and before the table, each on a page
     * boundary: DwarFS mounts an image in place only when it is aligned. */
    if (off == 0 || off % PAYLOAD_ALIGN != 0 || off > l->toc_off || len > l->toc_off - off) {
      boot_damaged("This file is damaged: its %s (entry %u, %llu bytes at %llu) lies outside it.",
                   kind_name(k), i, len, off);
    }
    /* Kinds this bootstrap does not know are a newer kretro's business; they
     * were bounds-checked, which is all a reader that ignores them needs. */
    if (k < K_TOOLS || k > K_PLAYER_BASE || k == K_PACK) continue;
    if (seen[k]++) boot_damaged("This file is damaged: it lists its %s twice.", kind_name(k));
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

  if (l->tools.len == 0) boot_damaged("This file is damaged: it carries no DwarFS tool.");
  if (l->image.len == 0) boot_damaged("This file is damaged: it carries no runtime.");
  if (l->app.len == 0) boot_damaged("This file is damaged: it carries no program to run.");
}

void boot_read_layout(const char *self, struct layout *l) {
  memset(l, 0, sizeof(*l));
  int fd = open(self, O_RDONLY);
  if (fd < 0) boot_die("cannot open %s: %s", self, strerror(errno));
  struct stat st;
  if (fstat(fd, &st) != 0) boot_die("cannot stat %s: %s", self, strerror(errno));
  unsigned long long size = (unsigned long long)st.st_size;

  unsigned char raw[TRAILER_SIZE];
  if (size >= TRAILER_V4_SIZE) {
    boot_read_exact(fd, raw, TRAILER_V4_SIZE, size - TRAILER_V4_SIZE);
    if (memcmp(raw, TRAILER_MAGIC_V4, 8) == 0) {
      read_v4(fd, size, raw, l);
      close(fd);
      return;
    }
  }

  int v2 = 0, v3 = 0;
  if (size >= TRAILER_SIZE) {
    boot_read_exact(fd, raw, TRAILER_SIZE, size - TRAILER_SIZE);
    v2 = memcmp(raw, TRAILER_MAGIC_V2, 8) == 0;
    v3 = memcmp(raw, TRAILER_MAGIC_V3, 8) == 0;
  }
  close(fd);
  if (!v2 && !v3) {
    /* Every file this bootstrap is ever part of ends in a trailer, so a file
     * without one has lost its end - and with it the only record of how long
     * it should have been. A bare bootstrap straight out of the build lands
     * here too; it carries nothing until kretro-link appends to it. */
    boot_trace("no trailer in the last %d bytes", TRAILER_SIZE);
    boot_damaged("This file is damaged or incomplete (it is %llu bytes, and the end of it, which "
                 "says what it carries, is missing).", size);
  }

  l->version = boot_rd_u32(raw + 8);
  if (l->version != 2 && l->version != 3) {
    boot_die("runtime trailer version %u is not understood", l->version);
  }
  l->tools.off = boot_rd_u64(raw + 16);
  l->tools.len = boot_rd_u64(raw + 24);
  memcpy(l->tools.hash, raw + 32, 32);
  l->image.off = boot_rd_u64(raw + 64);
  l->image.len = boot_rd_u64(raw + 72);
  memcpy(l->image.hash, raw + 80, 32);
  l->app.off = boot_rd_u64(raw + 112);
  l->app.len = boot_rd_u64(raw + 120);
  memcpy(l->app.hash, raw + 128, 32);
  /* A v3 trailer's fourth slot, at raw + 160, names a game carried inside the
   * binary. It is no longer handed down (see main.c), so it is not read. */

  if (l->image.len == 0) boot_die("the runtime image is empty");
  if (l->app.len == 0) boot_die("this binary carries no kretro");
}
