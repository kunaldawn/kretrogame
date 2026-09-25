// The size of the screen a game will be drawn on.
#pragma once

#include <cstdint>

namespace kg::gui {

// Measured in the units the nested Weston sizes its window in: the host's
// logical units under a Wayland host (weston --backend=wayland), X pixels
// under an X11 host (weston --backend=x11). Under a Wayland host with native
// Xwayland scaling the two differ, and asking X would overstate the panel.
struct PanelSize {
  uint32_t w = 0, h = 0;  // the whole desktop, which a fullscreen game covers
  // The work area: the desktop less the panels and docks the desktop keeps
  // for itself. A windowed game has to fit in it or the window manager
  // squeezes the window. The same as w,h when the desktop does not say.
  uint32_t usable_w = 0, usable_h = 0;
};

// The panel of the first display, or all zero when there is none. With
// require_display_env, a session with neither DISPLAY nor WAYLAND_DISPLAY set
// is taken to have no screen without asking SDL at all. The player asks that
// way and kretro does not, so the two differ under SDL's dummy driver.
//
// While SDL's video is down it is measured here, bringing video up with the
// driver that answers in the right units and down again. While it is up (the
// shelf, the launcher) a Wayland host is measured by `$KRETRO_APP panel`
// instead: SDL runs one video driver at a time, and the windows up are X11
// ones.
PanelSize desktop_size(bool require_display_env);

// The hidden `panel` command of both apps: measures as above and prints
// "W H UW UH", all 0 when there is no screen. Returns the exit status.
int panel_main();

}  // namespace kg::gui
