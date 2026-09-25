#include "focus.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "anim.h"
#include "imgui_internal.h"
#include "palette.h"
#include "scale.h"

namespace kg::gui {

namespace {

// A window comes up with the keyboard in mind: the focus is shown from the
// first frame, so the first arrow pressed moves it rather than finding it.
InputMode g_mode = InputMode::Keyboard;

// The page being drawn, and what it has said about its focus this frame.
struct PageFocus {
  ImGuiWindow* window = nullptr;
  // The key its focus is remembered under: the window, or the window and
  // the key the page set.
  ImGuiID key = 0;
  bool appearing = false;
  // Whether the focus is this page's to place this frame: it came up, and
  // no modal came up with it.
  bool placing = false;
  // The remembered item being tried, and what the focus was before it.
  ImGuiID restore = 0, before = 0;
  // The page's default, as default_focus() found it.
  ImGuiID def = 0;
  ImGuiWindow* def_window = nullptr;
  ImRect def_rect;
  ImGuiID def_scope = 0;
  ImGuiItemFlags def_flags = 0;
  // Whether default_focus_next() asked ImGui for a first item, and what
  // ImGui had chosen before it did.
  bool next = false;
  bool saved_request = false;
  ImGuiNavItemData saved_result;
};
PageFocus g_page;
// Where the focus was on each page when it was last drawn, by key.
std::unordered_map<ImGuiID, ImGuiID> g_memory;
// The key each page window gave last, to see it change.
std::unordered_map<ImGuiID, ImGuiID> g_last_key;
// The key set for the next page.
std::string g_next_key;
// The frame the page last drawn came up on.
int g_page_frame = -1000;

// Whether a field is being typed in, or was until this frame: the Escape that
// leaves a field has already made it inactive by the time a modal drawn after
// it asks.
bool typing() {
  const ImGuiContext& g = *GImGui;
  const ImGuiID field = g.InputTextState.ID;
  if (field == 0) return false;
  return g.ActiveId == field || g.ActiveIdPreviousFrame == field;
}

// Stops trying the remembered item, and puts back what the focus would have
// been without it.
void drop_restore() {
  ImGuiContext& g = *GImGui;
  if (g_page.restore == 0) return;
  if (g.NavId == g_page.restore) {
    g.NavId = g_page.before;
    g.NavIdIsAlive = false;
  }
  g_page.restore = 0;
}

}  // namespace

InputMode input_mode() { return g_mode; }

void set_input_mode(InputMode mode) {
  g_mode = mode;
  // ImGui hides the focus again itself when the mouse is clicked; all that
  // is needed here is to keep it shown while keys and buttons are in use.
  if (ImGui::GetCurrentContext()) ImGui::GetIO().ConfigNavCursorVisibleAlways = mode != InputMode::Mouse;
}

bool page_appearing() { return g_page.appearing; }

void set_page_focus_key(const std::string& key) { g_next_key = key; }

void page_focus_begin() {
  ImGuiContext& g = *GImGui;
  ImGuiWindow* w = g.CurrentWindow;
  g_page = PageFocus{};
  g_page.window = w;
  g_page.key = g_next_key.empty() ? w->ID : ImHashStr(g_next_key.c_str(), 0, w->ID);
  g_next_key.clear();
  const auto last = g_last_key.find(w->ID);
  const bool changed = last != g_last_key.end() && last->second != g_page.key;
  g_last_key[w->ID] = g_page.key;
  g_page.appearing = w->Appearing || changed;
  if (g_page.appearing) g_page_frame = g.FrameCount;
  // A modal that came up with the page keeps the focus it has.
  if (!g_page.appearing || !g.NavWindow || g.NavWindow->RootWindow != w) return;
  g_page.placing = true;
  // The remembered item is tried by making it the focus for the frame: ImGui
  // marks the focus alive when the item is drawn, which says whether it is
  // still there, and page_focus_end keeps it or puts things back.
  const auto m = g_memory.find(g_page.key);
  if (m != g_memory.end() && m->second != 0) {
    g_page.restore = m->second;
    g_page.before = g.NavId;
    g.NavId = g_page.restore;
    g.NavIdIsAlive = false;
  }
}

void default_focus(bool over_memory) {
  ImGuiContext& g = *GImGui;
  if (!g_page.placing) return;
  const ImGuiLastItemData& item = g.LastItemData;
  if (item.ID == 0 || (item.ItemFlags & ImGuiItemFlags_NoNav) || (item.ItemFlags & ImGuiItemFlags_Disabled)) return;
  if (!g.CurrentWindow || g.CurrentWindow->RootWindow != g_page.window) return;
  if (over_memory) drop_restore();
  g_page.def = item.ID;
  g_page.def_window = g.CurrentWindow;
  g_page.def_rect = item.NavRect;
  g_page.def_scope = g.CurrentFocusScopeId;
  g_page.def_flags = item.ItemFlags;
}

void default_focus_next() {
  ImGuiContext& g = *GImGui;
  if (!g_page.placing || g_page.def != 0) return;
  // ImGui's own request for a window's first item, made afresh from here:
  // what was drawn before this point is no longer a candidate, unless nothing
  // after it can take the focus.
  g_page.next = true;
  g_page.saved_request = g.NavInitRequest;
  g_page.saved_result = g.NavInitResult;
  g.NavInitRequest = true;
  g.NavInitRequestFromMove = false;
  g.NavInitResult.ID = 0;
  g.NavAnyRequest = true;
}

namespace {

// The sections of the page being drawn, and where the focus is to go in one
// of them on the frame after Tab or a shoulder button asked.
struct Sections {
  // This frame's, in the order drawn, and which of them holds the focus.
  std::vector<ImGuiID> order;
  int current = -1;
  // The section being drawn: its key, whether the focus was already found
  // alive before it began, and the remembered item being tried in it, with
  // what the focus was before.
  ImGuiID open = 0;
  bool alive_before = false;
  ImGuiID trying = 0, trying_before = 0;
  // The section the focus goes to next frame, and whether to its first item
  // rather than the one remembered.
  ImGuiID pending = 0;
  bool pending_first = false;
};
Sections g_sections;
// The item the focus was last on in each section, by page and section.
std::unordered_map<ImGuiID, ImGuiID> g_section_memory;

// Tab, Shift+Tab and the shoulder buttons, read once a frame at the end of a
// page that has sections. Tab is claimed from ImGui, whose own Tab would
// otherwise walk item by item at the same time.
void cycle_sections() {
  ImGuiContext& g = *GImGui;
  const int n = static_cast<int>(g_sections.order.size());
  if (n >= 2 && g_page.window) {
    const ImGuiID owner = ImHashStr("##sections", 0, g_page.key);
    ImGui::SetKeyOwner(ImGuiKey_Tab, owner);
    int step = 0;
    if (!g.IO.KeyCtrl && !g.IO.KeyAlt && back_allowed()) {
      if (ImGui::IsKeyPressed(ImGuiKey_Tab, ImGuiInputFlags_Repeat, owner)) step = g.IO.KeyShift ? -1 : 1;
      if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) step = -1;
      if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) step = 1;
    }
    if (step != 0) {
      const int from = g_sections.current;
      const int to = from < 0 ? (step > 0 ? 0 : n - 1) : (from + step + n) % n;
      g_sections.pending = g_sections.order[static_cast<size_t>(to)];
      g_sections.pending_first = false;
    }
  }
  g_sections.order.clear();
  g_sections.current = -1;
}

}  // namespace

