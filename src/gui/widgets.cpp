#include "widgets.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "anim.h"
#include "blocks.h"
#include "draw.h"
#include "imgui_internal.h"
#include "palette.h"
#include "scale.h"

namespace kg::gui {
namespace fs = std::filesystem;

namespace {

// Whether `name`, wrapped at `wrap`, fits `max_h` in `f` in no more than
// `max_lines` lines, with every word whole: a word wider than the wrap would
// be broken in the middle.
bool name_fits(ImFont* f, const std::string& name, float wrap, float max_h, int max_lines) {
  const float th = f->CalcTextSizeA(font_px(f), FLT_MAX, wrap, name.c_str()).y;
  if (th > max_h || th > font_px(f) * (static_cast<float>(max_lines) + 0.5f)) return false;
  size_t i = 0;
  while (i < name.size()) {
    size_t end = name.find(' ', i);
    if (end == std::string::npos) end = name.size();
    if (end > i && text_w(f, name.c_str() + i, name.c_str() + end) > wrap) return false;
    i = end + 1;
  }
  return true;
}

// The header set_page_trail asked for, for the page about to begin, or none
// at all (set_page_bare).
std::string g_trail;
bool g_bare = false;

}  // namespace

void set_page_trail(const std::string& trail) { g_trail = trail; }
void set_page_bare() { g_bare = true; }

void begin_page(const char* title, const char* hint, ImFont* big) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float bar_h = status_bar_height();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, std::max(1.0f, vp->WorkSize.y - bar_h)));
  // The page is the window; a border round it would only frame the screen.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::Begin(title, nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  page_focus_begin();
  draw_status_bar(title, hint);
  clear_chrome_path();

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const std::string trail = std::move(g_trail);
  g_trail.clear();
  if (g_bare) {
    g_bare = false;
    return;
  }
  if (!trail.empty()) {
    // One line: where the page is, up to its own name, in the body type with
    // the prompt block before it. The page's hero says the title.
    ImFont* body = font(FontRole::Body);
    const float bs = font_px(body);
    const float pw = std::max(2.0f, std::round(px(4)));
    const float gap = std::round(px(10));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float room = std::max(1.0f, ImGui::GetContentRegionAvail().x - pw - gap);
    const std::string shown = elide(body, trail, room);
    // The last step is the page itself; what leads to it is dim.
    const size_t cut = shown.rfind("\xe2\x80\xba");
    const size_t lead = cut == std::string::npos ? 0 : cut + 3;
    const float x = at.x + pw + gap;
    const float y = at.y;
    dl->AddText(body, bs, ImVec2(x, y), u32(kDim), shown.c_str(), shown.c_str() + lead);
    dl->AddText(body, bs, ImVec2(x + text_w(body, shown.c_str(), shown.c_str() + lead), y), u32(kText),
                shown.c_str() + lead);
    const float inset = std::round(bs * 0.15f);
    dl->AddRectFilled(ImVec2(at.x, y + inset), ImVec2(at.x + pw, y + bs - inset), u32(kAccent));
    ImGui::Dummy(ImVec2(0, bs));
    ImGui::Dummy(ImVec2(0, std::round(px(2))));
    const ImVec2 r0 = ImGui::GetCursorScreenPos();
    const float rule_w = ImGui::GetContentRegionAvail().x;
    const float thin = hairline();
    dl->AddRectFilled(r0, ImVec2(r0.x + rule_w, r0.y + thin), u32(kLine));
    dl->AddRectFilled(r0, ImVec2(r0.x + std::min(rule_w, std::round(px(64))), r0.y + thin * 2), u32(kAccent));
    ImGui::Dummy(ImVec2(0, thin * 2));
    return;
  }

  // The prompt: an accent block as tall as the title, the title after it,
  // and a cursor blinking at its end.
  if (!big) big = ImGui::GetFont();
  const float size = font_px(big);
  const float prompt_w = std::max(2.0f, std::round(px(5)));
  const float prompt_gap = std::round(px(12));
  const float cursor_w = std::round(size * 0.5f);
  const float avail = ImGui::GetContentRegionAvail().x;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const std::string shown = elide(big, title ? title : "", avail - prompt_w - prompt_gap - cursor_w - px(8));
  ImGui::SetCursorScreenPos(ImVec2(at.x + prompt_w + prompt_gap, at.y));
  ImGui::PushFont(big);
  ImGui::TextUnformatted(shown.c_str());
  ImGui::PopFont();
  const ImVec2 tmin = ImGui::GetItemRectMin(), tmax = ImGui::GetItemRectMax();
  const float inset = std::round(size * 0.12f);
  dl->AddRectFilled(ImVec2(at.x, tmin.y + inset), ImVec2(at.x + prompt_w, tmax.y - inset), u32(kAccent));
  // About once a second, easing in and out rather than snapping, and in the
  // dim green: present, not insistent.
  const float t = static_cast<float>(ImGui::GetTime());
  const float blink = reduce_motion() ? 1.0f : 0.5f + 0.5f * std::cos(t * 2.0f * 3.14159265f);
  const float cx = tmax.x + std::round(px(6));
  dl->AddRectFilled(ImVec2(cx, tmin.y + inset), ImVec2(cx + cursor_w, tmax.y - inset),
                    u32(kAccentDim, 0.25f + 0.75f * blink));

  // The rule: a hairline across the page with a short accent segment where
  // the prompt stands.
  ImGui::Dummy(ImVec2(0, std::round(px(4))));
  const ImVec2 r0 = ImGui::GetCursorScreenPos();
  const float rule_w = ImGui::GetContentRegionAvail().x;
  const float thin = hairline();
  dl->AddRectFilled(r0, ImVec2(r0.x + rule_w, r0.y + thin), u32(kLine));
  dl->AddRectFilled(r0, ImVec2(r0.x + std::min(rule_w, std::round(px(64))), r0.y + thin * 2), u32(kAccent));
  ImGui::Dummy(ImVec2(0, thin * 2 + std::round(px(10))));
}

