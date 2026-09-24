#include "proc.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
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

ProcResult run(const std::vector<std::string>& argv, const ProcOptions& opt) {
  ProcResult r;
  if (argv.empty()) return r;

  int pipefd[2] = {-1, -1};
  if (opt.capture && pipe(pipefd) != 0) return r;

  pid_t pid = fork();
  if (pid < 0) {
    if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
    return r;
  }

  if (pid == 0) {
    if (opt.capture) {
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);
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

  if (opt.capture) {
    close(pipefd[1]);
    char buf[1 << 14];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) r.out.append(buf, static_cast<size_t>(n));
    close(pipefd[0]);
  }

  int st = 0;
  if (opt.timeout_sec > 0) {
    // Poll rather than use alarm(), so a caller that is already using signals
    // is not disturbed by us.
    time_t deadline = time(nullptr) + opt.timeout_sec;
    while (true) {
      pid_t w = waitpid(pid, &st, WNOHANG);
      if (w == pid) break;
      if (w < 0) return r;
      if (time(nullptr) >= deadline) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        r.signalled = true;
        r.signal = SIGKILL;
        return r;
      }
      usleep(20000);
    }
  } else if (waitpid(pid, &st, 0) < 0) {
    return r;
  }

  if (WIFEXITED(st)) {
    r.status = WEXITSTATUS(st);
  } else if (WIFSIGNALED(st)) {
    r.signalled = true;
    r.signal = WTERMSIG(st);
  }
  return r;
}

}  // namespace kg