void nav_section_begin(const char* id) {
  ImGuiContext& g = *GImGui;
  Sections& s = g_sections;
  s.open = ImHashStr(id, 0, g_page.key);
  s.order.push_back(s.open);
  // Whether the focus is in this section is whether ImGui finds its item
  // alive while the section is drawn, so the flag starts over here and what
  // it was is put back at the end.
  s.alive_before = g.NavIdIsAlive;
  g.NavIdIsAlive = false;
  s.trying = 0;
  if (s.pending != s.open) return;
  s.pending = 0;
  const auto m = g_section_memory.find(s.open);
  ImGuiWindow* w = g.CurrentWindow;
  if (!s.pending_first && m != g_section_memory.end() && g.NavWindow &&
      g.NavWindow->RootWindowForNav == w->RootWindowForNav) {
    // Tried the way a page's remembered item is: made the focus for the
    // frame, kept if ImGui finds it drawn.
    s.trying = m->second;
    s.trying_before = g.NavId;
    g.NavId = s.trying;
  } else {
    // The item the focus is leaving must not be found again this frame:
    // ImGui takes the window it is drawn in as the focus's window again when
    // it is, which drops the request for the section's first item.
    g.NavId = 0;
    ImGui::SetKeyboardFocusHere();
    ImGui::SetNavCursorVisible(true);
  }
}

