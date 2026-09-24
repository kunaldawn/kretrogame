#include "preview.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "../../util/safe_names.h"

extern char** environ;

namespace kg::bundle {
namespace fs = std::filesystem;

namespace {

// Whether `s` names `root` or something under it anywhere in it - "/rt/lib"
// and "--path=/rt" do, "/rt-other" does not.
bool mentions(const std::string& s, const std::string& root) {
  if (root.empty()) return false;
  for (size_t at = s.find(root); at != std::string::npos; at = s.find(root, at + 1)) {
    size_t end = at + root.size();
    if (end == s.size() || s[end] == '/') return true;
  }
  return false;
}

// The directories a fresh account has, all inside the scratch HOME.
const std::pair<const char*, const char*> kHomeDirs[] = {
    {"XDG_CONFIG_HOME", ".config"},
    {"XDG_DATA_HOME", ".local/share"},
    {"XDG_CACHE_HOME", ".cache"},
    {"XDG_STATE_HOME", ".local/state"},
};

}  // namespace

std::vector<std::string> preview_env(const std::vector<std::string>& parent, const fs::path& runtime,
                                     const fs::path& scratch) {
  std::string rt = runtime.empty() ? std::string() : runtime.lexically_normal().string();
  while (rt.size() > 1 && rt.back() == '/') rt.pop_back();
  fs::path home = scratch / "home";

  std::vector<std::string> out;
  for (const std::string& kv : parent) {
    size_t eq = kv.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    std::string name = kv.substr(0, eq), value = kv.substr(eq + 1);
    // Everything kretro's bootstrap told kretro, and portable-mode state with it.
    if (name.rfind("KRETRO_", 0) == 0) continue;
    if (name == "HOME") continue;
    bool xdg_home = false;
    for (const auto& [var, rel] : kHomeDirs) xdg_home |= name == var;
    if (xdg_home) continue;

    // A path into kretro's runtime is a path into a mount that belongs to
    // kretro's process and that the player's runtime is not: a PATH or
    // XDG_DATA_DIRS keeps its other entries, anything else that names it goes.
    if (mentions(value, rt)) {
      std::string kept;
      bool any_other = false;
      size_t start = 0;
      while (start <= value.size()) {
        size_t colon = value.find(':', start);
        std::string part = value.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!mentions(part, rt)) {
          if (any_other) kept += ':';
          kept += part;
          any_other = true;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
      }
      if (!any_other || kept.empty()) continue;
      value = kept;
    }
    std::string entry = name + "=";
    entry += value;
    out.push_back(std::move(entry));
  }
  out.push_back("HOME=" + home.string());
  for (const auto& [var, rel] : kHomeDirs) out.push_back(std::string(var) + "=" + (home / rel).string());
  return out;
}

std::vector<std::string> current_env() {
  std::vector<std::string> out;
  for (char** e = environ; e && *e; ++e) out.emplace_back(*e);
  return out;
}

fs::path preview_scratch(const fs::path& cache, const std::string& id) {
  if (!kg::id_is_safe(id)) {
    throw std::runtime_error("the bundle id '" + id + "' is not a name a directory can have: fix it under Identity");
  }
  return cache / ("preview-" + id);
}

Preview::~Preview() { stop(); }

void Preview::start(const fs::path& file, const std::vector<std::string>& args, const std::vector<std::string>& env,
                    const fs::path& scratch) {
  stop();
  std::error_code ec;
  fs::remove_all(scratch, ec);
  fs::path home = scratch / "home";
  fs::create_directories(home, ec);
  if (ec) throw std::runtime_error("cannot make a scratch home at " + home.string() + ": " + ec.message());
  scratch_ = scratch;
  if (!fs::exists(file, ec)) throw std::runtime_error(file.string() + " is not there: build it first");

  int p[2];
  if (pipe2(p, O_CLOEXEC) != 0) throw std::runtime_error(std::string("cannot make a pipe: ") + std::strerror(errno));

  // Built before the fork: a child of a threaded program may only do what is
  // async-signal-safe, and allocating is not.
  std::vector<std::string> argv_s{file.string()};
  argv_s.insert(argv_s.end(), args.begin(), args.end());
  std::vector<char*> argv, envp;
  argv.reserve(argv_s.size() + 1);
  for (std::string& s : argv_s) argv.push_back(s.data());
  argv.push_back(nullptr);
  std::vector<std::string> env_s = env;
  envp.reserve(env_s.size() + 1);
  for (std::string& s : env_s) envp.push_back(s.data());
  envp.push_back(nullptr);
  std::string cwd = home.string();

  pid_t pid = fork();
  if (pid < 0) {
    ::close(p[0]);
    ::close(p[1]);
    throw std::runtime_error(std::string("cannot start the preview: ") + std::strerror(errno));
  }
  if (pid == 0) {
    // A group of its own, so Stop reaches the player and the game. What the
    // player starts in sessions of its own - Weston, Xwayland, the gamepad
    // helper, the screenshooter - is out of the group's reach, and ends
    // because it is tied to the player (kg::fork_tied), however that ends.
    setpgid(0, 0);
    int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) dup2(devnull, STDIN_FILENO);
    dup2(p[1], STDOUT_FILENO);
    dup2(p[1], STDERR_FILENO);
    if (chdir(cwd.c_str()) != 0) _exit(126);
    execve(argv[0], argv.data(), envp.data());
    static const char msg[] = "the preview could not be started: exec failed\n";
    ssize_t w = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)w;
    _exit(127);
  }
  ::close(p[1]);
  fd_ = p[0];
  fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
  pid_ = pid;
  started_ = true;
  status_ = 0;
  partial_.clear();
}

