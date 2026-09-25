#include "blocks.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <memory>
#include <vector>

#include "anim.h"
#include "draw.h"
#include "focus.h"
#include "imgui_internal.h"
#include "layout.h"
#include "scale.h"
#include "scroll.h"
#include "widgets.h"

namespace kg::gui {

namespace {

// A draw list split in two for the frame, for something drawn in a different
// order from the one it is laid out in: a band behind a row laid out before
// its height is known, a grown tile over the neighbours laid out after it.
struct Layers {
  ImDrawListSplitter split;
  ImDrawList* dl = nullptr;
  int frame = -1;
  // What channel 0 is for: behind (a band or a card) or the ordinary layer
  // (a grid, whose channel 1 is the raised one).
  bool behind = false;
  ImVec2 start;
  float pad = 0;
  float work_max = 0, content_max = 0, right = 0;
  // An action bar's line: where it starts and the height its items are
  // centred on (0 until the first item is down).
  bool bar = false;
  float row_top = 0, row_h = 0;
};
std::vector<std::unique_ptr<Layers>> g_layers;

Layers& push_layers(bool behind) {
  const int now = ImGui::GetFrameCount();
  // Left over from a frame a page threw in: that frame's draw list has been
  // reset, so all there is to do is let them go.
  while (!g_layers.empty() && g_layers.back()->frame != now) g_layers.pop_back();
  g_layers.push_back(std::make_unique<Layers>());
  Layers& l = *g_layers.back();
  l.dl = ImGui::GetWindowDrawList();
  l.frame = now;
  l.behind = behind;
  l.split.Split(l.dl, 2);
  l.split.SetCurrentChannel(l.dl, behind ? 1 : 0);
  return l;
}

void pop_layers() {
  if (g_layers.empty()) return;
  Layers& l = *g_layers.back();
  l.split.Merge(l.dl);
  g_layers.pop_back();
}

// The room a band across the page spans: the whole width of the window, from
// one edge to the other, rather than only its content.
ImRect full_width(float y0, float y1) {
  const ImGuiWindow* w = ImGui::GetCurrentWindow();
  return ImRect(ImVec2(w->Pos.x, y0), ImVec2(w->InnerRect.Max.x, y1));
}

// Where a band may draw: all of it that shows, out to the window's own edges.
// ImGui keeps a window's items half its padding clear of its sides, and a band
// clipped there would stop short of the edges it is meant to run to.
ImRect band_clip(const ImRect& band) {
  const ImGuiWindow* w = ImGui::GetCurrentWindow();
  ImRect r(ImVec2(std::max(w->InnerRect.Min.x, w->OuterRectClipped.Min.x), w->InnerClipRect.Min.y),
           ImVec2(std::min(w->InnerRect.Max.x, w->OuterRectClipped.Max.x), w->InnerClipRect.Max.y));
  r.ClipWithFull(band);
  return r;
}

std::string upper(const char* b, const char* e) {
  std::string s(b, e);
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// A colour as ImGui would draw it now: faded with the style's alpha, which a
// disabled block or a fading dialog has turned down.
ImU32 col(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(alpha(c, a)); }

ImVec4 lighter(const ImVec4& c) { return ImLerp(c, ImVec4(1, 1, 1, c.w), 0.12f); }

float ghost_h() { return std::round(px(breakpoint() == Breakpoint::Compact ? 36 : 40)); }

}  // namespace

// ---- art ---------------------------------------------------------------------

namespace {

// `c` with its alpha multiplied by `a`.
ImU32 faded(ImU32 c, float a) {
  const float was = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF);
  const ImU32 now = static_cast<ImU32>(std::lround(std::clamp(was * a, 0.0f, 255.0f)));
  return (c & ~IM_COL32_A_MASK) | (now << IM_COL32_A_SHIFT);
}

// The picture behind a band: `back` scaled to cover it and cropped about its
// middle, or the game's tint when there is none.
void hero_backdrop(ImDrawList* dl, const ImRect& band, const Texture* back, const std::string& id, float a) {
  if (back && back->w > 0 && back->h > 0) {
    ImVec2 uv0, uv1;
    cover_uv(static_cast<float>(back->w), static_cast<float>(back->h), band.GetWidth(), band.GetHeight(), &uv0, &uv1);
    draw_image(dl, *back, band.Min, band.Max, uv0, uv1, u32(ImVec4(1, 1, 1, a)));
  } else {
    dl->AddRectFilled(band.Min, band.Max, faded(tile_colour(id), a));
  }
}

}  // namespace

float hero(const HeroSpec& s) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w->SkipItems) return ImGui::GetCursorScreenPos().x;
  const float h = s.height > 0 ? std::round(px(s.height)) : hero_height();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const ImRect band = full_width(at.y, at.y + h);
  ImDrawList* dl = w->DrawList;
  const ImRect clip = band_clip(band);
  dl->PushClipRect(clip.Min, clip.Max, false);
  const float fade = fade_in(ImGui::GetID("##hero-fade"), page_appearing(), 0.15f);
  // Part way through a change of game, the old one's pictures under the new.
  const bool was = s.mix < 1.0f && !s.was_id.empty();
  const float mix = was ? std::clamp(s.mix, 0.0f, 1.0f) : 1.0f;

  // The picture behind everything, scaled to cover the band and cropped at
  // its middle; the blurred copy when there is one.
  const Texture* back = s.backdrop ? s.backdrop : s.art;
  const Texture* was_back = s.was_backdrop ? s.was_backdrop : s.was_art;
  dl->AddRectFilled(band.Min, band.Max, u32(kBg0));
  if (was) hero_backdrop(dl, band, was_back, s.was_id, fade);
  hero_backdrop(dl, band, back, s.id, fade * mix);
  // Darkened, and scanlined the way a tile with no picture is, so it is the
  // terminal's own light and any text over it reads.
  dl->AddRectFilled(band.Min, band.Max, u32(kScrim, 0.45f));
  const float step = std::max(2.0f, std::round(px(3)));
  const float thin = hairline();
  for (float y = band.Min.y + step; y + thin <= band.Max.y; y += step) {
    dl->AddRectFilled(ImVec2(band.Min.x, y), ImVec2(band.Max.x, y + thin), u32(kScrim, 0.18f));
  }
  // Behind the text at the left, and into the page at the foot.
  const float mid = band.Min.x + band.GetWidth() * 0.65f;
  gradient_h(dl, band.Min, ImVec2(mid, band.Max.y), u32(kScrim, 0.92f), u32(kScrim, 0.15f));
  dl->AddRectFilled(ImVec2(mid, band.Min.y), band.Max, u32(kScrim, 0.15f));
  gradient_v(dl, ImVec2(band.Min.x, band.Max.y - h * 0.4f), band.Max, u32(kScrim, 0.0f), u32(kBg0));

  // The sharp picture, framed at the right, where there is the room for it,
  // below what the page keeps at the band's top.
  const float margin = std::round(px(32));
  const float top = std::round(px(s.top));
  const float room_h = h - top;
  const bool capsule = s.art_right && breakpoint() != Breakpoint::Compact;
  const float right_edge = s.content_right > 0 ? std::min(s.content_right, band.Max.x - margin) : band.Max.x - margin;
  auto capsule_rect = [&](const Texture* t, ImVec2* c0, ImVec2* c1) {
    const float ch = std::round(std::min(h * 0.72f, room_h * 0.86f));
    const float cw = std::min(std::round(ch * static_cast<float>(t->w) / static_cast<float>(t->h)),
                              std::round(band.GetWidth() * 0.45f));
    *c0 = ImVec2(right_edge - cw, band.Min.y + top + std::round((room_h - ch) * 0.35f));
    *c1 = ImVec2(c0->x + cw, c0->y + ch);
  };
  float text_right = right_edge;
  if (capsule && was && s.was_art && s.was_art->w > 0 && s.was_art->h > 0) {
    ImVec2 c0, c1;
    capsule_rect(s.was_art, &c0, &c1);
    draw_image(dl, *s.was_art, c0, c1, ImVec2(0, 0), ImVec2(1, 1), u32(ImVec4(1, 1, 1, fade * (1.0f - mix))));
    frame_rect(dl, c0, c1, u32(kLine, 1.0f - mix), thin);
  }
  if (capsule && s.art && s.art->w > 0 && s.art->h > 0) {
    ImVec2 c0, c1;
    capsule_rect(s.art, &c0, &c1);
    draw_image(dl, *s.art, c0, c1, ImVec2(0, 0), ImVec2(1, 1), u32(ImVec4(1, 1, 1, fade * mix)));
    frame_rect(dl, c0, c1, u32(kLine, mix), thin);
    text_right = c0.x - margin;
  } else if (!back && s.top <= 0) {
    // No picture at all: where it would be says whose tint this is. Not
    // under a strip of the page's own, where it would read as the strip's.
    ImFont* small = font(FontRole::Small);
    const std::string where = "> " + s.id;
    dl->AddText(small, font_px(small), ImVec2(at.x, band.Min.y + std::round(px(16))), u32(kAccent, 0.6f * mix),
                where.c_str());
  }

  // The title at the foot, or above what the page keeps there, in the hero
  // type when it fits and on two lines of the big type when it does not, with
  // the prompt block before it.
  ImFont* hero_f = font(FontRole::Hero);
  ImFont* big = font(FontRole::Big);
  ImFont* small = font(FontRole::Small);
  const float prompt_w = std::max(2.0f, std::round(px(6)));
  const float prompt_gap = std::round(px(14));
  const float tx = at.x + prompt_w + prompt_gap;
  const float room = std::max(1.0f, text_right - tx);
  const float bottom = band.Max.y - (s.foot > 0 ? std::round(px(s.foot)) : std::round(px(28)));
  std::vector<std::string> lines;
  ImFont* tf = hero_f;
  if (text_w(hero_f, s.title.c_str()) <= room) {
    lines.push_back(s.title);
  } else {
    tf = big;
    lines = wrap_lines(big, s.title, room);
    if (lines.size() > 2) {
      std::string rest;
      for (size_t i = 1; i < lines.size(); ++i) rest += (rest.empty() ? "" : " ") + lines[i];
      lines.resize(2);
      lines[1] = elide(big, rest, room);
    }
  }
  // The new title comes up out of the old one's place rather than blinking.
  const float ink = 0.35f + 0.65f * mix;
  const float lh = font_px(tf);
  const float title_top = bottom - lh * static_cast<float>(lines.size());
  float y = title_top;
  for (const std::string& l : lines) {
    dl->AddText(tf, lh, ImVec2(tx, y), u32(kText, ink), l.c_str());
    y += lh;
  }
  const float inset = std::round(lh * 0.12f);
  dl->AddRectFilled(ImVec2(at.x, title_top + inset), ImVec2(at.x + prompt_w, bottom - inset), u32(kAccent));
  if (!s.kicker.empty()) {
    const std::string k = elide(small, s.kicker, room);
    dl->AddText(small, font_px(small), ImVec2(tx, title_top - font_px(small) - std::round(px(8))), u32(kText, 0.75f * ink),
                k.c_str());
  }
  dl->PopClipRect();
  ImGui::Dummy(ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), h));
  return text_right;
}

