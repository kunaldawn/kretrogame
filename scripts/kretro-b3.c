/* kretro-b3: the BLAKE3 of each file named, one hex line each, for
 * scripts/kretro-link.py.
 *
 * The v4 table of contents records a BLAKE3 for every payload, and the player
 * checks the runtime and every game against it. Python's hashlib has no BLAKE3,
 * and hashing a 400 MB runtime in pure Python takes minutes, so the linker asks
 * this instead: the same third_party/blake3 the C++ uses, built static so it
 * runs in either toolchain image. "-" reads standard input.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "blake3.h"

static int hash_one(const char *name) {
  FILE *f = strcmp(name, "-") == 0 ? stdin : fopen(name, "rb");
  if (!f) {
    fprintf(stderr, "kretro-b3: cannot open %s: %s\n", name, strerror(errno));
    return 1;
  }
  blake3_hasher h;
  blake3_hasher_init(&h);
  static unsigned char buf[1 << 20];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) blake3_hasher_update(&h, buf, n);
  int bad = ferror(f);
  if (f != stdin) fclose(f);
  if (bad) {
    fprintf(stderr, "kretro-b3: cannot read %s\n", name);
    return 1;
  }
  unsigned char out[BLAKE3_OUT_LEN];
  blake3_hasher_finalize(&h, out, sizeof(out));
  for (size_t i = 0; i < sizeof(out); ++i) printf("%02x", out[i]);
  printf("  %s\n", name);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: kretro-b3 FILE...\n");
    return 2;
  }
  int rc = 0;
  for (int i = 1; i < argc; ++i) rc |= hash_one(argv[i]);
  return rc;
}