ImU32 tile_colour(const std::string& id, float mul) {
  uint32_t h = 2166136261u;
  for (char c : id) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
  float hue = static_cast<float>(h % 360) / 360.0f;
  float r, g, b;
  // Dark and muted, so a tile with no picture is a tint of the terminal
  // rather than a coloured card, and its name reads on it.
  ImGui::ColorConvertHSVtoRGB(hue, 0.38f, 0.21f * mul, r, g, b);
  return ImGui::GetColorU32(ImVec4(r, g, b, 1.0f));
}

PageWindow::PageWindow(const char* title, const char* hint, ImFont* big)
    : uncaught_(std::uncaught_exceptions()) {
  begin_page(title, hint, big);
}

namespace {

// Multiplies the alpha of everything `w` and the windows inside it have drawn
// this frame by `a`.
void fade_drawn(ImGuiWindow* w, float a) {
  for (ImDrawVert& v : w->DrawList->VtxBuffer) {
    const float was = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF);
    v.col = (v.col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::lround(was * a)) << IM_COL32_A_SHIFT);
  }
  for (ImGuiWindow* child : w->DC.ChildWindows) fade_drawn(child, a);
}

}  // namespace

PageWindow::~PageWindow() {
  if (std::uncaught_exceptions() != uncaught_) return;
  // The page fades in as it comes up. Most of what a page draws by hand takes
  // its colours straight from the palette, not through the style's alpha, so
  // pushing ImGuiStyleVar_Alpha would fade its buttons and leave its art and
  // text standing: what the page and its panels drew is faded as a whole
  // instead. The status bar is on the background layer and stays as it is,
  // since it does not change between pages.
  const float a = fade_in(ImGui::GetID("##page-fade"), page_appearing(), 0.12f);
  if (a < 1.0f) fade_drawn(ImGui::GetCurrentWindow(), a);
  page_focus_end();
  ImGui::End();
}

void draw_image(ImDrawList* dl, const Texture& tex, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, ImU32 tint) {
  // SDL's software renderer, which a window falls back to when there is no
  // accelerated one, draws a very large textured triangle wrongly: half of a
  // big picture goes missing along its diagonal. It draws a rectangle made of
  // two small ones exactly, so a large picture is drawn as a grid of pieces
  // no bigger than kPiece. Each piece's edges fall on whole texels and on
  // whole pixels: that renderer copies a piece to whole pixels, and pieces
  // whose shared edge is not on one leave a dark seam between them.
  constexpr float kPiece = 256.0f;
  const ImTextureID id = reinterpret_cast<ImTextureID>(tex.tex);
  const float tw = static_cast<float>(std::max(tex.w, 1)), th = static_cast<float>(std::max(tex.h, 1));
  const int nx = std::max(1, static_cast<int>(std::ceil((b.x - a.x) / kPiece)));
  const int ny = std::max(1, static_cast<int>(std::ceil((b.y - a.y) / kPiece)));
  // The edge between piece i-1 and i, in texels and so in UV and on screen.
  auto edge = [](int i, int n, float lo, float hi, float size) {
    if (i <= 0) return lo;
    if (i >= n) return hi;
    return std::round((lo + (hi - lo) * static_cast<float>(i) / static_cast<float>(n)) * size) / size;
  };
  auto at = [](float u, float lo, float hi, float p, float q) {
    return std::round(hi == lo ? p : p + (q - p) * (u - lo) / (hi - lo));
  };
  for (int j = 0; j < ny; ++j) {
    const float v0 = edge(j, ny, uv0.y, uv1.y, th), v1 = edge(j + 1, ny, uv0.y, uv1.y, th);
    for (int i = 0; i < nx; ++i) {
      const float u0 = edge(i, nx, uv0.x, uv1.x, tw), u1 = edge(i + 1, nx, uv0.x, uv1.x, tw);
      dl->AddImage(id, ImVec2(at(u0, uv0.x, uv1.x, a.x, b.x), at(v0, uv0.y, uv1.y, a.y, b.y)),
                   ImVec2(at(u1, uv0.x, uv1.x, a.x, b.x), at(v1, uv0.y, uv1.y, a.y, b.y)), ImVec2(u0, v0),
                   ImVec2(u1, v1), tint);
    }
  }
}

void image(const Texture& tex, ImVec2 size, ImVec2 uv0, ImVec2 uv1) {
  const ImVec2 a = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  if (ImGui::IsItemVisible()) {
    draw_image(ImGui::GetWindowDrawList(), tex, a, ImVec2(a.x + size.x, a.y + size.y), uv0, uv1);
  }
}

// ---- the tile ---------------------------------------------------------------

bool tile(const std::string& id, const std::string& name, const std::string& sub,
          const Texture* art, ImFont* big, float w, float h, uint8_t dim, bool* focused_out) {
  TileSpec s;
  s.id = id;
  s.name = name;
  s.sub = sub;
  s.art = art;
  s.big = big;
  s.w = w;
  s.h = h;
  s.dim = dim;
  s.focused = focused_out;
  return tile_ex(s);
}