// ---- actions -------------------------------------------------------------------

bool play_button(const char* label, PlayKind kind, float min_w, const char* shown) {
  const char* end = ImGui::FindRenderedTextEnd(label);
  const std::string text = shown ? std::string(shown) : upper(label, end);
  ImFont* f = font(FontRole::Display);
  const float fs = font_px(f);
  const float h = std::round(px(56));
  const float pad = std::round(px(24)), glyph = std::round(fs * 0.8f), gap = std::round(px(12));
  const bool has_glyph = kind != PlayKind::Blocked;
  const float want = pad * 2 + (has_glyph ? glyph + gap : 0.0f) + text_w(f, text.c_str());
  const float w = std::round(std::max({want, min_w > 0 ? min_w : std::round(px(220)), 1.0f}));
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  if (!ImGui::IsItemVisible()) return pressed && (kind == PlayKind::Play || kind == PlayKind::Install);
  const ImVec2 b(a.x + w, a.y + h);
  ImVec4 fill = kAccent, ink = kPlayText;
  switch (kind) {
    case PlayKind::Play:
      fill = held ? kAccentDim : hovered ? kPlayHover
                                         : kAccent;
      break;
    case PlayKind::Install:
      fill = held ? alpha(kInstall, 0.7f) : hovered ? lighter(kInstall)
                                                    : kInstall;
      break;
    case PlayKind::Blocked:
      fill = kLine;
      ink = kDim;
      break;
    case PlayKind::Busy:
      fill = kAccentDim;
      ink = kText;
      break;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, col(fill), px(2));
  float x = a.x + std::round((w - want) * 0.5f) + pad;
  const float gy = std::round(a.y + (h - glyph) * 0.5f);
  if (kind == PlayKind::Play) draw_play_glyph(dl, ImVec2(x, gy), ImVec2(x + glyph, gy + glyph), col(ink));
  if (kind == PlayKind::Install) draw_install_glyph(dl, ImVec2(x, gy), ImVec2(x + glyph, gy + glyph), col(ink));
  if (kind == PlayKind::Busy) {
    dl->AddText(f, fs, ImVec2(x + std::round((glyph - text_w(f, spinner_glyph())) * 0.5f), ink_centred_y(f, a.y, h)),
                col(ink), spinner_glyph());
  }
  if (has_glyph) x += glyph + gap;
  dl->AddText(f, fs, ImVec2(x, ink_centred_y(f, a.y, h)), col(ink), text.c_str());
  return pressed && (kind == PlayKind::Play || kind == PlayKind::Install);
}