void nav_section_end() {
  ImGuiContext& g = *GImGui;
  Sections& s = g_sections;
  if (s.open == 0) return;
  bool in = g.NavIdIsAlive;
  if (s.trying != 0) {
    if (in && g.NavId == s.trying && g.NavWindow) {
      ImGui::NavClearPreferredPosForAxis(ImGuiAxis_X);
      ImGui::NavClearPreferredPosForAxis(ImGuiAxis_Y);
      ImGui::SetNavCursorVisible(true);
      ImGui::ScrollToRectEx(g.NavWindow, ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]),
                            ImGuiScrollFlags_KeepVisibleEdgeX | ImGuiScrollFlags_KeepVisibleEdgeY);
    } else {
      // Gone since: the section's first item next frame instead.
      g.NavId = s.trying_before;
      in = false;
      s.pending = s.open;
      s.pending_first = true;
    }
    s.trying = 0;
  }
  if (in) {
    s.current = static_cast<int>(s.order.size()) - 1;
    g_section_memory[s.open] = g.NavId;
  }
  g.NavIdIsAlive = in || s.alive_before;
  s.open = 0;
}

namespace {
// The window the focus was in at the end of the last page drawn.
ImGuiWindow* g_last_nav_window = nullptr;
}  // namespace

void page_focus_end() {
  ImGuiContext& g = *GImGui;
  cycle_sections();
  // ImGui keeps where along the other axis a run of moves started, to go
  // straight on across rows of different widths, but it keeps it relative
  // to the window the focus was in and reads it back relative to the one it
  // is in now. After a move from one flattened child into another (a sidebar
  // into the page's header), the two start at different places, and the
  // next Left or Right is aimed as though from well above the item, so it
  // finds nothing. Such a move starts a new run.
  if (g.NavJustMovedToId != 0 && g.NavWindow && g_last_nav_window && g.NavWindow != g_last_nav_window &&
      g.NavWindow->RootWindowForNav == g_page.window) {
    ImGui::NavClearPreferredPosForAxis(ImGuiAxis_X);
    ImGui::NavClearPreferredPosForAxis(ImGuiAxis_Y);
  }
  g_last_nav_window = g.NavWindow;
  if (!g_page.placing) {
    // Remembered only while the focus is in this page, not in a popup over it.
    if (g.NavId != 0 && g.NavWindow && g.NavWindow->RootWindow == g_page.window) g_memory[g_page.key] = g.NavId;
    return;
  }
  g_page.placing = false;
  // A popup that opened over the page as it came up has the focus, and its
  // own first item or default. The page's default is where the focus goes
  // back to when the popup closes.
  if (!g.NavWindow || g.NavWindow->RootWindow != g_page.window) {
    drop_restore();
    if (g_page.def != 0) {
      g_page.window->NavLastIds[0] = g_page.def;
      g_page.window->NavRectRel[0] = ImGui::WindowRectAbsToRel(g_page.window, g_page.def_rect);
    }
    return;
  }
  if (g_page.restore != 0 && g.NavId == g_page.restore && g.NavIdIsAlive) {
    // Back where it was. ImGui's own choice for a window just shown is
    // dropped, and the item brought into view if the page scrolled.
    g.NavInitRequest = false;
    g.NavInitResult.ID = 0;
    if (g.NavWindow) {
      ImGui::ScrollToRectEx(g.NavWindow, ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]),
                            ImGuiScrollFlags_None);
    }
    return;
  }
  drop_restore();
  if (g_page.def == 0) {
    if (!g_page.next) return;
    ImGuiNavItemData& r = g.NavInitResult;
    if (r.ID == 0) {
      // Nothing after default_focus_next() could take the focus: ImGui's
      // own choice stands.
      g.NavInitRequest = g_page.saved_request;
      r = g_page.saved_result;
    } else if (r.Window) {
      const ImRect at = ImGui::WindowRectRelToAbs(r.Window, r.RectRel);
      if (!r.Window->ClipRect.Contains(at)) ImGui::ScrollToRectEx(r.Window, at, ImGuiScrollFlags_None);
    }
    return;
  }
  // As SetItemDefaultFocus has it: the result ImGui applies at the start of
  // the next frame, as it does its own first item.
  g.NavInitRequest = false;
  ImGuiNavItemData& r = g.NavInitResult;
  r.Window = g_page.def_window;
  r.ID = g_page.def;
  r.FocusScopeId = g_page.def_scope;
  r.ItemFlags = g_page.def_flags;
  r.RectRel = ImGui::WindowRectAbsToRel(g_page.def_window, g_page.def_rect);
  r.SelectionUserData = ImGuiSelectionUserData_Invalid;
  if (!g_page.def_window->ClipRect.Contains(g_page.def_rect)) {
    ImGui::ScrollToRectEx(g_page.def_window, g_page.def_rect, ImGuiScrollFlags_None);
  }
}

