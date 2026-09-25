#include "draw.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "palette.h"
#include "scale.h"

namespace kg::gui {

namespace {

// The "arrows" key is drawn as four triangles rather than as the four arrow
// characters: at the small font's size the arrows run together into a smudge,
// and a triangle is crisp at any scale. Its side, and the gap between two.
float arrow_side(ImFont* f) { return std::max(5.0f, std::round(font_px(f) * 0.6f)); }
float arrow_gap(ImFont* f) { return std::max(2.0f, std::round(arrow_side(f) * 0.4f)); }

}  // namespace

const char* ellipsis(ImFont* f) {
  return f && f->FindGlyphNoFallback(0x2026) ? "\xe2\x80\xa6" : "...";
}

float text_w(ImFont* f, const char* b, const char* e) {
  return f->CalcTextSizeA(font_px(f), FLT_MAX, 0.0f, b, e).x;
}

float ink_centred_y(ImFont* f, float top, float h) {
  const float size = font_px(f);
  const ImFontGlyph* g = f->FindGlyphNoFallback('H');
  if (!g || f->FontSize <= 0) return std::round(top + (h - size) * 0.5f);
  const float k = size / f->FontSize;
  return std::round(top + (h - (g->Y1 - g->Y0) * k) * 0.5f - g->Y0 * k);
}

float hairline() { return std::max(1.0f, std::round(px(1))); }

void frame_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t) {
  dl->AddRectFilled(a, ImVec2(b.x, a.y + t), col);
  dl->AddRectFilled(ImVec2(a.x, b.y - t), b, col);
  dl->AddRectFilled(ImVec2(a.x, a.y + t), ImVec2(a.x + t, b.y - t), col);
  dl->AddRectFilled(ImVec2(b.x - t, a.y + t), ImVec2(b.x, b.y - t), col);
}

void gradient_v(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) {
  if (b.x <= a.x || b.y <= a.y) return;
  dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
}

void gradient_h(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 left, ImU32 right) {
  if (b.x <= a.x || b.y <= a.y) return;
  dl->AddRectFilledMultiColor(a, b, left, right, right, left);
}

void cover_uv(float tex_w, float tex_h, float w, float h, ImVec2* uv0, ImVec2* uv1) {
  *uv0 = ImVec2(0, 0);
  *uv1 = ImVec2(1, 1);
  if (tex_w <= 0 || tex_h <= 0 || w <= 0 || h <= 0) return;
  const float ta = tex_w / tex_h, ba = w / h;
  if (ta > ba) {
    const float cut = (1.0f - ba / ta) * 0.5f;
    uv0->x = cut;
    uv1->x = 1.0f - cut;
  } else {
    const float cut = (1.0f - ta / ba) * 0.5f;
    uv0->y = cut;
    uv1->y = 1.0f - cut;
  }
}

std::vector<std::string> wrap_lines(ImFont* f, const std::string& text, float wrap) {
  std::vector<std::string> out;
  const float scale = font_px(f) / f->FontSize;
  const char* s = text.c_str();
  const char* end = s + text.size();
  while (s < end) {
    const char* eol = f->CalcWordWrapPositionA(scale, s, end, std::max(wrap, 1.0f));
    // A wrap width narrower than one character still moves on by one.
    if (eol == s) eol = s + 1;
    std::string line(s, eol);
    const size_t nl = line.find('\n');
    if (nl != std::string::npos) {
      eol = s + nl;
      line.resize(nl);
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    out.push_back(line);
    s = eol;
    while (s < end && *s == ' ') ++s;
    if (s < end && *s == '\n') ++s;
  }
  return out;
}

float keycap_w(ImFont* f, const std::string& key) {
  const float pad = std::round(px(5));
  if (key == "arrows") return std::round(arrow_side(f) * 4 + arrow_gap(f) * 3 + pad * 2);
  return std::round(text_w(f, key.c_str()) + pad * 2);
}

float draw_keycap(ImDrawList* dl, ImFont* f, ImVec2 pos, float h, const std::string& key, ImU32 bg) {
  const float pad = std::round(px(5));
  const float w = keycap_w(f, key);
  dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bg, px(2));
  const ImU32 ink = u32(kBg0);
  if (key == "arrows") {
    // Left, up, down, right: the order the hint's arrows were always in.
    const float s = arrow_side(f), g = arrow_gap(f), r = s * 0.5f;
    const float cy = pos.y + std::round(h * 0.5f);
    float x = pos.x + pad;
    dl->AddTriangleFilled(ImVec2(x, cy), ImVec2(x + s, cy - r), ImVec2(x + s, cy + r), ink);
    x += s + g;
    dl->AddTriangleFilled(ImVec2(x + r, cy - r), ImVec2(x + s, cy + r), ImVec2(x, cy + r), ink);
    x += s + g;
    dl->AddTriangleFilled(ImVec2(x, cy - r), ImVec2(x + s, cy - r), ImVec2(x + r, cy + r), ink);
    x += s + g;
    dl->AddTriangleFilled(ImVec2(x + s, cy), ImVec2(x, cy + r), ImVec2(x, cy - r), ink);
    return w;
  }
  dl->AddText(f, font_px(f), ImVec2(pos.x + pad, ink_centred_y(f, pos.y, h)), ink, key.c_str());
  return w;
}

