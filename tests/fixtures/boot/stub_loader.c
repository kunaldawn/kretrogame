// Stands in for the runtime's ld-linux in tests/integration/test_boot.sh.
//
// The bootstrap runs `<runtime>/lib/ld-linux-x86-64.so.2 --library-path DIRS
// APP ARGS...`. A real runtime is 350 MB and needs a build that takes an hour,
// so the test image carries this instead: it checks it was called the way the
// real loader is, and runs the app the way the real loader would.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 4 || strcmp(argv[1], "--library-path") != 0) {
    fprintf(stderr, "stub-loader: called as a loader is not\n");
    return 99;
  }
  execv(argv[3], argv + 3);
  perror("stub-loader: exec");
  return 98;
}