bool ghost_button(const char* label, ImVec2 size, const char* shown) {
  const float h = size.y > 0 ? size.y : ghost_h();
  bool pressed = false;
  if (shown) {
    // The same frame and fill as below, round text that is not the label:
    // drawn over an invisible button that keeps the label as its ID.
    const float pad = std::round(px(16));
    const float w = size.x > 0 ? size.x : std::round(ImGui::CalcTextSize(shown).x + pad * 2);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    pressed = ImGui::InvisibleButton(label, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    const ImVec2 b(a.x + w, a.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemActive()) dl->AddRectFilled(a, b, col(kBgActive), px(2));
    else if (ImGui::IsItemHovered()) dl->AddRectFilled(a, b, col(kBg3), px(2));
    frame_rect(dl, a, b, col(kFrameLine), hairline());
    ImFont* f = ImGui::GetFont();
    const float tw = ImGui::CalcTextSize(shown).x;
    dl->AddText(f, ImGui::GetFontSize(), ImVec2(std::round(a.x + (w - tw) * 0.5f), ink_centred_y(f, a.y, h)), col(kText),
                shown);
  } else {
    const float fp = std::max(0.0f, std::round((h - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(px(16)), fp));
    ImGui::PushStyleColor(ImGuiCol_Button, alpha(kBg0, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBg3);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kBgActive);
    ImGui::PushStyleColor(ImGuiCol_Border, kFrameLine);
    pressed = ImGui::Button(label, ImVec2(size.x, h));
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
  }
  // Under the pointer its frame takes the accent's dim green, as a tile's
  // does; the amber is the keyboard's.
  if (ImGui::IsItemHovered() && !nav_focused()) {
    frame_rect(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), col(kAccentDim), hairline());
  }
  return pressed;
}

void stat(const char* label, const std::string& value, const ImVec4& colour) {
  ImGui::BeginGroup();
  ImFont* small = font(FontRole::Small);
  ImGui::PushFont(small);
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  const std::string caps = upper(label, label + std::char_traits<char>::length(label));
  ImGui::TextUnformatted(caps.c_str());
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextUnformatted(value.c_str());
  ImGui::PopStyleColor();
  ImGui::EndGroup();
}

ImVec2 stat_size(const char* label, const std::string& value) {
  ImFont* small = font(FontRole::Small);
  const std::string caps = upper(label, label + std::char_traits<char>::length(label));
  const float w = std::max(text_w(small, caps.c_str()), ImGui::CalcTextSize(value.c_str()).x);
  // The caption's line, as TextUnformatted lays it, then the value's.
  return ImVec2(w, font_px(small) + ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeight());
}

void action_bar_begin(const char* id) {
  ImGui::PushID(id);
  Layers& l = push_layers(true);
  l.start = ImGui::GetCursorScreenPos();
  l.pad = std::round(px(16));
  ImGui::Dummy(ImVec2(0, l.pad - ImGui::GetStyle().ItemSpacing.y));
  ImGui::BeginGroup();
  l.bar = true;
  l.row_top = ImGui::GetCursorPosY();
  l.row_h = 0;
}

bool action_bar_next(ImVec2 size, float gap, bool right) {
  if (g_layers.empty() || !g_layers.back()->bar) {
    ImGui::SameLine(0, gap);
    return true;
  }
  Layers& l = *g_layers.back();
  // The line's height is its first item's: the play button's, on the first.
  if (l.row_h <= 0) l.row_h = ImGui::GetItemRectSize().y;
  ImGui::SameLine(0, gap);
  if (ImGui::GetContentRegionAvail().x < size.x) {
    ImGui::NewLine();
    // A little more than the items' spacing, so a wrapped line reads as a
    // line of its own rather than as the play button's underline.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::round(px(6)));
    l.row_top = ImGui::GetCursorPosY();
    l.row_h = size.y;
    return false;
  }
  ImGui::SetCursorPosY(l.row_top + std::max(0.0f, std::round((l.row_h - size.y) * 0.5f)));
  if (right) align_right(size.x);
  return true;
}

int action_bar_ghosts(const char* const* labels, int n) {
  const float gap = std::round(px(10));
  const float pad = std::round(px(16));
  auto width = [&](const char* l) { return ImGui::CalcTextSize(l, nullptr, true).x + pad * 2; };
  float all = 0;
  for (int i = 0; i < n; ++i) all += width(labels[i]) + (i ? gap : 0.0f);
  int pressed = -1;
  for (int i = 0; i < n; ++i) {
    if (i == 0) action_bar_next(ImVec2(all, ghost_h()), std::round(px(32)), true);
    else action_bar_next(ImVec2(width(labels[i]), ghost_h()), gap);
    if (ghost_button(labels[i])) pressed = i;
  }
  return pressed;
}

void action_bar_end() {
  ImGui::EndGroup();
  if (g_layers.empty()) {
    ImGui::PopID();
    return;
  }
  Layers& l = *g_layers.back();
  const float bottom = ImGui::GetItemRectMax().y + l.pad;
  const ImVec2 start = l.start;
  l.split.SetCurrentChannel(l.dl, 0);
  const ImRect band = full_width(start.y, bottom);
  const ImRect clip = band_clip(band);
  l.dl->PushClipRect(clip.Min, clip.Max, false);
  l.dl->AddRectFilled(band.Min, band.Max, col(kBg1));
  l.dl->AddRectFilled(band.Min, ImVec2(band.Max.x, band.Min.y + hairline()), col(kLine));
  l.dl->PopClipRect();
  pop_layers();
  ImGui::SetCursorScreenPos(ImVec2(start.x, bottom));
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::PopID();
}

namespace {

// The page a page-wide region is in, and how far the page's content reached
// before it: the region runs past the page's margins, and without this the
// page would count that as content and let the keys scroll it sideways.
struct PageScroll {
  ImGuiWindow* page = nullptr;
  ImVec2 max, ideal;
};
PageScroll g_page_scroll;

}  // namespace

bool begin_page_scroll(const char* id) {
  ImGuiWindow* pw = ImGui::GetCurrentWindow();
  g_page_scroll = PageScroll{pw, pw->DC.CursorMaxPos, pw->DC.IdealMaxPos};
  const float top = ImGui::GetCursorScreenPos().y;
  const ImRect inner = pw->InnerRect;
  // A child is clipped to what its parent's clip is when it begins, and the
  // page's keeps clear of its sides; the region's scrollbar is at the edge.
  ImGui::PushClipRect(inner.Min, inner.Max, false);
  ImGui::SetCursorScreenPos(ImVec2(inner.Min.x, top));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().WindowPadding.x, 0.0f));
  ScrollOpts o;
  o.padded = true;
  const bool open = begin_scroll(id, ImVec2(inner.GetWidth(), std::max(1.0f, inner.Max.y - top)), o);
  ImGui::PopStyleVar();
  return open;
}

void end_page_scroll() {
  // The page's own margin under the last thing on it, once scrolled there.
  ImGui::Dummy(ImVec2(0, std::round(px(12))));
  end_scroll();
  ImGui::PopClipRect();
  ImGuiWindow* pw = ImGui::GetCurrentWindow();
  if (pw == g_page_scroll.page) {
    pw->DC.CursorMaxPos = g_page_scroll.max;
    pw->DC.IdealMaxPos = g_page_scroll.ideal;
  }
  g_page_scroll = PageScroll{};
}

// ---- navigation ----------------------------------------------------------------