void Preview::reap(bool wait) {
  if (pid_ <= 0) return;
  int st = 0;
  pid_t w = waitpid(pid_, &st, wait ? 0 : WNOHANG);
  if (w == pid_) {
    status_ = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    pid_ = -1;
  } else if (w < 0 && errno == ECHILD) {
    pid_ = -1;
  }
}

std::vector<std::string> Preview::poll() {
  std::vector<std::string> lines;
  // Reaped before reading, so that once the child is seen to have exited,
  // everything it wrote is already in the pipe to be read below.
  reap(false);
  bool eof = false;
  if (fd_ >= 0) {
    char buf[4096];
    for (;;) {
      ssize_t n = ::read(fd_, buf, sizeof(buf));
      if (n > 0) { partial_.append(buf, static_cast<size_t>(n)); continue; }
      if (n == 0) eof = true;
      break;  // EAGAIN, or an error: either way, nothing more this frame
    }
  }
  size_t nl;
  while ((nl = partial_.find('\n')) != std::string::npos) {
    lines.push_back(partial_.substr(0, nl));
    partial_.erase(0, nl + 1);
  }
  // A FUSE helper the player left running may hold the pipe open after the
  // player itself is gone; the exit is the end of the preview either way.
  if (pid_ <= 0 && started_ && (eof || fd_ >= 0 || !scratch_.empty())) {
    if (!partial_.empty()) lines.push_back(partial_);
    partial_.clear();
    clean();
  }
  return lines;
}

void Preview::stop() {
  if (pid_ > 0) {
    kill(-pid_, SIGTERM);
    for (int i = 0; i < 30 && pid_ > 0; ++i) {
      reap(false);
      if (pid_ > 0) usleep(100 * 1000);
    }
    if (pid_ > 0) {
      kill(-pid_, SIGKILL);
      reap(true);
    }
  }
  clean();
}

void Preview::clean() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (!scratch_.empty()) {
    std::error_code ec;
    fs::remove_all(scratch_, ec);
    scratch_.clear();
  }
}

}  // namespace kg::bundle
