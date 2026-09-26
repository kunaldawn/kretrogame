#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "../util/proc.h"
#include "internal.h"

namespace kg::session::detail {

// Set while a session holds mounts, so a signal can still release them.
// Interrupting a game is normal - a timeout, a Ctrl-C, closing the terminal -
// and mounts left behind silently demote every later session to the slower
// path, which is a confusing way to be punished for pressing Ctrl-C.
Layers* g_active = nullptr;

namespace {

constexpr int kSignals[3] = {SIGINT, SIGTERM, SIGHUP};

// What the pipe says when it is not a signal: the guard is ending.
constexpr unsigned char kStop = 0;

// Held by whatever reads or takes down g_active's mounts: the thread that
// releases them on a signal, and a guard closing them as it ends. A signal
// while the session is closing its layers waits for that, then finds nothing
// left to release.
std::mutex g_mu;

// The handler's only way of saying anything. Made once and never closed, so a
// handler still in its write as a guard ends cannot write into a descriptor
// that has since been closed and handed to something else. The write end does
// not block: a handler must not wait for anybody.
int g_wake[2] = {-1, -1};
std::once_flag g_wake_made;

// The process the guard's thread is in. A fork of it that has not reached its
// exec yet still has the handler, and not the thread.
std::atomic<pid_t> g_owner{0};

void make_wake_pipe() {
  if (pipe2(g_wake, O_CLOEXEC) != 0) {
    g_wake[0] = g_wake[1] = -1;
    return;
  }
  fcntl(g_wake[1], F_SETFL, O_NONBLOCK);
}

[[noreturn]] void release_and_exit(int sig) {
  std::lock_guard<std::mutex> lk(g_mu);
  const char* msg = "\nkretro: interrupted, releasing mounts\n";
  ssize_t ignored = ::write(2, msg, __builtin_strlen(msg));
  (void)ignored;
  if (g_active) {
    // The calls are spelled out here rather than made through unmount(), so
    // each line is read as exactly the one thing it does.
    // The game is still running with its working directory inside the mount,
    // so a plain unmount is refused as busy. The lazy form detaches anyway,
    // which is what we want: the mountpoint has to be usable next time even
    // though this session is being cut short.
    if (g_active->overlay_mounted) {
      kg::run({"fusermount3", "-u", g_active->merged.string()});
      kg::run({"fusermount3", "-u", "-z", g_active->merged.string()});
    }
    // Innermost first: the overlay sits on image/game, so the image cannot go
    // until the overlay has.
    if (g_active->image_mounted) {
      kg::run({"fusermount3", "-u", g_active->image.string()});
      kg::run({"fusermount3", "-u", "-z", g_active->image.string()});
    }
    g_active = nullptr;
  }
  _exit(128 + sig);
}

// The guard's thread: waits for the handler to say a signal came, and does
// what the handler may not - fork fusermount3, allocate, take a lock.
void watch(int fd) {
  for (;;) {
    unsigned char b = kStop;
    const ssize_t n = ::read(fd, &b, 1);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || b == kStop) return;
    release_and_exit(b);
  }
}

void say_to_watcher(unsigned char b) {
  for (;;) {
    if (::write(g_wake[1], &b, 1) == 1) return;
    if (errno != EINTR) return;
  }
}

}  // namespace

void wake_on_signal(int sig) {
  // Only what is safe in a handler. The session runs on a worker while the
  // window's thread goes on drawing, and a handler that forked, or allocated,
  // could land in the middle of a malloc on either and hang on its lock.
  const int saved = errno;
  if (getpid() != g_owner.load() || g_wake[1] < 0) _exit(128 + sig);
  const unsigned char b = static_cast<unsigned char>(sig);
  ssize_t ignored = ::write(g_wake[1], &b, 1);
  (void)ignored;
  errno = saved;
}

MountInterruptGuard::MountInterruptGuard(Layers& l) : l_(&l) {
  std::call_once(g_wake_made, make_wake_pipe);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_active = &l;
  }
  g_owner = getpid();
  if (g_wake[0] >= 0) watcher_ = std::thread(watch, g_wake[0]);
  struct sigaction sa {};
  sa.sa_handler = wake_on_signal;
  sigemptyset(&sa.sa_mask);
  // The handler returns now, into whatever it interrupted, and a waitpid for
  // the game that came back with EINTR would be taken for the game's end.
  sa.sa_flags = SA_RESTART;
  for (int i = 0; i < 3; ++i) sigaction(kSignals[i], &sa, &before_[i]);
}

MountInterruptGuard::~MountInterruptGuard() {
  // Before the handlers go back: a signal in between still finds nothing
  // left to release, rather than a session that is half closed.
  {
    std::lock_guard<std::mutex> lk(g_mu);
    close_layers(*l_);
    g_active = nullptr;
  }
  for (int i = 0; i < 3; ++i) sigaction(kSignals[i], &before_[i], nullptr);
  // After them: a signal that came before is ahead of the stop in the pipe,
  // and still ends the process as it was sent to.
  if (watcher_.joinable()) {
    say_to_watcher(kStop);
    watcher_.join();
  }
}

}  // namespace kg::session::detail
