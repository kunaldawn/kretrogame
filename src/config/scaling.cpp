#include "scaling.h"

namespace kg::config {

uint32_t largest_fitting_scale(uint32_t game_w, uint32_t game_h, uint32_t panel_w,
                               uint32_t panel_h) {
  if (game_w == 0 || game_h == 0 || panel_w == 0 || panel_h == 0) return 1;
  uint32_t by_w = panel_w / game_w;
  uint32_t by_h = panel_h / game_h;
  uint32_t n = by_w < by_h ? by_w : by_h;
  return n < 1 ? 1 : n;
}

Geometry compute_geometry(uint32_t game_w, uint32_t game_h, uint32_t panel_w, uint32_t panel_h,
                          const Display& d) {
  Geometry g;
  g.logical_w = game_w ? game_w : 640;
  g.logical_h = game_h ? game_h : 480;

  const bool panel_known = panel_w > 0 && panel_h > 0;
  const uint32_t fits = largest_fitting_scale(g.logical_w, g.logical_h, panel_w, panel_h);

  switch (d.mode) {
    case ScaleMode::Native:
      if (panel_known) {
        g.logical_w = panel_w;
        g.logical_h = panel_h;
        g.desktop_is_panel = true;
        g.fullscreen = true;
      }
      g.scale = 1;
      break;

    case ScaleMode::Fit:
      g.scale = panel_known ? fits : 1;
      g.fullscreen = panel_known;
      break;

    case ScaleMode::Integer:
      if (d.scale == 0) {
        g.scale = panel_known ? fits : 1;
      } else {
        // An explicit choice is honoured, but not past what the panel can show.
        g.scale = panel_known && d.scale > fits ? fits : d.scale;
      }
      if (g.scale < 1) g.scale = 1;
      break;
  }
  return g;
}

}  // namespace kg::config