void forget_focus() {
  g_memory.clear();
  g_last_key.clear();
  g_next_key.clear();
  g_page = PageFocus{};
  g_sections = Sections{};
  g_section_memory.clear();
}

bool nav_focused() { return ImGui::IsItemFocused() && GImGui->NavCursorVisible; }

int page_age() { return GImGui ? GImGui->FrameCount - g_page_frame : 0; }

bool focus_state(ImGuiID id, float* t) {
  const ImGuiContext& g = *GImGui;
  const bool on = id != 0 && g.NavId == id && g.NavCursorVisible;
  if (t) *t = anim01(id, on, 0.10f);
  return on;
}

namespace {

// What an item said about its glow this frame.
struct GlowNote {
  ImGuiID id = 0;
  int frame = -1;
  bool off = false;
  ImRect rect;
};
GlowNote g_glow_note;

// Where the glow was drawn last frame and round what, and how far the one
// drawn now still is from its item, so it can glide the rest of the way.
struct Glide {
  bool have = false;
  ImGuiID id = 0;
  ImGuiWindow* root = nullptr;
  ImRect drawn;
  ImVec2 off_min, off_max;
};
Glide g_glide;

}  // namespace

void focus_glow_rect(ImVec2 a, ImVec2 b) {
  const ImGuiContext& g = *GImGui;
  // Only the focused item's note is kept: a row of tiles each gives one, and
  // the last tile's would otherwise stand in for the focused one's.
  if (g.LastItemData.ID != g.NavId) return;
  g_glow_note = GlowNote{g.LastItemData.ID, g.FrameCount, false, ImRect(a, b)};
}

void own_focus_ring() {
  const ImGuiContext& g = *GImGui;
  g_glow_note = GlowNote{g.LastItemData.ID, g.FrameCount, true, g.LastItemData.Rect};
}

void focus_glow(ImDrawList* dl, ImVec2 a, ImVec2 b, float t) {
  if (t <= 0.0f) return;
  // Filled bars throughout, as frame_rect draws them: exact on the software
  // renderer, where a stroked rectangle is not.
  auto frame = [dl](ImVec2 p, ImVec2 q, ImU32 col, float w) {
    dl->AddRectFilled(p, ImVec2(q.x, p.y + w), col);
    dl->AddRectFilled(ImVec2(p.x, q.y - w), q, col);
    dl->AddRectFilled(ImVec2(p.x, p.y + w), ImVec2(p.x + w, q.y - w), col);
    dl->AddRectFilled(ImVec2(q.x - w, p.y + w), ImVec2(q.x, q.y - w), col);
  };
  const float solid = std::max(2.0f, std::round(px(2)));
  frame(a, b, u32(kAmber, t), solid);
  // The halo: three rings outside the frame, each fainter, each as wide as
  // the gap from the one inside it.
  const float r1 = std::round(px(2)), r2 = std::round(px(4)), r3 = std::round(px(7));
  const float rings[][2] = {{r1, 0.32f}, {r2, 0.14f}, {r3, 0.06f}};
  float inner = 0.0f;
  for (const auto& ring : rings) {
    const float out = std::max(ring[0], inner + 1.0f);
    frame(ImVec2(a.x - out, a.y - out), ImVec2(b.x + out, b.y + out), u32(kAmber, ring[1] * t), out - inner);
    inner = out;
  }
}

