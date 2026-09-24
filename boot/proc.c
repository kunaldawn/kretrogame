// Starting another program and waiting for it.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "boot.h"

/* Runs `file` with `argv` and waits. Returns its exit status, or -1 if it died
 * by a signal. When the exec itself failed, *exec_err says why, so a caller
 * can tell "the program ran and said no" from "the program could not be run"
 * - which for a tool held in memory is the difference between a missing FUSE
 * and a kernel that will not execute memory, and the two fall back
 * differently. The pipe carries that errno out of the child: it is closed by
 * a successful exec and written to by a failed one. */
int boot_run(const char *file, char *const argv[], int *exec_err) {
  int pfd[2];
  if (exec_err) *exec_err = 0;
  if (pipe2(pfd, O_CLOEXEC) != 0) boot_die("pipe failed: %s", strerror(errno));
  /* Started with stdin or stdout closed, the pipe is handed those numbers -
   * and the child's dup2 of /dev/null over stdout would then close its end of
   * the pipe before the exec is tried. A failed exec would look like a
   * program that ran, and the tool would never move out of memory. */
  for (int i = 0; i < 2; ++i) {
    if (pfd[i] > STDERR_FILENO) continue;
    int moved = fcntl(pfd[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) boot_die("pipe failed: %s", strerror(errno));
    close(pfd[i]);
    pfd[i] = moved;
  }
  pid_t pid = fork();
  if (pid < 0) boot_die("fork failed: %s", strerror(errno));
  if (pid == 0) {
    close(pfd[0]);
    if (!boot_verbose()) {
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