namespace {

// The colour of each face button's disc: A is "go", B is "back", X and Y the
// two lesser actions, as a console's own glyphs colour them.
ImVec4 face_colour(const std::string& b) {
  if (b == "A") return kAccent;
  if (b == "B") return kError;
  if (b == "X") return kCyan;
  return kWarm;
}

bool is_face(const std::string& b) { return b == "A" || b == "B" || b == "X" || b == "Y"; }

float pill_w(ImFont* f, const std::string& name, float h) {
  return std::round(text_w(f, name.c_str()) + h * 0.9f);
}

float draw_pill(ImDrawList* dl, ImFont* f, ImVec2 pos, float h, const std::string& name, float a) {
  const float w = pill_w(f, name, h);
  dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), u32(kText, 0.85f * a), h * 0.5f);
  const float tw = text_w(f, name.c_str());
  dl->AddText(f, font_px(f), ImVec2(std::round(pos.x + (w - tw) * 0.5f), ink_centred_y(f, pos.y, h)), u32(kBg0, a),
              name.c_str());
  return w;
}

}  // namespace

float pad_glyph_w(ImFont* f, const std::string& button, float h) {
  if (is_face(button) || button == "dpad") return h;
  if (button == "LB RB") return pill_w(f, "LB", h) + std::round(h * 0.2f) + pill_w(f, "RB", h);
  return pill_w(f, button, h);
}

float draw_pad_glyph(ImDrawList* dl, ImFont* f, ImVec2 pos, float h, const std::string& button, float a) {
  const float r = h * 0.5f;
  const ImVec2 c(pos.x + r, pos.y + r);
  if (is_face(button)) {
    // A disc and its letter, the letter in the window's colour so it reads as
    // cut out of the disc.
    dl->AddCircleFilled(c, r, u32(face_colour(button), a), 24);
    const float tw = text_w(f, button.c_str());
    dl->AddText(f, font_px(f), ImVec2(std::round(c.x - tw * 0.5f), ink_centred_y(f, pos.y, h)), u32(kBg0, a),
                button.c_str());
    return h;
  }
  if (button == "dpad") {
    // A plus sign's four arms as triangles pointing out, the way the pad's
    // cross is printed.
    const ImU32 ink = u32(kText, 0.85f * a);
    const float s = std::round(h * 0.28f), in = std::round(h * 0.1f);
    dl->AddTriangleFilled(ImVec2(c.x, pos.y), ImVec2(c.x + s, c.y - in - s * 0.2f), ImVec2(c.x - s, c.y - in - s * 0.2f), ink);
    dl->AddTriangleFilled(ImVec2(c.x, pos.y + h), ImVec2(c.x - s, c.y + in + s * 0.2f), ImVec2(c.x + s, c.y + in + s * 0.2f), ink);
    dl->AddTriangleFilled(ImVec2(pos.x, c.y), ImVec2(c.x - in - s * 0.2f, c.y - s), ImVec2(c.x - in - s * 0.2f, c.y + s), ink);
    dl->AddTriangleFilled(ImVec2(pos.x + h, c.y), ImVec2(c.x + in + s * 0.2f, c.y + s), ImVec2(c.x + in + s * 0.2f, c.y - s), ink);
    return h;
  }
  if (button == "LB RB") {
    float x = pos.x;
    x += draw_pill(dl, f, ImVec2(x, pos.y), h, "LB", a) + std::round(h * 0.2f);
    x += draw_pill(dl, f, ImVec2(x, pos.y), h, "RB", a);
    return x - pos.x;
  }
  return draw_pill(dl, f, pos, h, button, a);
}

void draw_play_glyph(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
  const float h = b.y - a.y, w = std::min(b.x - a.x, h * 0.9f);
  dl->AddTriangleFilled(ImVec2(a.x, a.y), ImVec2(a.x + w, a.y + h * 0.5f), ImVec2(a.x, b.y), col);
}

void draw_back_glyph(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
  const float h = b.y - a.y, w = std::min(b.x - a.x, h * 0.9f);
  dl->AddTriangleFilled(ImVec2(a.x + w, a.y), ImVec2(a.x + w, b.y), ImVec2(a.x, a.y + h * 0.5f), col);
}

void draw_install_glyph(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
  // A shaft and a head, and a bar under them: "down, onto the disk".
  const float w = b.x - a.x, h = b.y - a.y;
  const float cx = std::round(a.x + w * 0.5f);
  const float shaft = std::max(2.0f, std::round(w * 0.22f));
  const float head_y = a.y + h * 0.42f, bar = std::max(2.0f, std::round(h * 0.12f));
  dl->AddRectFilled(ImVec2(cx - shaft * 0.5f, a.y), ImVec2(cx + shaft * 0.5f, head_y), col);
  dl->AddTriangleFilled(ImVec2(a.x, head_y), ImVec2(b.x, head_y), ImVec2(cx, b.y - bar * 1.8f), col);
  dl->AddRectFilled(ImVec2(a.x, b.y - bar), ImVec2(b.x, b.y), col);
}

}  // namespace kg::gui
