#include <signal.h>
#include <unistd.h>

#include "../util/proc.h"
#include "internal.h"

namespace kg::session::detail {

// Set while a session holds mounts, so a signal can still release them.
// Interrupting a game is normal - a timeout, a Ctrl-C, closing the terminal -
// and mounts left behind silently demote every later session to the slower
// path, which is a confusing way to be punished for pressing Ctrl-C.
Layers* g_active = nullptr;

void release_on_signal(int sig) {
  const char* msg = "\nkretro: interrupted, releasing mounts\n";
  ssize_t ignored = ::write(2, msg, __builtin_strlen(msg));
  (void)ignored;
  if (g_active) {
    // The calls are spelled out here rather than made through unmount(): this
    // runs in a signal handler, and each line of it is meant to be read as
    // exactly the one thing it does.
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

namespace {
constexpr int kSignals[3] = {SIGINT, SIGTERM, SIGHUP};
}  // namespace

MountInterruptGuard::MountInterruptGuard(Layers& l) : l_(&l) {
  g_active = &l;
  struct sigaction sa {};
  sa.sa_handler = release_on_signal;
  sigemptyset(&sa.sa_mask);
  for (int i = 0; i < 3; ++i) sigaction(kSignals[i], &sa, &before_[i]);
}

MountInterruptGuard::~MountInterruptGuard() {
  close_layers(*l_);
  // Before the handlers go back: a signal in between still finds nothing
  // left to release, rather than a session that is half closed.
  g_active = nullptr;
  for (int i = 0; i < 3; ++i) sigaction(kSignals[i], &before_[i], nullptr);
}

}  // namespace kg::session::detail