void draw_focus_glow() {
  ImGuiContext& g = *GImGui;
  ImGuiWindow* w = g.NavWindow;
  const bool noted = g_glow_note.frame == g.FrameCount && g_glow_note.id == g.NavId;
  if (!w || g.NavId == 0 || !g.NavCursorVisible || !g.NavIdIsAlive || !w->WasActive || (noted && g_glow_note.off) ||
      g.NavLayer != ImGuiNavLayer_Main) {
    g_glide.have = false;
    return;
  }
  ImRect r = noted ? g_glow_note.rect : ImGui::WindowRectRelToAbs(w, w->NavRectRel[g.NavLayer]);
  if (r.IsInverted() || !w->ClipRect.Overlaps(r)) {
    g_glide.have = false;
    return;
  }
  // Glide from where the glow was to the new item: the offset from the item
  // shrinks each frame, so an item that moves (a page scrolling under it)
  // carries the glow with it rather than leaving it behind.
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float far = 0.4f * std::max(vp->Size.x, vp->Size.y);
  if (g.NavId != g_glide.id || !g_glide.have) {
    const ImVec2 c0 = g_glide.drawn.GetCenter(), c1 = r.GetCenter();
    const ImVec2 d(c0.x - c1.x, c0.y - c1.y);
    const bool snap = !g_glide.have || reduce_motion() || w->RootWindow != g_glide.root || std::sqrt(d.x * d.x + d.y * d.y) > far;
    const ImRect& was = g_glide.drawn;
    g_glide.off_min = snap ? ImVec2(0, 0) : ImVec2(was.Min.x - r.Min.x, was.Min.y - r.Min.y);
    g_glide.off_max = snap ? ImVec2(0, 0) : ImVec2(was.Max.x - r.Max.x, was.Max.y - r.Max.y);
  } else {
    const float k = std::exp(-anim_dt() / 0.04f);
    auto decay = [k](ImVec2 v) {
      v = ImVec2(v.x * k, v.y * k);
      return ImVec2(std::fabs(v.x) < 0.5f ? 0.0f : v.x, std::fabs(v.y) < 0.5f ? 0.0f : v.y);
    };
    g_glide.off_min = decay(g_glide.off_min);
    g_glide.off_max = decay(g_glide.off_max);
  }
  const ImRect drawn(ImVec2(std::round(r.Min.x + g_glide.off_min.x), std::round(r.Min.y + g_glide.off_min.y)),
                     ImVec2(std::round(r.Max.x + g_glide.off_max.x), std::round(r.Max.y + g_glide.off_max.y)));
  g_glide = Glide{true, g.NavId, w->RootWindow, drawn, g_glide.off_min, g_glide.off_max};

  // Breathing, slowly: present, never flashing. Held still without motion.
  const float breathe = reduce_motion() ? 1.0f : 0.88f + 0.12f * std::cos(static_cast<float>(g.Time) * 2.0f * 3.14159265f / 1.6f);
  // On the item's own window, last, so it is over the item and under any
  // popup; clipped to what the window shows, with room for the halo, and
  // never past the window around it.
  ImRect clip = w->ClipRect;
  clip.Expand(std::round(px(8)));
  if ((w->Flags & ImGuiWindowFlags_ChildWindow) && w->ParentWindow) clip.ClipWithFull(w->ParentWindow->ClipRect);
  else clip.ClipWithFull(ImRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y)));
  ImDrawList* dl = w->DrawList;
  dl->PushClipRect(clip.Min, clip.Max, false);
  focus_glow(dl, drawn.Min, drawn.Max, breathe);
  dl->PopClipRect();
}

void wrap_rows() { ImGui::NavMoveRequestTryWrapping(ImGui::GetCurrentWindow(), ImGuiNavMoveFlags_WrapX); }

namespace {

// The tiles of the region tiles_begin() opened: each one's ID, window, focus
// scope and rectangle on the screen.
struct TileRef {
  ImGuiID id = 0;
  ImGuiWindow* window = nullptr;
  ImGuiID scope = 0;
  ImRect rect;
};
struct Tiles {
  ImGuiWindow* region = nullptr;
  std::vector<TileRef> items;
};
Tiles g_tiles;

}  // namespace

