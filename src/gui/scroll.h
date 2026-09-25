// Smooth scrolling: a region that glides to where the wheel, the keys or the
// focus send it instead of jumping there, with a fade at each edge that has
// more beyond it.
//
// It adds no way of scrolling of its own. ImGui still decides where a region
// should be - a wheel notch, a touchpad's stroke, PageDown, the focus moving
// to an item out of view, a page's own SetScrollHereY - and the region then
// eases there over about 135 ms, starting on the very frame the input came
// in. A wheel turned several notches in a row adds them all up rather than
// restarting from where the glide had got to. Dragging the scrollbar is
// followed at once.
#pragma once

#include "imgui.h"
#include "palette.h"

namespace kg::gui {

struct ScrollOpts {
  // Which ways it scrolls. A horizontal one is a row that runs off the right,
  // as a carousel does.
  bool x = false, y = true;
  // The fade at an edge with more beyond it, in design pixels (0 for none),
  // and the colour it fades into: the colour behind the region.
  float edge_fade = 28;
  ImVec4 bg = kBg0;
  // The right stick scrolls it while the focus is in it or the pointer is
  // over it.
  bool stick = true;
  // Keeps the scrollbar hidden, for a row whose edges say it scrolls.
  bool no_scrollbar = false;
  // Keeps the style's window padding inside the region, which a child window
  // with no border otherwise drops: for a region as wide as the page.
  bool padded = false;
};

// A child window that scrolls smoothly: ImGui::BeginChild with the keyboard
// and the pad moving straight through it (flattened), `size` as BeginChild
// takes it and `id` its ID, so a page that swaps a BeginChild for this keeps
// every ID it had. Pair it with end_scroll whatever it returns.
bool begin_scroll(const char* id, ImVec2 size, const ScrollOpts& opts = ScrollOpts(), ImGuiWindowFlags flags = 0);
void end_scroll();

// The fades begin_scroll's regions draw, for any child window: a band of `bg`
// at each edge with more beyond it, `design_px` deep. Call it inside the
// window, after what it holds.
void edge_fades(const ImVec4& bg = kBg0, float design_px = 28, bool x = false, bool y = true);

// Scrolls the current window so that the last item is in view, gliding there
// when the window is a begin_scroll region: at `centre_ratio` of the way down
// (0 top, 0.5 middle, 1 bottom), or with a negative one just far enough.
void scroll_to_item(float centre_ratio = -1.0f);

// ---- the arithmetic, which needs no window ----------------------------------

// How far one wheel notch scrolls a region `inner` pixels tall in a font
// `font_px` high: five lines, and never more than two thirds of the region, as
// ImGui itself steps.
float wheel_step(float font_px, float inner);
// Where a region shown at `shown` is drawn next, gliding towards `target`
// over `dt` seconds: 95% of the way in about 135 ms, and exactly there once it
// is within half a pixel.
float scroll_glide(float shown, float target, float dt);
// The scroll ImGui's target for a region stands for: `target` is a position in
// the region's content, to be shown at `centre_ratio` of the way through the
// `view` pixels the region shows, and with `snap` above 0, a target within
// that distance of either end (0, or `content` long) goes all the way to it.
// Clamped to 0..max_scroll.
float scroll_from_target(float target, float centre_ratio, float snap, float view, float content, float max_scroll);

}  // namespace kg::gui
