// How big everything is drawn: one scale factor for the whole window, the
// fonts built for it, and the grid game tiles are laid out on.
//
// Every length a page draws is a design length - what it measures in a
// 1280x800 window at 100% - passed through px(). The window works the factor
// out each frame from its own size, the screen's density and the zoom the
// person chose, and rebuilds the fonts and the style when it changes. With no
// window at all, as in the tests, the factor is 1 and px() is the identity.
#pragma once

#include <optional>

#include "imgui.h"

namespace kg::gui {

// ---- the factor -------------------------------------------------------------

// The effective scale: 1.0 until a window sets it.
float ui_scale();
// A design length in pixels at the current scale.
float px(float design_px);
ImVec2 px(float x, float y);
// Sets the effective scale. The window calls it once the fonts and style are
// rebuilt for the new factor; tests and tools call it to lay a page out at a
// scale without a window. It rebuilds nothing.
void set_ui_scale(float s);

// The zoom the person chose with Ctrl +, Ctrl - and Ctrl 0 (or Ctrl and the
// mouse wheel): a multiplier on top of the automatic factor, 50% to 300% in
// steps of 10%, and never further than makes a difference: a step that the
// scale's own limits would swallow is not taken.
float ui_zoom();
void zoom_in();
void zoom_out();
void zoom_reset();
// The factor the zoom multiplies, which the window sets each frame, so that
// the zoom stops where the scale does.
void set_zoom_base(float base);

// KRETRO_UI_SCALE from the environment, when it is a sensible number: an
// absolute scale that replaces the automatic one, for a person whose screen
// the automatic factor gets wrong and for tools that take screenshots at a
// known size. Read once.
std::optional<float> forced_ui_scale();

// The automatic factor for a window of `logical_w` by `logical_h` (in SDL's
// window coordinates). It fits the 1280x800 design into the window, clamped
// to 0.75..3 so a small window and a television are both in proportion, and
// held at 1 for a window only a little short of the design. When the
// framebuffer is not scaled (`fb_scale` 1, as on most X11 desktops), the
// screen's density `ddpi` puts a floor under it, so text is never physically
// tiny on a dense screen in a small window, as far as the window still holds
// a 1024x640 design area.
float auto_ui_scale(float logical_w, float logical_h, float fb_scale, float ddpi);

// The factor rounded to sixteenths, so dragging a window's edge does not
// rebuild the fonts on every frame, and clamped to what the font atlas can
// hold.
float quantize_ui_scale(float s);

// ---- the fonts --------------------------------------------------------------

// The sizes of type the pages draw in, at scale 1: body 17, small 14 (the
// status bar, captions), big 28 bold (page titles), display 22 bold (a tile's
// name when it has no picture), hero 44 bold (a game's title over its art).
// All monospace.
enum class FontRole { Body,
                      Small,
                      Big,
                      Display,
                      Hero,
                      Count };
inline constexpr float kFontDesignPx[] = {17.0f, 14.0f, 28.0f, 22.0f, 44.0f};

// The font for a role, as the window last built it. Rebuilding the atlas for
// a new scale destroys every ImFont, so a font is asked for when it is used
// and never kept. Before a window has built any, this is ImGui's current
// font, which is what a test with only AddFontDefault gets.
ImFont* font(FontRole r);
// Set by the window after each rebuild; nullptr clears it.
void set_font(FontRole r, ImFont* f);
// The size text in `f` is laid out at, in window coordinates. On a HiDPI
// framebuffer the fonts are rasterised larger than they are laid out, so a
// font's own FontSize is not the size to draw it at.
float font_px(ImFont* f);

// A handle to the fonts that is always current: what a page keeps in place of
// ImFont pointers, which go stale the moment the scale changes.
struct Fonts {
  ImFont* body() const { return font(FontRole::Body); }
  ImFont* small() const { return font(FontRole::Small); }
  ImFont* big() const { return font(FontRole::Big); }
  ImFont* display() const { return font(FontRole::Display); }
  ImFont* hero() const { return font(FontRole::Hero); }
};

// ---- the tile grid ----------------------------------------------------------

// Columns of tiles that fill a row evenly: as many as fit at `min_w` or wider
// with `gap` between them, each then stretched to share the row. `h` is `w`
// times `aspect`. All in pixels, so a page passes px(280) and not 280. A
// negative `gap` is px(16).
struct Grid {
  int cols = 1;
  float w = 0, h = 0, gap = 0;
};
Grid tile_grid(float avail_w, float min_w, float aspect, float gap = -1.0f);

}  // namespace kg::gui
