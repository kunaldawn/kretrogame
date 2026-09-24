// Stopping, and saying what is going on. Every message the bootstrap prints
// starts here.

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "boot.h"

void boot_die(const char *fmt, ...) {
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
void boot_damaged(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("kretro: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputs(" Download it again.\n", stderr);
  va_end(ap);
  exit(1);
}

int boot_verbose(void) { return getenv("KRETRO_DEBUG") != NULL; }

void boot_trace(const char *fmt, ...) {
  if (!boot_verbose()) return;
  va_list ap;
  va_start(ap, fmt);
  fputs("kretro[boot]: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}