namespace {

// Right from a sidebar row, when the page beside it has nothing level with
// that row (a row further down the column than a short grid reaches). ImGui
// only moves right to an item that overlaps the row's height, so on its own
// the key does nothing there.
struct SideRight {
  ImGuiID window = 0, item = 0;
  int frame = -1;
};
SideRight g_side_right;

// Notes each Right that starts from the sidebar; on the frame after one that
// found nothing, the focus still on the row, asks again from a rect as tall as
// the screen centred on the row. Every item beside the column then counts as
// level with it, the nearest column wins, and within it the item nearest the
// row's height. Asked only when the first try found nothing, so a row with an
// item beside it goes where it always did.
void sidebar_right_to_nearest(ImGuiWindow* self) {
  ImGuiContext& g = *GImGui;
  const int now = ImGui::GetFrameCount();
  if (g.NavWindow != self) return;
  if (g.NavMoveScoringItems) {
    if (g.NavMoveDir == ImGuiDir_Right && !(g.NavMoveFlags & ImGuiNavMoveFlags_FocusApi)) {
      g_side_right = SideRight{self->ID, g.NavId, now};
    }
    return;
  }
  const SideRight last = g_side_right;
  if (last.window != self->ID || last.frame != now - 1 || g.NavId != last.item || g.NavId == 0) return;
  g_side_right = SideRight{};
  const ImRect row = ImGui::WindowRectRelToAbs(self, self->NavRectRel[ImGuiNavLayer_Main]);
  if (row.IsInverted()) return;
  ImGui::NavMoveRequestSubmit(ImGuiDir_Right, ImGuiDir_Right, ImGuiNavMoveFlags_None, ImGuiScrollFlags_None);
  const float cy = row.GetCenter().y;
  const float reach = ImGui::GetMainViewport()->Size.y * 2.0f;
  g.NavScoringRect = ImRect(ImVec2(row.Max.x, cy - reach), ImVec2(row.Max.x, cy + reach));
  g.NavScoringNoClipRect = ImRect(+FLT_MAX, +FLT_MAX, -FLT_MAX, -FLT_MAX);
}

}  // namespace

int nav_sidebar(const char* id, const NavEntry* entries, int n, float width, float height) {
  const bool rail = breakpoint() == Breakpoint::Compact;
  if (width <= 0) width = sidebar_width();
  int chosen = -1;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBg1);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(px(8)), std::round(px(12))));
  const bool open = ImGui::BeginChild(id, ImVec2(width, height), ImGuiChildFlags_NavFlattened);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, std::round(px(2))));
  if (open) {
    ImFont* body = font(FontRole::Body);
    ImFont* small = font(FontRole::Small);
    const float row_h = std::round(px(rail ? 44 : 40));
    const float cell = std::round(px(28));
    const float bar = std::max(2.0f, std::round(px(3)));
    const float cap_h = keycap_size("A").y;
    for (int i = 0; i < n; ++i) {
      const NavEntry& e = entries[i];
      if (e.heading) {
        // A rule across the column, and the group's name under it in small
        // dim type, the way a console's library heads its collections.
        ImGui::Dummy(ImVec2(0, std::round(px(6))));
        const ImVec2 r = ImGui::GetCursorScreenPos();
        const float rw = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(r.x + std::round(px(6)), r.y), ImVec2(r.x + rw - std::round(px(6)), r.y + hairline()),
                          col(kLine));
        float h = hairline() + std::round(px(6));
        if (!rail && *e.heading) {
          dl->AddText(small, font_px(small), ImVec2(r.x + bar + std::round(px(8)), r.y + h), col(kDim), e.heading);
          h += font_px(small) + std::round(px(4));
        }
        ImGui::Dummy(ImVec2(rw, h));
      }
      ImGui::PushID(i);
      const ImVec2 a = ImGui::GetCursorScreenPos();
      const float rw = std::max(1.0f, ImGui::GetContentRegionAvail().x);
      if (ImGui::InvisibleButton(e.label, ImVec2(rw, row_h), ImGuiButtonFlags_EnableNav)) chosen = i;
      if (e.opens_here) default_focus();
      const bool hovered = ImGui::IsItemHovered();
      const ImVec2 b(a.x + rw, a.y + row_h);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      if (e.current) dl->AddRectFilled(a, b, col(kPanelRaised));
      else if (hovered) dl->AddRectFilled(a, b, col(kBg3));
      if (e.current) dl->AddRectFilled(a, ImVec2(a.x + bar, b.y), col(kAccent));
      const ImVec4 ink = e.current ? kAccent : kText;
      if (rail) {
        // The glyph and the key side by side, centred in the rail, where a
        // glyph at the left and a key at the right would leave neither in
        // line with the rows' centre.
        const bool has_key = e.key && *e.key;
        const float gw = e.glyph ? text_w(body, e.glyph) : 0.0f;
        const float kw = has_key ? keycap_w(small, e.key) : 0.0f;
        const float sep = gw > 0 && kw > 0 ? std::round(px(6)) : 0.0f;
        const float gx = std::round(a.x + bar + (rw - bar - (gw + sep + kw)) * 0.5f);
        if (e.glyph) {
          dl->AddText(body, font_px(body), ImVec2(gx, ink_centred_y(body, a.y, row_h)), col(e.current ? kAccent : kDim),
                      e.glyph);
        }
        if (has_key) {
          draw_keycap(dl, small, ImVec2(gx + gw + sep, std::round(a.y + (row_h - cap_h) * 0.5f)), cap_h, e.key,
                      col(kCyan, 0.85f));
        }
        if (hovered) ImGui::SetTooltip("%s", std::string(e.label, ImGui::FindRenderedTextEnd(e.label)).c_str());
        ImGui::PopID();
        continue;
      }
      float x = a.x + bar + std::round(px(8));
      if (e.glyph) {
        const float gw = text_w(body, e.glyph);
        dl->AddText(body, font_px(body), ImVec2(std::round(x + (cell - gw) * 0.5f), ink_centred_y(body, a.y, row_h)),
                    col(e.current ? kAccent : kDim), e.glyph);
      }
      x += cell + std::round(px(6));
      float right = b.x - std::round(px(8));
      if (e.key && *e.key) {
        const float kw = keycap_w(small, e.key);
        const float ky = std::round(a.y + (row_h - cap_h) * 0.5f);
        draw_keycap(dl, small, ImVec2(right - kw, ky), cap_h, e.key, col(kCyan, 0.85f));
        right -= kw + std::round(px(8));
      }
      if (e.badge >= 0) {
        const std::string n_s = std::to_string(e.badge);
        const float nw = text_w(small, n_s.c_str());
        dl->AddText(small, font_px(small), ImVec2(right - nw, ink_centred_y(small, a.y, row_h)), col(kDim), n_s.c_str());
        right -= nw + std::round(px(8));
      }
      const std::string label = elide(body, std::string(e.label, ImGui::FindRenderedTextEnd(e.label)), right - x);
      dl->AddText(body, font_px(body), ImVec2(x, ink_centred_y(body, a.y, row_h)), col(ink), label.c_str());
      ImGui::PopID();
    }
    // Up at the top goes round to the bottom, as a console's menu does.
    ImGuiWindow* self = ImGui::GetCurrentWindow();
    ImGui::NavMoveRequestTryWrapping(self, ImGuiNavMoveFlags_LoopY);
    // The column is reached from the page beside it by Left and Right only.
    // ImGui scores a row down the side as nearer than a tile straight below
    // one in a row of its own, so Down from a carousel would land in the
    // sidebar; the sidebar's rows are no answer to Up or Down from outside it.
    ImGuiContext& g = *GImGui;
    if (g.NavMoveScoringItems && g.NavWindow && g.NavWindow != self &&
        (g.NavMoveDir == ImGuiDir_Up || g.NavMoveDir == ImGuiDir_Down)) {
      for (ImGuiNavItemData* r : {&g.NavMoveResultLocal, &g.NavMoveResultOther, &g.NavMoveResultLocalVisible}) {
        if (r->Window == self) r->Clear();
      }
    }
    sidebar_right_to_nearest(self);
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  return chosen;
}