void tiles_begin() {
  g_tiles.region = ImGui::GetCurrentWindow();
  g_tiles.items.clear();
}

void tiles_item() {
  const ImGuiContext& g = *GImGui;
  const ImGuiLastItemData& it = g.LastItemData;
  if (!g_tiles.region || it.ID == 0 || (it.ItemFlags & ImGuiItemFlags_NoNav) || (it.ItemFlags & ImGuiItemFlags_Disabled)) return;
  g_tiles.items.push_back(TileRef{it.ID, g.CurrentWindow, g.CurrentFocusScopeId, it.NavRect});
}

void tiles_end() {
  ImGuiContext& g = *GImGui;
  ImGuiWindow* region = g_tiles.region;
  const std::vector<TileRef> items = std::move(g_tiles.items);
  g_tiles = Tiles{};
  if (!region || items.empty() || !back_allowed() || g.IO.KeyCtrl || g.IO.KeyAlt) return;
  const auto on = std::find_if(items.begin(), items.end(), [&](const TileRef& t) { return t.id == g.NavId; });
  if (on == items.end()) return;
  const bool home = ImGui::IsKeyPressed(ImGuiKey_Home), end = ImGui::IsKeyPressed(ImGuiKey_End);
  const bool up = ImGui::IsKeyPressed(ImGuiKey_PageUp), down = ImGui::IsKeyPressed(ImGuiKey_PageDown);
  if (home == end && up == down) return;

  const float view = region->InnerRect.GetHeight();
  const float top = std::max(0.0f, region->ScrollMax.y);
  const TileRef* to = nullptr;
  float scroll = region->Scroll.y;
  if (home || end) {
    to = home ? &items.front() : &items.back();
    scroll = home ? 0.0f : top;
  } else {
    // A page is most of the region's height, so the row the focus was on
    // stays in sight at the other edge, and the focus goes to the tile
    // nearest the spot on the screen it was at.
    const float step = (down ? 1.0f : -1.0f) * std::round(view * 0.8f);
    scroll = std::clamp(scroll + step, 0.0f, top);
    const ImVec2 want(on->rect.GetCenter().x, on->rect.GetCenter().y + step);
    float best = FLT_MAX;
    for (const TileRef& t : items) {
      const ImVec2 c = t.rect.GetCenter();
      const float d = (c.x - want.x) * (c.x - want.x) + (c.y - want.y) * (c.y - want.y);
      if (d < best) {
        best = d;
        to = &t;
      }
    }
    // The page scrolls at least as far as puts that tile in sight.
    const float t0 = to->rect.Min.y - region->InnerRect.Min.y + region->Scroll.y;
    const float t1 = to->rect.Max.y - region->InnerRect.Min.y + region->Scroll.y;
    if (t0 < scroll) scroll = t0;
    else if (t1 > scroll + view) scroll = t1 - view;
    scroll = std::clamp(scroll, 0.0f, top);
  }
  // ImGui's own Home, End and page moves would land somewhere else next
  // frame: on the nearest item at the top or foot, which may be a heading's
  // control, not a tile.
  if (g.NavMoveSubmitted && (g.NavMoveFlags & (ImGuiNavMoveFlags_IsPageMove | ImGuiNavMoveFlags_ScrollToEdgeY))) {
    ImGui::NavMoveRequestCancel();
  }
  ImGui::PushFocusScope(to->scope);
  ImGui::SetFocusID(to->id, to->window);
  ImGui::PopFocusScope();
  to->window->NavRectRel[ImGuiNavLayer_Main] = ImGui::WindowRectAbsToRel(to->window, to->rect);
  ImGui::SetNavCursorVisible(true);
  g.NavHighlightItemUnderNav = true;
  if (to->window != region) ImGui::ScrollToRectEx(to->window, to->rect, ImGuiScrollFlags_KeepVisibleEdgeX);
  ImGui::SetScrollY(region, scroll);
}

void own_tab_while_focused() {
  if (ImGui::IsItemFocused()) ImGui::SetKeyOwner(ImGuiKey_Tab, GImGui->LastItemData.ID);
}

