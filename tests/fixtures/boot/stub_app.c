// Stands in for kretro's app, or a player's, in tests/integration/test_boot.sh.
//
// It reports what the bootstrap handed down - its arguments and the KRETRO_*
// environment - one fact per line, so the test can grep for exactly one thing
// at a time. And it runs the DwarFS tool it was pointed at, because an app
// told /proc/self/fd/N is only helped if fd N survived two execs to reach it.

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *keys[] = {"KRETRO_SELF",       "KRETRO_TOC",     "KRETRO_RUNTIME",
                             "KRETRO_DWARFS",     "KRETRO_APP",     "KRETRO_MOUNT_MODE",
                             "KRETRO_STATE",      "KRETRO_BUNDLED_GAME", "KRETRO_GUI", NULL};

int main(int argc, char **argv) {
  printf("stub-app ran\n");
  for (int i = 1; i < argc; ++i) printf("arg%d=%s\n", i, argv[i]);
  for (int i = 0; keys[i]; ++i) {
    const char *v = getenv(keys[i]);
    if (v) printf("%s=%s\n", keys[i], v);
    else printf("%s unset\n", keys[i]);
  }
  fflush(stdout);

  const char *tool = getenv("KRETRO_DWARFS");
  if (tool) {
    pid_t pid = fork();
    if (pid == 0) {
      /* argv[0] as kretro's own code passes it: the path, with --tool= first. */
      freopen("/dev/null", "w", stdout);
      freopen("/dev/null", "w", stderr);
      execl(tool, tool, "--tool=mkdwarfs", "--help", (char *)NULL);
      _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    printf("dwarfs=%s\n", WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "ok" : "failed");
  }
  return 0;
}