// ---- collections ---------------------------------------------------------------

void grid_begin() { push_layers(false); }

void grid_end() {
  pop_layers();
  wrap_rows();
}

bool raise_in_grid(ImDrawList* dl) {
  if (g_layers.empty()) return false;
  Layers& l = *g_layers.back();
  if (l.behind || l.dl != dl || l.frame != ImGui::GetFrameCount()) return false;
  l.split.SetCurrentChannel(dl, 1);
  return true;
}

void lower_in_grid(ImDrawList* dl) {
  if (g_layers.empty()) return;
  Layers& l = *g_layers.back();
  if (!l.behind && l.dl == dl) l.split.SetCurrentChannel(dl, 0);
}

namespace {

// The carousel being drawn: whether the next tile is its first, and the room
// at its left edge that a tile is brought to.
struct Row {
  bool first = true;
  float pad_x = 0;
};
Row g_row;

}  // namespace

bool carousel_begin(const char* id, float item_h, float grow) {
  // Room for a tile grown the usual 3.5% and its glow, and for the half of
  // any growth beyond that which falls on each side.
  const float more = std::max(0.0f, std::round(item_h * (grow - 0.035f) * 0.5f));
  const float pad_y = std::max(std::round(item_h * 0.03f), std::round(px(10))) + more;
  g_row = Row{true, std::round(px(12)) + std::round(more * 4.0f / 3.0f)};
  ScrollOpts o;
  o.x = true;
  o.y = false;
  o.no_scrollbar = true;
  // The room round the tiles is the row's own padding, which a child window
  // with no border drops unless asked to keep it; the row starts that much
  // to the left, so its first tile still lines up with what is above it.
  o.padded = true;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() - g_row.pad_x);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(g_row.pad_x, pad_y));
  const bool open = begin_scroll(id, ImVec2(0, std::round(item_h + pad_y * 2)), o);
  ImGui::PopStyleVar();
  push_layers(false);
  return open;
}

bool carousel_item(const TileSpec& spec) {
  if (!g_row.first) ImGui::SameLine(0, std::round(px(16)));
  g_row.first = false;
  const bool pressed = tile_ex(spec);
  // The focus arriving at a tile brings it to the row's left edge, so what
  // comes next is in view; ImGui then only scrolls as far as the row goes.
  // The tile a page opens on is put there too, so the first move from it
  // glides one tile along rather than from wherever ImGui scrolled it into
  // view.
  const ImGuiContext& g = *GImGui;
  const ImGuiID id = g.LastItemData.ID;
  if (id != 0 && (g.NavJustMovedToId == id || (page_age() <= 1 && g.NavId == id))) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImGui::SetScrollFromPosX(w, g.LastItemData.Rect.Min.x - w->Pos.x - g_row.pad_x, 0.0f);
  }
  return pressed;
}

void carousel_end() {
  pop_layers();
  end_scroll();
  g_row = Row{};
}

// ---- flows ---------------------------------------------------------------------

namespace {

// A step's node: a disc with its number, or a tick when done.
void draw_node(ImDrawList* dl, ImVec2 c, float r, int i, StepInfo info, float current) {
  ImFont* small = font(FontRole::Small);
  ImVec4 fill = kBg2, ring = kLine, ink = kDim;
  switch (info.state) {
    case StepState::Done:
      fill = kAccentDim;
      ring = kAccent;
      ink = kAccent;
      break;
    case StepState::Warn:
      ring = kWarm;
      ink = kWarm;
      break;
    case StepState::Fail:
      fill = alpha(kError, 0.25f);
      ring = kError;
      ink = kError;
      break;
    case StepState::Todo: break;
  }
  // The current step fills amber as it becomes current.
  fill = ImLerp(fill, kAmber, current);
  ring = ImLerp(ring, kAmber, current);
  ink = ImLerp(ink, kBg0, current);
  dl->AddCircleFilled(c, r, col(ring), 24);
  // The page under a fill that lets it through, so the ring stays a ring.
  const float inner = r - std::max(1.0f, std::round(px(1.5f)));
  dl->AddCircleFilled(c, inner, col(kBg0), 24);
  dl->AddCircleFilled(c, inner, col(fill), 24);
  if (info.state == StepState::Done && current < 0.5f) {
    const float t = std::max(1.5f, px(2));
    dl->AddLine(ImVec2(c.x - r * 0.42f, c.y + r * 0.02f), ImVec2(c.x - r * 0.1f, c.y + r * 0.34f), col(ink), t);
    dl->AddLine(ImVec2(c.x - r * 0.1f, c.y + r * 0.34f), ImVec2(c.x + r * 0.45f, c.y - r * 0.32f), col(ink), t);
    return;
  }
  const std::string num = info.state == StepState::Fail ? "!" : std::to_string(i + 1);
  const float nw = text_w(small, num.c_str());
  dl->AddText(small, font_px(small), ImVec2(std::round(c.x - nw * 0.5f), ink_centred_y(small, c.y - r, r * 2)), col(ink),
              num.c_str());
}

}  // namespace

