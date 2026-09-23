// BLAKE3 of stdin, as 64 hex digits. The builder image's Python has no BLAKE3,
// and the table of contents in a v4 file is checked with it, so the test's
// generator asks this.

#include <stdio.h>

#include "blake3.h"

int main(void) {
  blake3_hasher h;
  blake3_hasher_init(&h);
  unsigned char buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) blake3_hasher_update(&h, buf, n);
  if (ferror(stdin)) return 1;
  unsigned char out[BLAKE3_OUT_LEN];
  blake3_hasher_finalize(&h, out, sizeof(out));
  for (size_t i = 0; i < sizeof(out); ++i) printf("%02x", out[i]);
  printf("\n");
  return 0;
}
