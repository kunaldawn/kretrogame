// The drawing primitives the widget files share: text measured in a font, thin
// bars on whole pixels, keycaps and the pad's buttons, and the few shapes the
// chrome draws instead of a glyph. Pages do not use these; they use the
// widgets built from them (widgets.h).
#pragma once

#include <string>
#include <vector>

#include "imgui.h"

namespace kg::gui {

// U+2026, when the font has it; three dots when it does not, as ImGui's own
// font does not.
const char* ellipsis(ImFont* f);
// How wide `b` (to `e`, or its end) is in `f` at the size it is laid out at.
float text_w(ImFont* f, const char* b, const char* e = nullptr);

// Where to draw a line of `f` so that its capitals sit in the middle of the
// band from `top`, `h` high. A font's line is taller than its letters, with
// the room for accents above them and for descenders below, so text centred
// on the font's height sits low in a chip; centred on a capital's ink, it
// sits where the eye expects.
float ink_centred_y(ImFont* f, float top, float h);

// A hairline, or any thin bar, as a filled rectangle on whole pixels. A line
// of thickness 1 drawn with AddLine straddles a pixel boundary, and the
// software renderer draws it as a stepped, half-covered smear.
float hairline();
// A rectangle's outline `t` pixels thick, drawn inside it as four filled bars
// for the same reason.
void frame_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t);
// A band shading from `top` colour at its top edge to `bottom` at its bottom,
// or from `left` to `right` across it.
void gradient_v(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom);
void gradient_h(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 left, ImU32 right);

// The part of a picture `tex_w` by `tex_h` that covers a box `w` by `h` of
// another shape without stretching it: all of it one way, and the middle of
// it the other, as `uv0`..`uv1`.
void cover_uv(float tex_w, float tex_h, float w, float h, ImVec2* uv0, ImVec2* uv1);

// `text` broken into the lines it wraps to at `wrap` pixels in `f`, each
// without the spaces at either end: for text drawn line by line, centred or
// cut short.
std::vector<std::string> wrap_lines(ImFont* f, const std::string& text, float wrap);

// How wide a keycap for `key` is in `f`; "arrows" is four triangles.
float keycap_w(ImFont* f, const std::string& key);
// A keycap for `key` at `pos`, in `f`, `h` high: `bg` behind, the window's
// own colour for the text. Returns its width.
float draw_keycap(ImDrawList* dl, ImFont* f, ImVec2 pos, float h, const std::string& key, ImU32 bg);

// The pad's buttons, as a console's hint bar draws them. `button` is one of
// "A", "B", "X", "Y" (a coloured disc with its letter), "LB", "RB", "Start"
// (a pill with its name), "LB RB" (the two pills), or "dpad" (a plus of four
// triangles). pad_glyph_w is how wide one is in `f` at height `h`.
float pad_glyph_w(ImFont* f, const std::string& button, float h);
float draw_pad_glyph(ImDrawList* dl, ImFont* f, ImVec2 pos, float h, const std::string& button, float alpha = 1.0f);

// Shapes drawn rather than taken from a font, so they are crisp at any scale
// and there whatever the font carries: a triangle pointing right (play) or
// left (back), in the box from `a` to `b`, and a downward arrow (install).
void draw_play_glyph(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col);
void draw_back_glyph(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col);
void draw_install_glyph(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col);

// The status bar begin_page draws for `title` and `hint` (status_bar.cpp).
void draw_status_bar(const char* title, const char* hint);
// The path the status bar shows after the tag, for `title`, and forgetting
// the one a page set for this frame.
std::string chrome_path(const char* title);
void clear_chrome_path();

}  // namespace kg::gui