int stepper(const char* id, const char* const* labels, int n, int current, const std::function<StepInfo(int)>& info,
            const std::function<bool(int)>& can_go, bool collapsed) {
  if (n <= 0) return -1;
  ImGui::PushID(id);
  ImFont* small = font(FontRole::Small);
  const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  const float r = std::round(px(11));
  const float gap = std::round(px(8));
  // Each step needs its node, its label and a stretch of rule; with less room
  // than that, the steps are a count and a bar.
  float need = 0;
  for (int i = 0; i < n; ++i) need += r * 2 + gap + text_w(small, labels[i]) + std::round(px(28));
  int picked = -1;
  if (collapsed || breakpoint() == Breakpoint::Compact || need > avail) {
    const int c = std::clamp(current, 0, n - 1);
    const std::string count = std::to_string(c + 1) + "/" + std::to_string(n);
    ImGui::TextColored(kAmber, "%s", count.c_str());
    ImGui::SameLine(0, ImGui::CalcTextSize(" ").x);
    // U+00B7, in every font's Latin-1.
    ImGui::TextColored(kDim, "\xc2\xb7");
    ImGui::SameLine(0, ImGui::CalcTextSize(" ").x);
    ImGui::TextUnformatted(labels[c]);
    ImGui::SameLine(0, std::round(px(16)));
    block_progress(static_cast<float>(c + 1) / static_cast<float>(n));
    ImGui::PopID();
    return -1;
  }
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float row_h = std::max(r * 2, ImGui::GetFrameHeight());
  // The steps spread so the first starts at the left margin and the last
  // ends at the right one.
  auto item_w_of = [&](int i) {
    std::string l = labels[i];
    const StepInfo si = info ? info(i) : StepInfo{};
    if (si.state == StepState::Warn && si.count > 0) l += " (" + std::to_string(si.count) + ")";
    return r * 2 + gap + text_w(small, l.c_str());
  };
  const float slot = n > 1 ? (avail - item_w_of(n - 1)) / static_cast<float>(n - 1) : avail;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cy = std::round(at.y + row_h * 0.5f);
  for (int i = 0; i < n; ++i) {
    const StepInfo si = info ? info(i) : StepInfo{};
    const float x0 = std::round(at.x + slot * static_cast<float>(i));
    const float cur = anim01(ImGui::GetID(i), i == current, 0.10f);
    std::string label = labels[i];
    if (si.state == StepState::Warn && si.count > 0) label += " (" + std::to_string(si.count) + ")";
    const float lw = text_w(small, label.c_str());
    const float item_w = r * 2 + gap + lw;
    // A step that may be gone to is a button over its node and label.
    ImGui::SetCursorScreenPos(ImVec2(x0, at.y));
    ImGui::PushID(i);
    const bool can = i != current && can_go && can_go(i);
    if (can) {
      if (ImGui::InvisibleButton(labels[i], ImVec2(item_w, row_h), ImGuiButtonFlags_EnableNav)) picked = i;
    } else {
      ImGui::Dummy(ImVec2(item_w, row_h));
    }
    const bool hovered = can && ImGui::IsItemHovered();
    ImGui::PopID();
    draw_node(dl, ImVec2(x0 + r, cy), r, i, si, cur);
    const ImVec4 ink = i == current ? kText : si.state == StepState::Done ? kAccent
                                          : si.state == StepState::Warn   ? kWarm
                                          : si.state == StepState::Fail   ? kError
                                                                          : kDim;
    dl->AddText(small, font_px(small), ImVec2(x0 + r * 2 + gap, ink_centred_y(small, at.y, row_h)),
                col(hovered ? kText : ink), label.c_str());
    // The rule on to the next step.
    if (i + 1 < n) {
      const float rx0 = x0 + item_w + gap, rx1 = std::round(at.x + slot * static_cast<float>(i + 1)) - gap;
      if (rx1 > rx0) {
        dl->AddRectFilled(ImVec2(rx0, cy), ImVec2(rx1, cy + hairline()),
                          col(si.state == StepState::Done ? kAccentDim : kLine));
      }
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(at.x, at.y));
  ImGui::Dummy(ImVec2(avail, row_h));
  ImGui::PopID();
  return picked;
}

void step_heading(const std::string& title) {
  ImFont* big = font(FontRole::Big);
  const float size = font_px(big);
  const float prompt_w = std::max(2.0f, std::round(px(5)));
  const float prompt_gap = std::round(px(12));
  const float cursor_w = std::round(size * 0.5f);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float room = ImGui::GetContentRegionAvail().x - prompt_w - prompt_gap - cursor_w - std::round(px(8));
  const std::string shown = elide(big, title, std::max(1.0f, room));
  const float tx = at.x + prompt_w + prompt_gap;
  const float tw = text_w(big, shown.c_str());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float inset = std::round(size * 0.12f);
  dl->AddRectFilled(ImVec2(at.x, at.y + inset), ImVec2(at.x + prompt_w, at.y + size - inset), col(kAccent));
  dl->AddText(big, size, ImVec2(tx, at.y), col(kText), shown.c_str());
  // The cursor blinks as the page title's does, and holds still when motion
  // is off.
  const float t = static_cast<float>(ImGui::GetTime());
  const float blink = reduce_motion() ? 1.0f : 0.5f + 0.5f * std::cos(t * 2.0f * 3.14159265f);
  const float cx = tx + tw + std::round(px(6));
  dl->AddRectFilled(ImVec2(cx, at.y + inset), ImVec2(cx + cursor_w, at.y + size - inset),
                    col(kAccentDim, 0.25f + 0.75f * blink));
  ImGui::Dummy(ImVec2(std::max(1.0f, prompt_w + prompt_gap + tw + cursor_w), size));
  ImGui::Dummy(ImVec2(0, std::round(px(4))));
}

float flow_footer_height() { return std::round(px(64)); }

FlowAction flow_footer(const FlowFooter& f) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  const float h = flow_footer_height();
  const float bottom = w->Pos.y + ImGui::GetWindowContentRegionMax().y;
  const ImVec2 cur = ImGui::GetCursorScreenPos();
  const float y = std::max(cur.y, bottom - h);
  const float left = cur.x, right = w->Pos.x + ImGui::GetWindowContentRegionMax().x;
  const ImRect line = full_width(y, y + hairline());
  w->DrawList->AddRectFilled(line.Min, line.Max, col(kLine));
  const float bh = std::round(px(44));
  const float by = std::round(y + (h - bh) * 0.5f);
  FlowAction act = FlowAction::None;
  const float gap = std::round(px(12));
  // The back button's width, known before it is laid out: it is laid out
  // last (see FlowFooter), and the words between it and the action stop
  // short of it.
  const float back_pad = std::round(px(16));
  const float back_w = f.back ? std::round(ImGui::CalcTextSize(f.back, nullptr, true).x + back_pad * 2) : 0.0f;
  const float back_right = left + back_w;
  float x = right;
  if (f.primary) {
    // The step's own action, a solid block with the play glyph after its
    // label: the one way on.
    ImFont* bf = font(FontRole::Body);
    const char* end = ImGui::FindRenderedTextEnd(f.primary);
    const float pad = std::round(px(20)), glyph = std::round(font_px(bf) * 0.7f);
    const float pw = std::round(text_w(bf, f.primary, end) + pad * 2 + gap + glyph);
    x -= pw;
    ImGui::SetCursorScreenPos(ImVec2(x, by));
    // One that cannot be taken yet is disabled, as the button it stands for
    // was, so the keys and the pad pass it by.
    ImGui::BeginDisabled(!f.primary_enabled);
    const bool pressed = ImGui::InvisibleButton(f.primary, ImVec2(pw, bh), ImGuiButtonFlags_EnableNav);
    if (f.primary_default) default_focus();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    const ImVec4 fill = !f.primary_enabled ? kLine : held  ? kAccentDim
                                                 : hovered ? kPlayHover
                                                           : kAccent;
    const ImVec4 ink = f.primary_enabled ? kPlayText : kDim;
    ImDrawList* dl = w->DrawList;
    const ImVec2 a(x, by), b(x + pw, by + bh);
    dl->AddRectFilled(a, b, col(fill), px(2));
    dl->AddText(bf, font_px(bf), ImVec2(a.x + pad, ink_centred_y(bf, a.y, bh)), col(ink), f.primary, end);
    const float gx = b.x - pad - glyph, gy = std::round(a.y + (bh - glyph) * 0.5f);
    draw_play_glyph(dl, ImVec2(gx, gy), ImVec2(gx + glyph, gy + glyph), col(ink));
    ImGui::EndDisabled();
    if (pressed && f.primary_enabled) act = FlowAction::Primary;
    x -= gap;
  }
  if (f.secondary) {
    const float sw = ImGui::CalcTextSize(f.secondary, nullptr, true).x + std::round(px(16)) * 2;
    x -= sw;
    ImGui::SetCursorScreenPos(ImVec2(x, by));
    if (ghost_button(f.secondary, ImVec2(sw, bh))) act = FlowAction::Secondary;
    x -= gap;
  }
  // Why the action cannot be taken, or a word about the step, in the room
  // between Back and the actions.
  const char* words = !f.primary_enabled && f.reason && *f.reason ? f.reason : f.primary_enabled ? f.note
                                                                                                 : nullptr;
  if (words && *words) {
    ImFont* small = font(FontRole::Small);
    const float room = x - back_right - gap * 2;
    if (room > px(40)) {
      const std::string why = elide(small, words, room);
      w->DrawList->AddText(small, font_px(small), ImVec2(x - text_w(small, why.c_str()), ink_centred_y(small, by, bh)),
                           col(kDim), why.c_str());
    }
  }
  if (f.back) {
    ImGui::PushID("##flow-footer");
    ImGui::SetCursorScreenPos(ImVec2(left, by));
    if (ghost_button(f.back, ImVec2(back_w, bh))) act = FlowAction::Back;
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(ImVec2(left, y + h));
  ImGui::Dummy(ImVec2(0, 0));
  return act;
}

// ---- cards ---------------------------------------------------------------------

void card_begin(const char* id, const char* title) {
  ImGui::PushID(id);
  Layers& l = push_layers(true);
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  l.start = ImGui::GetCursorScreenPos();
  l.pad = std::round(px(16));
  l.work_max = w->WorkRect.Max.x;
  l.content_max = w->ContentRegionRect.Max.x;
  // The card spans the room left on the line; what is in it stays inside its
  // padding, wrapping and stretching there.
  const float right = l.start.x + ImGui::GetContentRegionAvail().x;
  l.right = right;
  w->WorkRect.Max.x = std::min(w->WorkRect.Max.x, right - l.pad);
  w->ContentRegionRect.Max.x = std::min(w->ContentRegionRect.Max.x, right - l.pad);
  ImGui::Indent(l.pad);
  ImGui::Dummy(ImVec2(0, l.pad - ImGui::GetStyle().ItemSpacing.y));
  if (title) section(title);
}

void card_end() {
  if (g_layers.empty()) {
    ImGui::PopID();
    return;
  }
  Layers& l = *g_layers.back();
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  ImGui::Unindent(l.pad);
  const float right = l.right;
  w->WorkRect.Max.x = l.work_max;
  w->ContentRegionRect.Max.x = l.content_max;
  const float bottom = w->DC.CursorPos.y - ImGui::GetStyle().ItemSpacing.y + l.pad;
  l.split.SetCurrentChannel(l.dl, 0);
  l.dl->AddRectFilled(l.start, ImVec2(right, bottom), col(kBg1));
  frame_rect(l.dl, l.start, ImVec2(right, bottom), col(kLine), hairline());
  const ImVec2 start = l.start;
  pop_layers();
  ImGui::SetCursorScreenPos(ImVec2(start.x, bottom));
  ImGui::Dummy(ImVec2(right - start.x, 0));
  ImGui::PopID();
}

namespace {

// The centred card being drawn: where its height is kept, and its top.
struct CentredCard {
  bool open = false;
  ImGuiID key = 0;
  float top = 0;
};
CentredCard g_centred;

}  // namespace

void centred_card_begin(const char* id, float design_w, const char* title) {
  // Cards do not nest; one still open is left over from a page that threw.
  g_centred = CentredCard{};
  ImGuiStorage* store = ImGui::GetStateStorage();
  const ImGuiID key = ImGui::GetID(id) ^ 0x63617264u;
  // Down the page by last frame's height, a little above the middle, where a
  // message is read; the first frame it is drawn from the top, under the fade
  // the page comes up with.
  const float h = store->GetFloat(key, 0.0f);
  const float room = ImGui::GetContentRegionAvail().y;
  if (h > 0.0f && room > h) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((room - h) * 0.45f));
  g_centred.open = true;
  g_centred.key = key;
  g_centred.top = ImGui::GetCursorScreenPos().y;
  centre_column(std::round(px(design_w)));
  card_begin(id, title);
}