bool tile_ex(const TileSpec& spec) {
  const std::string& id = spec.id;
  const std::string& name = spec.name;
  const std::string& sub = spec.sub;
  const Texture* art = spec.art;
  ImFont* big = spec.big;
  const float w = spec.w, h = spec.h;
  const uint8_t dim = spec.dim;
  bool* focused_out = spec.focused;
  ImGui::PushID(id.c_str());
  ImGui::BeginGroup();

  ImVec2 p0 = ImGui::GetCursorScreenPos();
  // An invisible button underneath makes the whole tile focusable, which is
  // what the arrows and a gamepad move between. ImGui leaves an invisible
  // button out of navigation unless it is asked in. Its own focus rectangle
  // is left out: the tile draws its focus itself.
  ImGui::PushStyleColor(ImGuiCol_NavCursor, alpha(kAmber, 0.0f));
  bool activated = ImGui::InvisibleButton("tile", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
  ImGui::PopStyleColor();
  const ImGuiLastItemData button = GImGui->LastItemData;
  // Amber is where the keys go, so it is drawn only for the keyboard's and
  // the pad's focus; the mouse gets a frame in the accent's dim green.
  const bool focused = nav_focused();
  const bool hovered = !focused && ImGui::IsItemHovered();
  if (focused_out) *focused_out = ImGui::IsItemFocused();
  // It grows a little about its centre while it has the focus, and less
  // under the pointer, as a console's library does. The button stays the
  // size it was, so the mouse sees nothing move, and the glow goes round
  // what is drawn.
  const float ft = anim01(button.ID ^ 0x67726f77u, focused, 0.10f);
  const float ht = anim01(button.ID ^ 0x686f7672u, hovered, 0.10f);
  const float grow = spec.grow * ft + 0.015f * ht * (1.0f - ft);
  const float gx = std::round(w * grow * 0.5f), gy = std::round(h * grow * 0.5f);
  p0 = ImVec2(p0.x - gx, p0.y - gy);
  ImVec2 p1 = ImVec2(p0.x + w + gx * 2, p0.y + h + gy * 2);
  const float dw = p1.x - p0.x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Grown, it is drawn over its neighbours (grid_begin, blocks.h).
  const bool raised = (gx > 0 || gy > 0) && raise_in_grid(dl);
  ImFont* body = font(FontRole::Body);
  ImFont* small = font(FontRole::Small);
  if (!big) big = body;

  const float round = px(2);
  const float pad = std::round(px(12));
  // Without its caption a tile is its picture, or its tint with its name in
  // large type.
  const bool caption = spec.caption;
  const float cap_h = caption ? std::round(px(9) + font_px(body) + px(3) + font_px(small) + px(9)) : 0.0f;
  const float cap_y = p1.y - cap_h;
  const ImU32 name_col = focused ? u32(kAmber) : u32(kText);

  if (art) {
    // A little darker at rest when asked, and full brightness with the
    // focus, so the eye finds the focused one.
    const float lit = spec.rest_bright + (1.0f - spec.rest_bright) * ft;
    ImVec2 uv0, uv1;
    cover_uv(static_cast<float>(art->w), static_cast<float>(art->h), p1.x - p0.x, p1.y - p0.y, &uv0, &uv1);
    draw_image(dl, *art, p0, p1, uv0, uv1, u32(ImVec4(lit, lit, lit, 1.0f)));
  } else {
    // No picture: the game's own tint, darkening towards the caption, with
    // scanlines over it, and the game's name carrying the tile.
    dl->AddRectFilled(p0, p1, tile_colour(id), round);
    dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, cap_y), u32(kBg0, 0.0f), u32(kBg0, 0.0f), u32(kBg0, 0.55f),
                                u32(kBg0, 0.55f));
    const float step = std::max(2.0f, std::round(px(4)));
    const ImU32 line = u32(kBg0, 0.28f);
    const float thin = hairline();
    for (float y = p0.y + step; y + thin <= cap_y; y += step) {
      dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p1.x, y + thin), line);
    }

    dl->PushClipRect(p0, ImVec2(p1.x, cap_y), true);
    const std::string where = elide(small, "> " + id, dw - pad * 2);
    dl->AddText(small, font_px(small), ImVec2(p0.x + pad, p0.y + std::round(px(10))), u32(kAccent, 0.75f),
                where.c_str());
    // The name in the largest type it fits in whole, between the id and the
    // caption, without breaking a word, and with room left above the caption
    // so a long name does not run up against it. The large types take at
    // most three lines; past that a name reads as a paragraph, not a title.
    const float top = p0.y + std::round(px(10)) + font_px(small) + std::round(px(6));
    const float area_h = cap_y - top - std::round(px(6));
    const float fit_h = area_h - std::round(px(8));
    const float wrap = dw - pad * 2;
    ImFont* pick = nullptr;
    for (ImFont* f : {big, font(FontRole::Display), body}) {
      if (f && name_fits(f, name, wrap, fit_h, f == body ? 1000 : 3)) {
        pick = f;
        break;
      }
    }
    if (pick) {
      const float bs = font_px(pick);
      const ImVec2 ts = pick->CalcTextSizeA(bs, FLT_MAX, wrap, name.c_str());
      dl->AddText(pick, bs, ImVec2(p0.x + pad, top + std::max(0.0f, std::round((area_h - ts.y) * 0.5f))), name_col,
                  name.c_str(), nullptr, wrap);
    } else {
      // Not even the body type fits whole: as many lines as the space holds,
      // the last cut short with an ellipsis, rather than a line cut through
      // the middle by the edge of the art.
      const float lh = font_px(body);
      std::vector<std::string> lines = wrap_lines(body, name, wrap);
      const size_t room = static_cast<size_t>(std::max(1.0f, std::floor(fit_h / lh)));
      if (lines.size() > room) {
        std::string rest;
        for (size_t i = room - 1; i < lines.size(); ++i) rest += (rest.empty() ? "" : " ") + lines[i];
        lines.resize(room);
        lines.back() = elide(body, rest, wrap);
      }
      const float th = lh * static_cast<float>(lines.size());
      float y = top + std::max(0.0f, std::round((area_h - th) * 0.5f));
      for (const std::string& l : lines) {
        dl->AddText(body, lh, ImVec2(p0.x + pad, y), name_col, l.c_str());
        y += lh;
      }
    }
    dl->PopClipRect();
  }

  // Dimmed before the caption and the frame are drawn: the picture says the
  // game is not ready, and the caption stays readable, as its second line is
  // often the reason why. The focus still shows at full strength.
  if (dim) dl->AddRectFilled(p0, ImVec2(p1.x, cap_y), u32(kBg0, static_cast<float>(dim) / 255.0f));

  // A caption strip, so the tile still says what it is when the art is a
  // screenshot of somewhere deep inside the game.
  if (caption) {
    dl->AddRectFilled(ImVec2(p0.x, cap_y), p1, u32(kBg0, 0.9f), round, ImDrawFlags_RoundCornersBottom);
    dl->AddRectFilled(ImVec2(p0.x, cap_y), ImVec2(p1.x, cap_y + hairline()), u32(kLine));
    const std::string name_line = elide(body, name, dw - pad * 2);
    const std::string sub_line = elide(small, sub, dw - pad * 2);
    const float ny = cap_y + std::round(px(9));
    // A dimmed tile's name is a step quieter than a ready one's, unless it has
    // the focus.
    const ImU32 caption_col = focused || !dim ? name_col : u32(kText, 0.7f);
    dl->AddText(body, font_px(body), ImVec2(p0.x + pad, ny), caption_col, name_line.c_str());
    dl->AddText(small, font_px(small), ImVec2(p0.x + pad, ny + font_px(body) + std::round(px(3))), u32(kDim),
                sub_line.c_str());
  }

  if (focused) {
    const float t = std::max(2.0f, std::round(px(2)));
    frame_rect(dl, p0, p1, u32(kAmber), t);
    // Corner brackets, inside the frame so a tile at the edge of its grid
    // keeps all four.
    const float len = std::round(px(16)), in = std::round(px(6)), bt = std::max(2.0f, std::round(px(3)));
    const ImU32 c = u32(kAmber);
    const ImVec2 a(p0.x + in, p0.y + in), b(p1.x - in, p1.y - in);
    dl->AddRectFilled(a, ImVec2(a.x + len, a.y + bt), c);
    dl->AddRectFilled(a, ImVec2(a.x + bt, a.y + len), c);
    dl->AddRectFilled(ImVec2(b.x - len, a.y), ImVec2(b.x, a.y + bt), c);
    dl->AddRectFilled(ImVec2(b.x - bt, a.y), ImVec2(b.x, a.y + len), c);
    dl->AddRectFilled(ImVec2(a.x, b.y - bt), ImVec2(a.x + len, b.y), c);
    dl->AddRectFilled(ImVec2(a.x, b.y - len), ImVec2(a.x + bt, b.y), c);
    dl->AddRectFilled(ImVec2(b.x - len, b.y - bt), b, c);
    dl->AddRectFilled(ImVec2(b.x - bt, b.y - len), b, c);
  } else if (hovered) {
    frame_rect(dl, p0, p1, u32(kAccentDim), std::max(2.0f, std::round(px(2))));
  } else {
    frame_rect(dl, p0, p1, u32(kLine), hairline());
  }
  if (raised) lower_in_grid(dl);

  ImGui::EndGroup();
  // The button is the item the tile leaves behind, not the group round it,
  // so what follows a tile can ask whether it has the focus or make it the
  // page's default. The two have the same rectangle.
  GImGui->LastItemData = button;
  focus_glow_rect(p0, p1);
  ImGui::PopID();
  return activated;
}

