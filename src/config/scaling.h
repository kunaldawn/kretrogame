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
  bool fullscreen = false;
  bool desktop_is_panel = false;  // Native: the virtual desktop is the panel
};

// Never zero, so a caller can divide by it without thinking.
uint32_t largest_fitting_scale(uint32_t game_w, uint32_t game_h, uint32_t panel_w,
                               uint32_t panel_h);

// The screen a game goes on. w,h is the whole panel, which the fullscreen
// modes (Native, Fit) cover. usable_w,usable_h is the work area, the part the
// desktop leaves to windows once its own panels and docks are drawn, and a
// windowed game (Integer) has to fit in it or the window manager squeezes the
// window. A usable size of zero means the same as the panel.
struct Panel {
  uint32_t w = 0, h = 0;
  uint32_t usable_w = 0, usable_h = 0;
};

// A panel w or h of zero means the panel size is unknown, in which case
// nothing is assumed: scale 1, windowed.
Geometry compute_geometry(uint32_t game_w, uint32_t game_h, const Panel& panel, const Display& d);

// The same with no work area known: the whole panel is usable.
Geometry compute_geometry(uint32_t game_w, uint32_t game_h, uint32_t panel_w, uint32_t panel_h,
                          const Display& d);

}  // namespace kg::config