void centred_card_end() {
  card_end();
  end_centre_column();
  if (!g_centred.open) return;
  ImGui::GetStateStorage()->SetFloat(g_centred.key, ImGui::GetCursorScreenPos().y - g_centred.top);
  g_centred = CentredCard{};
}

// ---- snapshots -----------------------------------------------------------------

bool snapshot_card(const SnapshotCard& c, const char* button) {
  const ImGuiStyle& st = ImGui::GetStyle();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* f = ImGui::GetFont();
  const float fs = ImGui::GetFontSize();
  const bool compact = breakpoint() == Breakpoint::Compact;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float avail = c.width > 0 ? std::min(c.width, ImGui::GetContentRegionAvail().x) : ImGui::GetContentRegionAvail().x;
  const float rail = std::round(px(28)), pad = std::round(px(12)), gap = std::round(px(16));
  const float min_h = std::round(px(96)), between = std::round(px(10));
  // The picture fills the card's least height, in the shape of a screen; on
  // compact the words need the room more.
  const float thumb_h = min_h - pad * 2;
  const float thumb_w = compact ? 0.0f : std::round(thumb_h * 16.0f / 9.0f);
  const float x0 = at.x + rail, x1 = at.x + avail;
  const float bw = std::round(ImGui::CalcTextSize(button, nullptr, true).x + px(16) * 2);
  const float bh = ghost_h();
  const float tx = x0 + pad + (thumb_w > 0 ? thumb_w + gap : 0.0f);
  const float room = std::max(std::round(px(40)), x1 - pad - bw - gap - tx);
  const float line = ImGui::GetTextLineHeight(), lead = std::round(px(4));

  // What it says, measured before anything is drawn, so the card is as tall
  // as its words.
  std::string facts = c.played.empty() ? std::string() : "played " + c.played;
  if (!c.files.empty()) facts += (facts.empty() ? "" : " \xc2\xb7 ") + c.files;
  const std::vector<std::string> note =
      c.note.empty() ? std::vector<std::string>{} : wrap_lines(f, "\"" + c.note + "\"", room);
  float text_h = line;
  if (!facts.empty()) text_h += lead + line;
  if (!note.empty()) text_h += lead + static_cast<float>(note.size()) * line;
  const float h = std::max(min_h, std::round(text_h + pad * 2));
  const ImVec2 a(x0, at.y), b(x1, at.y + h);

  // The rail: a line through the column and a node level with each card's
  // heading, the newest lit.
  const float thin = hairline();
  const float cx = std::round(at.x + rail * 0.4f), cy = std::round(at.y + min_h * 0.5f);
  const float node = std::round(px(4));
  const float y_from = c.first ? cy : at.y, y_to = c.last ? cy : at.y + h + between;
  const float lx = cx - std::floor(thin * 0.5f);
  if (y_to > y_from) dl->AddRectFilled(ImVec2(lx, y_from), ImVec2(lx + thin, y_to), col(kFrameLine));
  dl->AddRectFilled(ImVec2(cx - node, cy - node), ImVec2(cx + node, cy + node), col(c.first ? kAccent : kAccentDim));

  dl->AddRectFilled(a, b, col(kBg1));
  frame_rect(dl, a, b, col(kLine), thin);

  if (thumb_w > 0) {
    const ImVec2 p(x0 + pad, at.y + pad), q(x0 + pad + thumb_w, at.y + pad + thumb_h);
    if (c.art && c.art->w > 0 && c.art->h > 0) {
      ImVec2 uv0, uv1;
      cover_uv(static_cast<float>(c.art->w), static_cast<float>(c.art->h), thumb_w, thumb_h, &uv0, &uv1);
      draw_image(dl, *c.art, p, q, uv0, uv1, col(ImVec4(1, 1, 1, 1)));
    } else {
      // No picture of the game as it was: its tint, and the snapshot's
      // number, as a tile with no picture has its name.
      dl->AddRectFilled(p, q, faded(tile_colour(c.id), ImGui::GetStyle().Alpha));
      ImFont* small = font(FontRole::Small);
      const float ss = font_px(small);
      const float w = text_w(small, c.gen.c_str());
      dl->AddText(small, ss, ImVec2(std::round((p.x + q.x - w) * 0.5f), ink_centred_y(small, p.y, thumb_h)), col(kDim),
                  c.gen.c_str());
    }
    frame_rect(dl, p, q, col(kLine), thin);
  }

  // The words, centred down the card: "snapshot 0004 · 3 days ago", then what
  // the session did, then its note.
  float y = std::round(at.y + (h - text_h) * 0.5f);
  float x = tx;
  auto put = [&](const char* s, const ImVec4& colour) {
    const std::string shown = elide(f, s, std::max(1.0f, tx + room - x));
    dl->AddText(f, fs, ImVec2(x, y), col(colour), shown.c_str());
    x += text_w(f, shown.c_str());
  };
  put("snapshot ", kDim);
  put(c.gen.c_str(), kCyan);
  if (!c.when.empty()) {
    put(" \xc2\xb7 ", kDim);
    put(c.when.c_str(), kText);
  }
  if (!facts.empty()) {
    y += line + lead;
    x = tx;
    put(facts.c_str(), kDim);
  }
  if (!note.empty()) y += lead;
  for (const std::string& l : note) {
    y += line;
    dl->AddText(f, fs, ImVec2(tx, y), col(kWarm), l.c_str());
  }

  ImGui::SetCursorScreenPos(ImVec2(x1 - pad - bw, std::round(at.y + (h - bh) * 0.5f)));
  const bool pressed = ghost_button(button, ImVec2(bw, bh));
  ImGui::SetCursorScreenPos(at);
  ImGui::Dummy(ImVec2(avail, h + between - st.ItemSpacing.y));
  return pressed;
}