// ---- chrome a page can use --------------------------------------------------

std::string elide(ImFont* f, const std::string& s, float max_w) {
  if (!f) f = ImGui::GetFont();
  if (text_w(f, s.c_str(), s.c_str() + s.size()) <= max_w) return s;
  const char* ell = ellipsis(f);
  const float ell_w = text_w(f, ell);
  if (ell_w > max_w) return std::string();
  // Every place the string may be cut: the start of each UTF-8 character.
  std::vector<size_t> cuts;
  for (size_t i = 0; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) cuts.push_back(i);
  }
  // The longest prefix that fits with the ellipsis after it.
  size_t lo = 0, hi = cuts.size();
  while (lo + 1 < hi) {
    size_t mid = (lo + hi) / 2;
    if (text_w(f, s.c_str(), s.c_str() + cuts[mid]) + ell_w <= max_w) lo = mid;
    else hi = mid;
  }
  std::string out = s.substr(0, cuts.empty() ? 0 : cuts[lo]);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out + ell;
}

ImVec2 keycap_size(const char* key) {
  ImFont* f = font(FontRole::Small);
  return ImVec2(keycap_w(f, key), std::round(font_px(f) + px(5)));
}

void keycap(const char* key, const ImVec4& bg) {
  ImFont* f = font(FontRole::Small);
  const ImVec2 size = keycap_size(key);
  // Centred on the line of body text it sits in.
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float y = at.y + std::round((ImGui::GetTextLineHeight() - size.y) * 0.5f);
  draw_keycap(ImGui::GetWindowDrawList(), f, ImVec2(at.x, y), size.y, key, u32(bg));
  ImGui::Dummy(ImVec2(size.x, ImGui::GetTextLineHeight()));
}

