#include "screen.h"

#include <SDL.h>

#include "../util/env.h"

namespace kg::gui {

// Asked here, in the GUI, because this code already links SDL and the session
// layer deliberately does not.
PanelSize desktop_size(bool require_display_env) {
  if (require_display_env && !env_nonempty("DISPLAY") && !env_nonempty("WAYLAND_DISPLAY")) return {};
  // SDL counts how often a subsystem is brought up, so bringing it up only
  // when it is down is the same, to SDL, as bringing it up and down every time.
  bool inited = SDL_WasInit(SDL_INIT_VIDEO) != 0;
  if (!inited && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return {};
  PanelSize ps;
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    ps.w = static_cast<uint32_t>(dm.w);
    ps.h = static_cast<uint32_t>(dm.h);
  }
  if (!inited) SDL_QuitSubSystem(SDL_INIT_VIDEO);
  return ps;
}

}  // namespace kg::gui