bool back_allowed() {
  if (!ImGui::GetCurrentContext()) return true;
  if (typing()) return false;
  // A popup left open but no longer drawn answers nothing, so only one that
  // was on screen last frame counts.
  for (const ImGuiPopupData& p : GImGui->OpenPopupStack) {
    if (p.Window && (p.Window->Active || p.Window->WasActive)) return false;
  }
  return true;
}

bool modal_cancelled() {
  ImGuiContext& g = *GImGui;
  ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
  if (!modal || !g.CurrentWindow || g.CurrentWindow->RootWindow != modal) return false;
  if (typing()) return false;
  if (g.NavWindow && g.NavWindow->RootWindow != modal) return false;
  return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

void scroll_with_keys() {
  ImGuiContext& g = *GImGui;
  ImGuiWindow* w = g.CurrentWindow;
  if (!w || !(w->Flags & ImGuiWindowFlags_ChildWindow)) return;
  // One stop in the window around it; entered with Enter, ImGui scrolls it
  // itself, all but the right stick.
  const bool stop = g.NavId != 0 && g.NavId == w->ChildId;
  const bool inside = g.NavWindow == w;
  const bool hovered = !g.NavCursorVisible && ImGui::IsWindowHovered();
  if (!stop && !inside && !hovered) return;

  const float y = w->Scroll.y, top = w->ScrollMax.y;
  const float line = ImGui::GetTextLineHeightWithSpacing();
  const float page = std::max(line, w->InnerRect.GetHeight() - line * 2.0f);
  float to = y;
  bool paged = false;
  if (stop || hovered) {
    paged = true;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) to = y + page;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) to = y - page;
    else if (ImGui::IsKeyPressed(ImGuiKey_Home)) to = 0.0f;
    else if (ImGui::IsKeyPressed(ImGuiKey_End)) to = top;
    else paged = false;
  }
  if (stop && g.NavMoveSubmitted && !(g.NavMoveFlags & ImGuiNavMoveFlags_IsTabbing)) {
    // The window around it would move the focus on these: to the next item,
    // or for PageDown to the last one in view.
    bool mine = paged;
    if (!paged && g.NavMoveDir == ImGuiDir_Down && y < top) {
      to = y + line * 2.0f;
      mine = true;
    } else if (!paged && g.NavMoveDir == ImGuiDir_Up && y > 0.0f) {
      to = y - line * 2.0f;
      mine = true;
    }
    if (mine) ImGui::NavMoveRequestCancel();
  }

  // The sticks: faster the further they are pushed, and never so slow near
  // the middle that a resting thumb creeps the text along.
  auto axis = [](ImGuiKey down, ImGuiKey up) {
    return ImGui::GetKeyData(down)->AnalogValue - ImGui::GetKeyData(up)->AnalogValue;
  };
  float stick = axis(ImGuiKey_GamepadRStickDown, ImGuiKey_GamepadRStickUp);
  if (stop || hovered) stick += axis(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp);
  stick = std::clamp(stick, -1.0f, 1.0f);
  if (std::fabs(stick) > 0.12f) to += stick * std::fabs(stick) * px(1400) * std::min(g.IO.DeltaTime, 0.05f);

  if (to != y) ImGui::SetScrollY(w, std::clamp(to, 0.0f, top));
}

void scroll_with_keys_end() {
  ImGuiWindow* w = GImGui->CurrentWindow;
  if (!w || !(w->Flags & ImGuiWindowFlags_ChildWindow) || (w->ChildFlags & ImGuiChildFlags_NavFlattened)) return;
  // ImGui makes a child a stop only once it knows the child scrolls, which it
  // learns a frame late: on the frame a page comes up its text panel would
  // not be a stop yet, and could not be the page's default.
  if (w->DC.CursorMaxPos.y - w->DC.CursorStartPos.y > w->InnerRect.GetHeight() + 0.5f) w->DC.NavWindowHasScrollY = true;
}

bool Refresh::due(const std::string& key, double every) {
  const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
  const bool stale = every > 0.0 && now - read_at_ >= every;
  if (read_at_ >= 0.0 && key == key_ && !page_appearing() && !stale) return false;
  key_ = key;
  read_at_ = now;
  return true;
}

}  // namespace kg::gui