namespace {

// How many characters, not bytes, `s` has (with the terminal pieces below).
size_t utf8_len(const char* s);

// A section's heading with its rule running to `end`, a screen x.
void section_to(const char* label, float end) {
  ImGui::Spacing();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float lead = std::round(px(14)), gap = std::round(px(8));
  const float mid = at.y + std::round(ImGui::GetTextLineHeight() * 0.5f);
  const float thin = hairline();
  dl->AddRectFilled(ImVec2(at.x, mid), ImVec2(at.x + lead, mid + thin), u32(kAccentDim));
  ImGui::SetCursorScreenPos(ImVec2(at.x + lead + gap, at.y));
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  const float x = ImGui::GetItemRectMax().x + gap;
  if (end > x) dl->AddRectFilled(ImVec2(x, mid), ImVec2(end, mid + thin), u32(kLine));
}

}  // namespace

void section(const char* label) {
  section_to(label, ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x);
}

void section(const char* label, BadgeKind kind, const char* text) {
  const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  // The badge's width, as badge() lays it out: its word in four cells, or in
  // as many as it has when it has more.
  static const char* const words[] = {"OK", "WARN", "FAIL", "INFO"};
  const char* word = text ? text : words[static_cast<int>(kind)];
  const size_t cells = std::max<size_t>(4, utf8_len(word));
  const float badge_w = ImGui::CalcTextSize("[]").x + ImGui::CalcTextSize(std::string(cells, ' ').c_str()).x;
  const float gap = std::round(px(8));
  // The rule stops short of the badge, which ends the line.
  section_to(label, right - badge_w - gap);
  ImGui::SameLine();
  ImGui::SetCursorScreenPos(ImVec2(right - badge_w, ImGui::GetCursorScreenPos().y));
  badge(kind, text);
}

bool primary_button(const char* label, ImVec2 size) {
  const char* shown_end = ImGui::FindRenderedTextEnd(label);
  const float bracket = ImGui::CalcTextSize("[ ").x;
  if (size.x == 0) {
    size.x = ImGui::CalcTextSize(label, shown_end).x + ImGui::GetStyle().FramePadding.x * 2 + bracket * 2;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::PushStyleColor(ImGuiCol_Button, alpha(kAccentDim, 0.22f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, alpha(kAccentDim, 0.45f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentDim);
  ImGui::PushStyleColor(ImGuiCol_Border, alpha(kAccent, 0.55f));
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(5);
  // The brackets, either side of the label, "[ Play ]" however wide the
  // button is: at its edges on a wide button they read as two stray glyphs.
  // Amber while it has the keyboard's or the pad's focus, which the mouse's
  // hover is not; dim, and as faded as the rest of it, while it is disabled,
  // so a button that cannot be pressed does not look half on.
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
  ImVec4 col = nav_focused() ? kAmber : kAccent;
  if (disabled) col = kDim;
  const ImU32 c = u32(col, ImGui::GetStyle().Alpha);
  const float pad = ImGui::GetStyle().FramePadding.x;
  const float label_w = ImGui::CalcTextSize(label, shown_end).x;
  const float close_w = ImGui::CalcTextSize("]").x;
  const float mid = (a.x + b.x) * 0.5f;
  const float lx = std::max(a.x + pad, std::round(mid - label_w * 0.5f - bracket));
  const float rx = std::min(b.x - pad - close_w, std::round(mid + label_w * 0.5f + bracket - close_w));
  const float y = a.y + std::round((b.y - a.y - ImGui::GetTextLineHeight()) * 0.5f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(lx, y), c, "[");
  dl->AddText(ImVec2(rx, y), c, "]");
  return pressed;
}

void next_modal(float design_w, float design_h) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float room = std::round(px(24));
  const float w = std::min(px(design_w), vp->WorkSize.x - room * 2);
  const float max_h = std::max(1.0f, vp->WorkSize.y - status_bar_height() - room * 2);
  float h = 0.0f;
  if (design_h > 0) h = std::min(px(design_h), max_h);
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::max(w, 1.0f), h));
  // A height that follows the contents is otherwise held only to the whole
  // window, and a tall modal in a short one would run over the status bar.
  if (design_h <= 0) ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(w, 1.0f), 0), ImVec2(std::max(w, 1.0f), max_h));
}

bool small_button(const char* label) {
  // ImGui's own SmallButton has no padding above or below at all, so its
  // border sits on the glyphs; this is the same button with some.
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(px(6)), std::round(px(2))));
  const bool pressed = ImGui::ButtonEx(label, ImVec2(0, 0), ImGuiButtonFlags_AlignTextBaseLine);
  ImGui::PopStyleVar();
  return pressed;
}

float small_button_width(const char* label) {
  return ImGui::CalcTextSize(label, nullptr, true).x + std::round(px(6)) * 2;
}

// ---- layout -----------------------------------------------------------------

void vgap(float design_px) { ImGui::Dummy(ImVec2(0, std::round(px(design_px)))); }

void same_line_if_fits(float next_w) {
  ImGui::SameLine();
  if (ImGui::GetContentRegionAvail().x < next_w) ImGui::NewLine();
}

void same_line_if_fits(const char* next) {
  same_line_if_fits(ImGui::CalcTextSize(next, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2);
}

void align_right(float w) {
  const ImVec2 here = ImGui::GetCursorScreenPos();
  const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - w;
  ImGui::SetCursorScreenPos(ImVec2(std::max(here.x, right), here.y));
}

void right_aligned(const std::string& text, const ImVec4& colour) {
  const float w = ImGui::CalcTextSize(text.c_str()).x;
  const float room = ImGui::GetContentRegionAvail().x;
  if (w < room) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - w);
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopStyleColor();
}

