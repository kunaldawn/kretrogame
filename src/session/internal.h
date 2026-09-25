// What the session's own files share and nobody else may use. Everything here
// is in kg::session::detail, and this header is never included from outside
// src/session/.
#pragma once

#include <signal.h>

#include <filesystem>
#include <functional>
#include <string>

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

// The layers of the session that holds mounts, for release_on_signal
// (interrupt.cpp). A plain pointer, set and cleared by MountInterruptGuard.
extern Layers* g_active;
void release_on_signal(int sig);

// A session's hold on its mounts for signals (interrupt.cpp). Made before the
// first mount: it points g_active at `l` and hands SIGINT, SIGTERM and SIGHUP
// to release_on_signal. Its end closes the layers, clears g_active and puts
// back the handlers it found.
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
};

// Puts a backend decision into the prefix and the environment (backend.cpp).
void apply_backend(const rt::Env& e, rt::Env& we, const std::filesystem::path& prefix,
                   const backend::Plan& s);

}  // namespace kg::session::detail
