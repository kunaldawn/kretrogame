// The size of the screen a game will be drawn on.
#pragma once

#include <cstdint>

namespace kg::gui {

struct PanelSize {
  uint32_t w = 0, h = 0;
};

// The desktop size of the first display, or 0x0 when there is none. SDL's video
// is brought up for the question and put down again, unless it was already up.
// With require_display_env, a session with neither DISPLAY nor WAYLAND_DISPLAY
// set is taken to have no screen without asking SDL at all. The player asks
// that way and kretro does not, so the two differ under SDL's dummy driver.
PanelSize desktop_size(bool require_display_env);

}  // namespace kg::gui