void elided_text(const std::string& text, const ImVec4& colour) {
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextUnformatted(elide(nullptr, text, ImGui::GetContentRegionAvail().x).c_str());
  ImGui::PopStyleColor();
}

float field_width(float design) { return std::min(px(design), ImGui::GetContentRegionAvail().x); }

bool form_begin(const char* id, std::initializer_list<const char*> labels) {
  float w = 0;
  for (const char* l : labels) w = std::max(w, ImGui::CalcTextSize(l).x);
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(std::round(px(8)), std::round(px(4))));
  const bool open = ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  if (!open) return false;
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, w + std::round(px(8)));
  ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

void form_row(const char* label, bool framed) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  if (framed) ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", label);
  ImGui::TableSetColumnIndex(1);
}

bool begin_panel(const char* id, ImVec2 size, ImVec2 padding, const ImVec4& border, ImGuiWindowFlags flags) {
  // The child reads its colours and padding as it begins, so they are popped
  // straight away and never reach a window opened inside it.
  const bool pad = padding.x >= 0 && padding.y >= 0;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBg1);
  ImGui::PushStyleColor(ImGuiCol_Border, border);
  if (pad) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
  // Flattened, so the keyboard and the pad move between its items and the
  // page's as though they were one window, with no Enter to go in first.
  const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened, flags);
  if (pad) ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
  return open;
}

bool begin_text_panel(const char* id, ImVec2 size, ImVec2 padding, const ImVec4& border, ImGuiWindowFlags flags) {
  // Not flattened: a panel of text has no item for the focus to land on, and
  // flattened it could not be reached, or scrolled, at all.
  const bool pad = padding.x >= 0 && padding.y >= 0;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBg1);
  ImGui::PushStyleColor(ImGuiCol_Border, border);
  if (pad) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
  const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, flags);
  if (pad) ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
  scroll_with_keys();
  return open;
}

bool begin_list_panel(const char* id, float lines, float max_lines, float row_h) {
  const ImVec2 pad = px(10, 6);
  const float h = std::min(lines, max_lines) * row_h + pad.y * 2;
  return begin_panel(id, ImVec2(0, h), pad, kFrameLine);
}

void end_panel() {
  scroll_with_keys_end();
  ImGui::EndChild();
}

void title_rule() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  const float thin = hairline();
  dl->AddRectFilled(at, ImVec2(at.x + w, at.y + thin), u32(kLine));
  dl->AddRectFilled(at, ImVec2(at.x + std::min(w, std::round(px(48))), at.y + thin * 2), u32(kAccent));
  ImGui::Dummy(ImVec2(w, thin * 2));
}

void modal_title(const std::string& title) {
  // One line in the largest type it fits on, as a heading reads best whole;
  // a title too long even for the display type wraps in that.
  ImFont* big = font(FontRole::Big);
  ImFont* display = font(FontRole::Display);
  const float avail = ImGui::GetContentRegionAvail().x;
  ImFont* f = text_w(big, title.c_str()) <= avail ? big : display;
  ImGui::PushFont(f);
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::TextWrapped("%s", title.c_str());
  ImGui::PopStyleColor();
  ImGui::PopFont();
  title_rule();
  vgap(6);
}

// ---- terminal pieces --------------------------------------------------------

namespace {

// Whether the rows kv() draws are in a kv_begin table.
bool g_kv_table = false;

// How many characters, not bytes, `s` has.
size_t utf8_len(const char* s) {
  size_t n = 0;
  for (; *s; ++s) n += (static_cast<unsigned char>(*s) & 0xC0) != 0x80;
  return n;
}

}  // namespace

ImVec4 badge_colour(BadgeKind kind) {
  switch (kind) {
    case BadgeKind::Ok: return kAccent;
    case BadgeKind::Warn: return kWarm;
    case BadgeKind::Fail: return kError;
    case BadgeKind::Info: return kCyan;
  }
  return kText;
}

void badge(BadgeKind kind, const char* text) {
  static const char* const words[] = {"OK", "WARN", "FAIL", "INFO"};
  const char* word = text ? text : words[static_cast<int>(kind)];
  // Four characters between the brackets, the word centred in them, so a
  // column of badges lines up the way a boot log's does.
  const size_t n = utf8_len(word);
  const size_t pad = n < 4 ? 4 - n : 0;
  const std::string inner = std::string(pad / 2, ' ') + word + std::string(pad - pad / 2, ' ');
  ImGui::BeginGroup();
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextUnformatted("[");
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 0);
  ImGui::PushStyleColor(ImGuiCol_Text, badge_colour(kind));
  ImGui::TextUnformatted(inner.c_str());
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 0);
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextUnformatted("]");
  ImGui::PopStyleColor();
  ImGui::EndGroup();
}

float badge_gap() { return std::round(px(10)); }

void badge_line(BadgeKind kind, const std::string& text, const ImVec4& colour) {
  badge(kind);
  ImGui::SameLine(0, badge_gap());
  colored_text(colour, text);
}

void begin_badge_block(BadgeKind kind, const char* text) {
  badge(kind, text);
  ImGui::SameLine(0, badge_gap());
  ImGui::BeginGroup();
}

void end_badge_block() { ImGui::EndGroup(); }