// ---- settings ------------------------------------------------------------------

namespace {

// The settings row being drawn.
struct SettingRow {
  bool open = false;
  ImVec2 start;
  float right = 0, pad = 0, label_bottom = 0;
};
SettingRow g_setting;

}  // namespace

void setting_begin(const char* name, const char* help, const ImVec4& colour) {
  g_setting = SettingRow{};
  g_setting.open = true;
  g_setting.start = ImGui::GetCursorScreenPos();
  const float avail = ImGui::GetContentRegionAvail().x;
  g_setting.right = g_setting.start.x + avail;
  g_setting.pad = std::round(px(10));
  const bool compact = breakpoint() == Breakpoint::Compact || avail < px(640);
  const float ctrl_w = compact ? avail : std::min(std::round(px(360)), std::floor(avail * 0.55f));
  const float label_w = compact ? avail : avail - ctrl_w - std::round(px(24));

  // The name on the line of the control beside it, its help under it.
  ImGui::SetCursorScreenPos(ImVec2(g_setting.start.x, g_setting.start.y + g_setting.pad));
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + label_w);
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextWrapped("%s", name);
  ImGui::PopStyleColor();
  if (help && *help) {
    ImGui::PushFont(font(FontRole::Small));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", help);
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  g_setting.label_bottom = ImGui::GetItemRectMax().y;

  const ImVec2 ctrl = compact ? ImVec2(g_setting.start.x, g_setting.label_bottom + std::round(px(6)))
                              : ImVec2(g_setting.right - ctrl_w, g_setting.start.y + g_setting.pad);
  ImGui::SetCursorScreenPos(ctrl);
  ImGui::BeginGroup();
  ImGui::SetNextItemWidth(ctrl_w);
}

void setting_end() {
  ImGui::EndGroup();
  if (!g_setting.open) return;
  const float bottom = std::max({ImGui::GetItemRectMax().y + g_setting.pad, g_setting.label_bottom + g_setting.pad,
                                 g_setting.start.y + std::round(px(52))});
  // A hairline under the row, across the column, so a list of settings reads
  // as rows the eye can follow from a name to its control.
  const float thin = hairline();
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(g_setting.start.x, bottom), ImVec2(g_setting.right, bottom + thin), col(kLine));
  ImGui::SetCursorScreenPos(ImVec2(g_setting.start.x, bottom + thin));
  ImGui::Dummy(ImVec2(g_setting.right - g_setting.start.x, 0.0f));
  g_setting = SettingRow{};
}

bool step_combo(const char* label, int* current, const char* const* items, int n) {
  bool changed = ImGui::Combo(label, current, items, n);
  // Only while the keys are on it: a mouse over it scrolls and clicks as ever.
  if (n > 0 && nav_focused()) {
    int step = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)) {
      step = -1;
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
               ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight)) {
      step = 1;
    }
    if (step != 0) {
      // The key is the combo's: the focus stays put rather than moving to
      // whatever is beside it.
      ImGui::NavMoveRequestCancel();
      const int next = std::clamp(*current + step, 0, n - 1);
      if (next != *current) {
        *current = next;
        changed = true;
      }
    }
  }
  return changed;
}

void step_labels(const char* const* names, int top, int chosen) {
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  // ImGui keeps the grab 2 pixels in from either end.
  const float inner = b.x - a.x - 4.0f;
  const float cell = std::max(inner / static_cast<float>(top + 1), ImGui::GetStyle().GrabMinSize);
  const float step = top > 0 ? (inner - cell) / static_cast<float>(top) : 0.0f;
  ImFont* small = font(FontRole::Small);
  const float size = font_px(small);
  const float y = b.y + std::round(px(3));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int i = 0; i <= top; ++i) {
    const float w = small->CalcTextSizeA(size, FLT_MAX, 0.0f, names[i]).x;
    const float cx = a.x + 2.0f + cell * 0.5f + step * static_cast<float>(i);
    dl->AddText(small, size, ImVec2(std::round(cx - w * 0.5f), y), u32(i == chosen ? kAccent : kDim), names[i]);
  }
  ImGui::Dummy(ImVec2(0, std::round(size + px(3)) - ImGui::GetStyle().ItemSpacing.y));
}

// ---- misc ----------------------------------------------------------------------

void pad_glyph(const char* button) {
  ImFont* f = font(FontRole::Small);
  const float h = keycap_size("A").y;
  const float w = pad_glyph_w(f, button, h);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float y = at.y + std::round((ImGui::GetTextLineHeight() - h) * 0.5f);
  draw_pad_glyph(ImGui::GetWindowDrawList(), f, ImVec2(at.x, y), h, button, ImGui::GetStyle().Alpha);
  ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
}

}  // namespace kg::gui
