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

// How long the output is still read once the child has exited. What it wrote
// before it exited is already in the pipe and takes no time to read; anything
// later is from a process it left behind.
constexpr int64_t kDrainMs = 100;

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
      // dup2 leaves the new descriptors without close-on-exec.
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
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
    int64_t drain_until = -1;
    char buf[1 << 14];
    while (true) {
      if (!reaped) {
        const pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
          reaped = true;
          drain_until = now_ms() + kDrainMs;
        } else if (w < 0 && errno != EINTR) {
          close(pipefd[0]);
          return r;
        }
      }
      if (reaped && now_ms() >= drain_until) break;
      if (!reaped && expired()) break;
      pollfd pfd{pipefd[0], POLLIN, 0};
      const int n = poll(&pfd, 1, reaped ? 10 : 20);
      if (n < 0 && errno != EINTR) break;
      if (n <= 0) continue;
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
    if (w == pid) {
      reaped = true;
      break;
    }
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
