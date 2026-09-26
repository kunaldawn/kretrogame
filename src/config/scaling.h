// Getting a 1997 game onto a 2026 panel.
//
// We own the whole display chain, so this is arithmetic rather than
// negotiation: pick a whole-number scale, size the nested output to the game's
// resolution, and let Weston multiply. At an exact whole-number ratio every
// source pixel covers exactly N by N destination pixels, so the result is sharp
// whether the compositor filters or not.
//
// The one thing this cannot do is a smooth stretch to an arbitrary size:
// Weston's nested backends take a whole-number output scale only. `Fit`
// therefore means the largest whole number that fits, shown fullscreen and
// letterboxed - which for the low-resolution games this is for is the better picture
// anyway.
#pragma once

#include <cstdint>

#include "config.h"

namespace kg::config {

struct Geometry {
  uint32_t logical_w = 640;   // the size the game renders at
  uint32_t logical_h = 480;
  uint32_t scale = 1;         // whole-number multiplier applied by the compositor
  // The scale the game has as a window. The same as `scale` for a windowed
  // start; for a fullscreen one it is what the fullscreen toggle goes back to,
  // which has to fit the work area rather than the whole panel.
  uint32_t window_scale = 1;
  bool fullscreen = false;
  bool desktop_is_panel = false;  // Native: the virtual desktop is the panel
};

// Never zero, so a caller can divide by it without thinking.
uint32_t largest_fitting_scale(uint32_t game_w, uint32_t game_h, uint32_t panel_w,
                               uint32_t panel_h);

// The scale a fullscreen game is shown at on a screen of screen_w x
// screen_h: the largest whole number that fits, centred on black. The nested
// Weston works this out again itself from the size the host gives its
// fullscreen window, so the two must agree; this one is what kretro plans
// and reports with. Never zero.
uint32_t letterbox_scale(uint32_t game_w, uint32_t game_h, uint32_t screen_w, uint32_t screen_h);

// What a window's decoration adds to the game's size, in host pixels.
struct WindowFrame {
  uint32_t w = 0, h = 0;
};

// The frame the nested Weston draws around a game's window on a Wayland host.
// Its wayland backend decorates the window itself, with the theme in
// shared/cairo-util.c (theme_create: a 27 px title bar and 6 px borders), and
// declares as the window everything but the drop shadow (frame_input_rect).
// So the window the host places is the game plus 2 * 6 across and 27 + 6 down,
// and a scale that fits only the game into the work area can still leave the
// window taller than it. Fullscreen has no frame, and an X11 host's window
// manager draws its own decoration, which its work area already allows for;
// both get a zero frame.
WindowFrame weston_window_frame(bool wayland_host);

// The screen a game goes on. w,h is the whole panel, which the fullscreen
// modes (Native, Fit) cover. usable_w,usable_h is the work area, the part the
// desktop leaves to windows once its own panels and docks are drawn, and a
// windowed game (Integer) has to fit in it or the window manager squeezes the
// window. A usable size of zero means the same as the panel. `frame` is the
// decoration the window will have; a windowed scale fits the game plus it.
struct Panel {
  uint32_t w = 0, h = 0;
  uint32_t usable_w = 0, usable_h = 0;
  WindowFrame frame{};
};

// A panel w or h of zero means the panel size is unknown, in which case
// nothing is assumed: scale 1, windowed.
Geometry compute_geometry(uint32_t game_w, uint32_t game_h, const Panel& panel, const Display& d);

// The same with no work area known: the whole panel is usable.
Geometry compute_geometry(uint32_t game_w, uint32_t game_h, uint32_t panel_w, uint32_t panel_h,
                          const Display& d);

}  // namespace kg::config
