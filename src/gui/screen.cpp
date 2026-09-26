#include "screen.h"

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "../rt/env.h"
#include "../util/env.h"

namespace kg::gui {

// Asked here, in the GUI, because this code already links SDL and the session
// layer deliberately does not.

namespace {

struct Area {
  uint32_t w = 0, h = 0, usable_w = 0, usable_h = 0;
};

// The first display as the video driver already up sees it. A driver that
// does not know a work area (Wayland has no protocol for one) answers with the
// whole desktop, which is what an unknown work area means anyway.
bool measure_current(Area& a) {
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) != 0 || dm.w <= 0 || dm.h <= 0) return false;
  a.w = static_cast<uint32_t>(dm.w);
  a.h = static_cast<uint32_t>(dm.h);
  a.usable_w = a.w;
  a.usable_h = a.h;
  SDL_Rect r;
  if (SDL_GetDisplayUsableBounds(0, &r) == 0 && r.w > 0 && r.h > 0) {
    a.usable_w = static_cast<uint32_t>(r.w) < a.w ? static_cast<uint32_t>(r.w) : a.w;
    a.usable_h = static_cast<uint32_t>(r.h) < a.h ? static_cast<uint32_t>(r.h) : a.h;
  }
  return true;
}

// Brings SDL's video up with `driver` (nullptr: SDL's own choice), measures,
// and puts it down again. Only while video is down: SDL runs one video driver
// at a time, and switching is only possible between two of these. The hint is
// set at override priority because SDL_VIDEODRIVER in the environment would
// otherwise win over it, and is reset afterwards so the GUI that comes up
// later in this process gets the driver it always got.
bool measure_with(const char* driver, Area& a) {
  if (driver) SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, driver, SDL_HINT_OVERRIDE);
  bool ok = false;
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
    ok = measure_current(a);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
  if (driver) SDL_ResetHint(SDL_HINT_VIDEODRIVER);
  return ok;
}

// Scales an X measure into the Wayland one's units. Under a Wayland host
// Weston's window is sized in the host's logical units, while the X server
// (Xwayland) may be scaled up to the monitor's real pixels. Only X knows the
// work area (_NET_WORKAREA), so its work area is taken over at the ratio of
// the two desktops, which is 1 unless Xwayland scales natively.
uint32_t rescale(uint32_t v, uint32_t to, uint32_t from) {
  if (from == 0) return to;
  uint64_t r = static_cast<uint64_t>(v) * to / from;
  return r > to ? to : static_cast<uint32_t>(r);
}

PanelSize measure_in_process() {
  const bool wayland = env_nonempty("WAYLAND_DISPLAY") != nullptr;
  const bool x11 = env_nonempty("DISPLAY") != nullptr;
  Area wl, x;
  const bool have_wl = wayland && measure_with("wayland", wl);
  const bool have_x = x11 && measure_with("x11", x);
  PanelSize ps;
  if (have_wl) {
    ps.w = wl.w;
    ps.h = wl.h;
    ps.usable_w = have_x ? rescale(x.usable_w, wl.w, x.w) : wl.w;
    ps.usable_h = have_x ? rescale(x.usable_h, wl.h, x.h) : wl.h;
  } else if (have_x && !wayland) {
    // Only on an X11 host. Under a Wayland host the X numbers can be in
    // Xwayland's scaled-up pixels, twice the desktop the game's window is
    // placed on; with no Wayland answer (a monitor switched off, say) the
    // panel is better unknown - the game then opens at 1x - than wrong.
    ps = {x.w, x.h, x.usable_w, x.usable_h};
  } else if (!wayland && !x11) {
    // Whatever SDL picks by itself, as it always was asked: kretro without a
    // display environment still gets SDL's answer.
    Area any;
    if (measure_with(nullptr, any)) ps = {any.w, any.h, any.usable_w, any.usable_h};
  }
  return ps;
}

// `$KRETRO_APP panel`, which measures in a process whose video is down. Unset
// or failed, false.
bool measure_by_helper(PanelSize& ps) {
  const char* app = env_nonempty("KRETRO_APP");
  if (!app) return false;
  rt::Env e = rt::make(nullptr);
  if (!e.valid()) return false;
  ProcOptions opt;
  opt.timeout_sec = 5;
  ProcResult r = rt::run(e, app, {"panel"}, opt);
  if (!r.ok()) return false;
  // stdout and stderr arrive together, and SDL may have something to say on
  // stderr, so the answer is the last line that reads as four numbers.
  std::istringstream lines(r.out);
  std::string line;
  bool found = false;
  while (std::getline(lines, line)) {
    unsigned w, h, uw, uh;
    char extra;
    if (std::sscanf(line.c_str(), "%u %u %u %u %c", &w, &h, &uw, &uh, &extra) == 4) {
      ps = {w, h, uw, uh};
      found = true;
    }
  }
  return found && ps.w > 0 && ps.h > 0;
}

}  // namespace

PanelSize desktop_size(bool require_display_env) {
  if (require_display_env && !env_nonempty("DISPLAY") && !env_nonempty("WAYLAND_DISPLAY")) return {};
  if (SDL_WasInit(SDL_INIT_VIDEO) == 0) return measure_in_process();

  // Video is up. When its driver is X11 under a Wayland host the units are
  // wrong, so another process is asked; it is measured now rather than
  // remembered from start-up, because the shelf outlives a change of monitor,
  // scale or dock.
  const char* driver = SDL_GetCurrentVideoDriver();
  const bool x_under_wayland =
      env_nonempty("WAYLAND_DISPLAY") && driver && std::strcmp(driver, "x11") == 0;
  PanelSize ps;
  if (x_under_wayland) {
    // An unanswered helper leaves the panel unknown rather than measured in
    // units the game's window is not placed in (see measure_in_process).
    measure_by_helper(ps);
    return ps;
  }
  // Right as it is on an X11 host.
  Area a;
  if (measure_current(a)) ps = {a.w, a.h, a.usable_w, a.usable_h};
  return ps;
}

int panel_main() {
  const PanelSize ps = measure_in_process();
  std::printf("%u %u %u %u\n", ps.w, ps.h, ps.usable_w, ps.usable_h);
  return 0;
}

}  // namespace kg::gui