void block_progress(float fraction, float width, const char* overlay) {
  const bool known = fraction >= 0;
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  char pct[16] = "";
  if (known) std::snprintf(pct, sizeof pct, "%3d%%", static_cast<int>(std::floor(fraction * 100.0f)));
  const float total = width > 0 ? width : ImGui::GetContentRegionAvail().x;
  // A cell is a whole number of pixels wide, so every block is the same
  // width; at a fraction the rounding makes some a pixel wider and the bar
  // reads as a dashed line. At least a character, so the brackets fit.
  const float cw = std::max(4.0f, std::ceil(ImGui::CalcTextSize("0").x));
  const float h = ImGui::GetTextLineHeight();
  const float gap = cw * 2;
  const float pct_w = known ? ImGui::CalcTextSize(pct).x : 0.0f;
  const float over_w = overlay && *overlay ? ImGui::CalcTextSize(overlay).x : 0.0f;
  // The bar gets what the words leave, and never fewer than eight cells;
  // an overlay that does not fit is cut short rather than the bar.
  const float words_w = (known ? gap + pct_w : 0.0f) + (over_w > 0 ? gap + over_w : 0.0f);
  const int cells = std::max(8, static_cast<int>(std::floor((total - words_w) / cw)) - 2);
  const float bar_w = cw * static_cast<float>(cells + 2);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 cur = ImGui::GetCursorScreenPos();
  const ImVec2 at(std::round(cur.x), cur.y);
  const ImU32 dim = u32(kDim);
  dl->AddText(at, dim, "[");
  dl->AddText(ImVec2(at.x + cw * static_cast<float>(cells + 1), at.y), dim, "]");
  // Each cell a block with a pixel between it and the next, so the bar reads
  // as characters, as a terminal's does, and not as one smooth fill.
  const float inset = std::round(h * 0.2f);
  const float sep = std::max(1.0f, std::round(px(1)));
  const float y0 = at.y + inset, y1 = at.y + h - inset;
  auto cell = [&](int i, float fill, ImU32 c) {
    const float x0 = at.x + cw * static_cast<float>(i + 1);
    const float x1 = x0 + cw - sep;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + std::round((x1 - x0) * fill), y1), c);
  };
  const ImU32 empty = u32(kLine), full = u32(kAccent);
  for (int i = 0; i < cells; ++i) cell(i, 1.0f, empty);
  if (known) {
    const float done = fraction * static_cast<float>(cells);
    const int whole = static_cast<int>(std::floor(done));
    for (int i = 0; i < whole; ++i) cell(i, 1.0f, full);
    if (whole < cells && done > static_cast<float>(whole)) cell(whole, done - static_cast<float>(whole), full);
  } else {
    // A block a sixth of the bar wide, sliding to one end and back about
    // every two seconds.
    const int len = std::max(2, cells / 6);
    const float t = std::fmod(static_cast<float>(ImGui::GetTime()), 2.0f) / 2.0f;
    const float u = t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
    const int start = static_cast<int>(std::lround(u * static_cast<float>(cells - len)));
    for (int i = start; i < start + len; ++i) cell(i, 1.0f, full);
  }
  float x = at.x + bar_w;
  if (known) {
    dl->AddText(ImVec2(x + gap, at.y), u32(kText), pct);
    x += gap + pct_w;
  }
  if (over_w > 0) {
    const std::string shown = elide(nullptr, overlay, at.x + total - x - gap);
    dl->AddText(ImVec2(x + gap, at.y), dim, shown.c_str());
  }
  ImGui::Dummy(ImVec2(std::max(total, bar_w), h));
}

bool kv_begin(const char* id, float label_w) {
  // Rows close together, and the gap between the columns from the padding
  // either side of it.
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(std::round(px(6)), std::round(px(2))));
  g_kv_table = ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  if (!g_kv_table) return false;
  ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, label_w > 0 ? label_w : 0.0f);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

void kv(const char* label, const std::string& value, const ImVec4& colour) {
  if (g_kv_table) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
  } else {
    const float lw = std::round(px(150));
    const float x = ImGui::GetCursorPosX();
    ImGui::TextDisabled("%s", elide(nullptr, label, lw - std::round(px(8))).c_str());
    ImGui::SameLine(x + lw);
  }
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextWrapped("%s", value.c_str());
  ImGui::PopStyleColor();
}

void kv_end() {
  if (!g_kv_table) return;
  g_kv_table = false;
  ImGui::EndTable();
}

const char* spinner_glyph() {
  static const char* const frames[] = {"|", "/", "-", "\\"};
  return frames[static_cast<int>(ImGui::GetTime() * 8.0) % 4];
}

void spinner() {
  // One character cell whatever the frame, so the text after it stays put
  // even in a font that is not monospace.
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(ImGui::CalcTextSize("-").x, ImGui::GetTextLineHeight()));
  ImGui::GetWindowDrawList()->AddText(at, u32(kAccent), spinner_glyph());
}

namespace {

// A piece of an empty state's line: words, or a key drawn as a keycap.
struct Piece {
  std::string text;
  bool key = false;
};

// A line with the key named after "press" drawn as a keycap: "press A", "Press
// Enter". A key is a word of letters and digits that starts with a capital or
// a digit, so "press a button" stays words.
std::vector<Piece> key_pieces(const std::string& line) {
  std::vector<Piece> out;
  size_t from = 0;
  for (size_t i = 0; i + 6 < line.size(); ++i) {
    if (i > 0 && std::isalnum(static_cast<unsigned char>(line[i - 1]))) continue;
    std::string word = line.substr(i, 6);
    for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (word != "press ") continue;
    const size_t k = i + 6;
    size_t e = k;
    while (e < line.size() && std::isalnum(static_cast<unsigned char>(line[e]))) ++e;
    const unsigned char first = static_cast<unsigned char>(line[k]);
    if (e == k || e - k > 6 || !(std::isupper(first) || std::isdigit(first))) continue;
    out.push_back({line.substr(from, k - from), false});
    out.push_back({line.substr(k, e - k), true});
    from = e;
    i = e - 1;
  }
  out.push_back({line.substr(from), false});
  return out;
}

float pieces_w(const std::vector<Piece>& ps) {
  float w = 0;
  for (const Piece& p : ps) w += p.key ? keycap_size(p.text.c_str()).x : ImGui::CalcTextSize(p.text.c_str()).x;
  return w;
}

}  // namespace

