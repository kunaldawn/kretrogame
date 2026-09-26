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

uint32_t letterbox_scale(uint32_t game_w, uint32_t game_h, uint32_t screen_w, uint32_t screen_h) {
  return largest_fitting_scale(game_w, game_h, screen_w, screen_h);
}

WindowFrame weston_window_frame(bool wayland_host) {
  // cairo-util.c's theme: t->titlebar_height = 27, t->width = 6.
  constexpr uint32_t title_bar = 27, border = 6;
  if (!wayland_host) return {};
  return {2 * border, title_bar + border};
}

Geometry compute_geometry(uint32_t game_w, uint32_t game_h, const Panel& panel, const Display& d) {
  Geometry g;
  g.logical_w = game_w ? game_w : 640;
  g.logical_h = game_h ? game_h : 480;

  const uint32_t panel_w = panel.w, panel_h = panel.h;
  const bool panel_known = panel_w > 0 && panel_h > 0;
  const uint32_t fits = letterbox_scale(g.logical_w, g.logical_h, panel_w, panel_h);
  // A window has only the work area, less its own frame; fullscreen has the
  // whole panel. Never less than a pixel, so that a frame larger than the work
  // area does not read as "unknown".
  auto less_frame = [](uint32_t area, uint32_t frame) { return area > frame ? area - frame : 1; };
  const uint32_t usable_w = less_frame(panel.usable_w ? panel.usable_w : panel_w, panel.frame.w);
  const uint32_t usable_h = less_frame(panel.usable_h ? panel.usable_h : panel_h, panel.frame.h);
  const uint32_t fits_window = largest_fitting_scale(g.logical_w, g.logical_h, usable_w, usable_h);

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
      g.window_scale = panel_known ? fits_window : 1;
      g.fullscreen = panel_known;
      break;

    case ScaleMode::Integer:
      if (d.scale == 0) {
        g.scale = panel_known ? fits_window : 1;
      } else {
        // An explicit choice is honoured, but not past what the work area
        // can show.
        g.scale = panel_known && d.scale > fits_window ? fits_window : d.scale;
      }
      if (g.scale < 1) g.scale = 1;
      g.window_scale = g.scale;
      break;
  }
  return g;
}

Geometry compute_geometry(uint32_t game_w, uint32_t game_h, uint32_t panel_w, uint32_t panel_h,
                          const Display& d) {
  return compute_geometry(game_w, game_h, Panel{panel_w, panel_h, 0, 0}, d);
}

}  // namespace kg::config
