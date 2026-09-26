// What the session's own files share and nobody else may use. Everything here
// is in kg::session::detail, and this header is never included from outside
// src/session/, except by the unit tests that exercise it.
#pragma once

#include <signal.h>

#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "../backend/policy.h"
#include "../rt/env.h"
#include "layers.h"

namespace kg::session::detail {

// Asks `cond` up to `tries` times, `ms` apart, until it says yes.
bool wait_for(const std::function<bool()>& cond, int tries, int ms);

// The name in `dir` that matches `want` ignoring case, or empty.
std::string find_ci_in_dir(const std::filesystem::path& dir, const std::string& want);

// How a FUSE mount is taken down with fusermount3. Plain is `-u`, which a
// busy mount refuses; Lazy is `-u -z`, which detaches it anyway, even when
// its FUSE process is gone; PlainThenLazy tries the one and then the other.
enum class Unmount { Plain, PlainThenLazy, Lazy };
void unmount(const std::filesystem::path& mountpoint, Unmount how);

// The layers of the session that holds mounts, for the thread that releases
// them on a signal (interrupt.cpp). A plain pointer, set and cleared by
// MountInterruptGuard under its lock.
extern Layers* g_active;
// The handler MountInterruptGuard installs. It only writes the signal down
// for the guard's thread, which unmounts and exits with 128 + the signal: a
// handler may not fork or allocate, and the window's thread is still running
// while a session plays.
void wake_on_signal(int sig);

// A session's hold on its mounts for signals (interrupt.cpp). Made before the
// first mount: it points g_active at `l`, starts the thread that releases
// the mounts, and hands SIGINT, SIGTERM and SIGHUP to wake_on_signal. Its end
// closes the layers, clears g_active, puts back the handlers it found and
// stops the thread.
//
// Put back because a session is not always the whole of the process: the
// shelf and a player's launcher play a game and then carry on, and SDL's own
// handlers, which turn a SIGTERM into a quit the window can answer, were
// replaced for good by one that exits on the spot with nothing to release.
class MountInterruptGuard {
 public:
  explicit MountInterruptGuard(Layers& l);
  ~MountInterruptGuard();
  MountInterruptGuard(const MountInterruptGuard&) = delete;
  MountInterruptGuard& operator=(const MountInterruptGuard&) = delete;

 private:
  Layers* l_;
  // What SIGINT, SIGTERM and SIGHUP were handled by before, in that order.
  struct sigaction before_[3] {};
  std::thread watcher_;
};

// Whether the nested Weston captures the pointer on a click: when the session
// asks for it (CompositorOptions::pointer_capture) and the person has not
// turned it off. `setting` is their KRETRO_WESTON_CAPTURE, or nullptr when it
// is unset or empty; the backend takes "1" alone as on.
bool pointer_capture_on(bool asked, const char* setting);

// Puts a backend decision into the prefix and the environment (backend.cpp).
void apply_backend(const rt::Env& e, rt::Env& we, const std::filesystem::path& prefix,
                   const backend::Plan& s);

}  // namespace kg::session::detail