void empty_state(const char* message, const char* glyph, float height) {
  ImFont* big = font(FontRole::Big);
  ImFont* body = ImGui::GetFont();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float w = std::max(1.0f, avail.x);
  const float wrap = std::min(w, std::round(px(560)));
  // The first paragraph, or the first sentence of a message that is one
  // paragraph, is the headline, in full-strength text; what follows explains
  // it and is dim.
  const std::string msg = message ? message : "";
  std::string head = msg, rest;
  if (size_t p = msg.find("\n\n"); p != std::string::npos) {
    head = msg.substr(0, p);
    rest = msg.substr(p + 2);
  } else {
    for (size_t i = 0; i + 1 < msg.size(); ++i) {
      if ((msg[i] == '.' || msg[i] == '?' || msg[i] == '!') && msg[i + 1] == ' ') {
        size_t r = i + 1;
        while (r < msg.size() && msg[r] == ' ') ++r;
        if (r < msg.size()) {
          head = msg.substr(0, i + 1);
          rest = msg.substr(r);
        }
        break;
      }
    }
  }
  while (!rest.empty() && rest.front() == '\n') rest.erase(0, 1);
  const std::vector<std::string> head_lines = wrap_lines(body, head, wrap);
  const std::vector<std::string> rest_lines = rest.empty() ? std::vector<std::string>() : wrap_lines(body, rest, wrap);
  const float lh = ImGui::GetTextLineHeightWithSpacing();
  const float para = std::round(lh * 0.5f);
  const float bs = font_px(big);
  const float gap = std::round(px(16));
  const float content = bs + gap + lh * static_cast<float>(head_lines.size() + rest_lines.size()) +
                        (rest_lines.empty() ? 0.0f : para);
  const float h = std::max(height > 0 ? height : avail.y, content);
  // A little above the middle, where the eye expects the centre of a page.
  float y = at.y + std::max(0.0f, std::round((h - content) * 0.42f));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cx = at.x + w * 0.5f;
  if (glyph) {
    const float gw = big->CalcTextSizeA(bs, FLT_MAX, 0.0f, glyph).x;
    dl->AddText(big, bs, ImVec2(std::round(cx - gw * 0.5f), y), u32(kDim), glyph);
  } else {
    // A prompt waiting at an empty line, its cursor blinking as the title's
    // does.
    const float pw = big->CalcTextSizeA(bs, FLT_MAX, 0.0f, ">").x;
    const float cur = std::round(bs * 0.5f), sp = std::round(bs * 0.3f);
    const float x0 = std::round(cx - (pw + sp + cur) * 0.5f);
    dl->AddText(big, bs, ImVec2(x0, y), u32(kAccentDim), ">");
    const float blink = reduce_motion() ? 1.0f : 0.5f + 0.5f * std::cos(static_cast<float>(ImGui::GetTime()) * 2.0f * 3.14159265f);
    const float inset = std::round(bs * 0.12f);
    dl->AddRectFilled(ImVec2(x0 + pw + sp, y + inset), ImVec2(x0 + pw + sp + cur, y + bs - inset),
                      u32(kAccentDim, 0.25f + 0.75f * blink));
  }
  y += bs + gap;
  ImFont* small = font(FontRole::Small);
  const float cap_h = keycap_size("A").y;
  auto draw_lines = [&](const std::vector<std::string>& lines, const ImVec4& colour) {
    for (const std::string& l : lines) {
      const std::vector<Piece> ps = key_pieces(l);
      float x = std::round(cx - pieces_w(ps) * 0.5f);
      for (const Piece& p : ps) {
        if (p.key) {
          const float cap_y = y + std::round((ImGui::GetTextLineHeight() - cap_h) * 0.5f);
          x += draw_keycap(dl, small, ImVec2(x, cap_y), cap_h, p.text, u32(kCyan));
        } else {
          dl->AddText(ImVec2(x, y), u32(colour), p.text.c_str());
          x += ImGui::CalcTextSize(p.text.c_str()).x;
        }
      }
      y += lh;
    }
  };
  draw_lines(head_lines, kText);
  if (!rest_lines.empty()) {
    y += para;
    draw_lines(rest_lines, kDim);
  }
  ImGui::Dummy(ImVec2(w, h));
}

// ---- small pieces -----------------------------------------------------------

void colored_text(const ImVec4& colour, const std::string& s) {
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextWrapped("%s", s.c_str());
  ImGui::PopStyleColor();
}

void warn_text(const std::string& s) { colored_text(kWarn, s); }

void good_text(const std::string& s) { colored_text(kGood, s); }

bool input_string(const char* label, std::string& s, size_t cap, ImGuiInputTextFlags flags) {
  std::vector<char> buf(cap, '\0');
  std::snprintf(buf.data(), cap, "%s", s.c_str());
  if (!ImGui::InputText(label, buf.data(), cap, flags)) return false;
  s = buf.data();
  return true;
}

bool input_string_multiline(const char* label, std::string& s, size_t cap, ImVec2 size) {
  std::vector<char> buf(cap, '\0');
  std::snprintf(buf.data(), cap, "%s", s.c_str());
  if (!ImGui::InputTextMultiline(label, buf.data(), cap, size)) return false;
  s = buf.data();
  return true;
}

fs::path home_dir() {
  if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
  return fs::path("/");
}

}  // namespace kg::gui
