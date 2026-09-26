#include "proc.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace kg {

int fork_tied(int sig) {
  const pid_t parent = getpid();
  pid_t pid = fork();
  if (pid == 0) {
    // The parent's handler for it, if it has one, is inherited until an exec
    // and is not the child's to run: a session's own handler unmounts the
    // session's game.
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    prctl(PR_SET_PDEATHSIG, sig);
    // The parent may have gone between the fork and the prctl, and then no
    // signal is coming: the child has been handed to another parent already.
    if (getppid() != parent) raise(sig);
  }
  return pid;
}

namespace {

// Milliseconds on a clock that a change of the wall clock does not move.
int64_t now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void fill(ProcResult& r, int st) {
  if (WIFEXITED(st)) {
    r.status = WEXITSTATUS(st);
  } else if (WIFSIGNALED(st)) {
    r.signalled = true;
    r.signal = WTERMSIG(st);
  }
}

}  // namespace

ProcResult run(const std::vector<std::string>& argv, const ProcOptions& opt) {
  ProcResult r;
  if (argv.empty()) return r;

  // Close-on-exec, so the only copies a program started here holds are its
  // own stdout and stderr: a second child started from another thread in the
  // meantime does not carry this one's pipe away with it.
  int pipefd[2] = {-1, -1};
  if (opt.capture && pipe2(pipefd, O_CLOEXEC) != 0) return r;

  pid_t pid = fork();
  if (pid < 0) {
    if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
    return r;
  }

  if (pid == 0) {
    if (opt.capture) {
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      // dup2 onto a different descriptor leaves close-on-exec off, but onto
      // the same one it does nothing at all. A parent with 1 or 2 closed gets
      // the pipe there from pipe2, close-on-exec and all, and the program
      // would start with that output closed.
      fcntl(STDOUT_FILENO, F_SETFD, 0);
      fcntl(STDERR_FILENO, F_SETFD, 0);
    }
    if (!opt.cwd.empty() && chdir(opt.cwd.c_str()) != 0) _exit(126);
    for (const auto& [k, v] : opt.env) setenv(k.c_str(), v.c_str(), 1);

    std::vector<char*> a;
    std::vector<std::string> owned = argv;
    a.reserve(owned.size() + 1);
    for (std::string& s : owned) a.push_back(s.data());
    a.push_back(nullptr);
    // A bare name is looked up on PATH; a path is used as given. execv alone
    // would fail with ENOENT for "fusermount3" and report nothing useful,
    // which is a remarkably quiet way for cleanup to stop happening.
    if (owned[0].find('/') == std::string::npos) {
      execvp(owned[0].c_str(), a.data());
    } else {
      execv(owned[0].c_str(), a.data());
    }
    _exit(127);
  }

  // Polled rather than timed with alarm(), so a caller that is already using
  // signals is not disturbed by us. The deadline covers the whole call.
  const int64_t deadline = opt.timeout_sec > 0 ? now_ms() + int64_t{opt.timeout_sec} * 1000 : -1;
  auto expired = [&] { return deadline >= 0 && now_ms() >= deadline; };
  int st = 0;
  bool reaped = false;

  if (opt.capture) {
    close(pipefd[1]);
    // The call is over when the child is, not when the pipe is. Whatever the
    // child starts inherits its stdout, and a program that leaves a daemon
    // behind - every Wine command leaves a wineserver and its services for a
    // few seconds - would otherwise keep the pipe open, and this call with
    // it, for as long as the daemon lives.
    //
    // So once the child is reaped the pipe is read until it ends, or until it
    // has nothing to say and the grace is over. Never cut while there is
    // something to read: what the child wrote before it exited is in the pipe
    // still, and a busy machine can take longer than any grace to get to it -
    // 7z -so leaves the last of a member there, and a hash of it that stops
    // short is a wrong hash reported as a good one.
    const int64_t grace = opt.grace_ms > 0 ? opt.grace_ms : 0;
    int64_t grace_until = -1;
    char buf[1 << 16];
    while (true) {
      if (!reaped) {
        const pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
          reaped = true;
          grace_until = now_ms() + grace;
        } else if (w < 0 && errno != EINTR) {
          close(pipefd[0]);
          return r;
        }
      }
      // The deadline is for the program; once it has gone, what is left in
      // the pipe is read whatever the time.
      if (!reaped && expired()) break;
      int wait_ms = 20;
      if (reaped) {
        const int64_t left = grace_until - now_ms();
        wait_ms = left <= 0 ? 0 : static_cast<int>(left < 10 ? left : 10);
      }
      pollfd pfd{pipefd[0], POLLIN, 0};
      const int n = poll(&pfd, 1, wait_ms);
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) {
        // Nothing to read: only now may the grace end the call.
        if (reaped && now_ms() >= grace_until) break;
        continue;
      }
      const ssize_t got = read(pipefd[0], buf, sizeof(buf));
      if (got > 0) {
        r.out.append(buf, static_cast<size_t>(got));
      } else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
        // Every writer is gone. Only the child is left to wait for.
        break;
      }
    }
    close(pipefd[0]);
  }

  while (!reaped) {
    const pid_t w = waitpid(pid, &st, deadline >= 0 ? WNOHANG : 0);
    if (w == pid) break;
    if (w < 0 && errno != EINTR) return r;
    if (expired()) {
      kill(pid, SIGKILL);
      waitpid(pid, &st, 0);
      r.signalled = true;
      r.signal = SIGKILL;
      return r;
    }
    if (w == 0) usleep(20000);
  }

  fill(r, st);
  return r;
}

}  // namespace kg
