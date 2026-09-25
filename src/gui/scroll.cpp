#include "scroll.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "anim.h"
#include "focus.h"
#include "imgui_internal.h"
#include "scale.h"

namespace kg::gui {

namespace {

// How long a glide takes: 63% of the way in 45 ms, 95% in about 135 ms.
constexpr float kTau = 0.045f;

// One axis of a region: where it is going, where it is drawn, and the scroll
// it was last set to, to notice something else moving it (the scrollbar).
struct Axis {
  float target = 0, shown = 0, set = -1;
};

struct Region {
  // The region's child window by its ID, looked up each frame rather than
  // kept as a pointer: a window closed and opened again (the player's
  // launcher after kretro, a tool's second window) is a new ImGui context,
  // and a pointer kept from the old one would point at freed memory.
  ImGuiID win = 0;
  Axis axis[2];
  int frame = -1;
};
std::unordered_map<ImGuiID, Region> g_regions;

// What end_scroll needs of the begin_scroll it closes.
struct Open {
  ScrollOpts opts;
  int frame = 0;
};
std::vector<Open> g_open;

// Whether `w` is `inside` or a window within it.
bool within(const ImGuiWindow* w, const ImGuiWindow* inside) {
  for (; w; w = w->ParentWindow) {
    if (w == inside) return true;
    if (!(w->Flags & ImGuiWindowFlags_ChildWindow)) return false;
  }
  return false;
}

// The target ImGui set for a window's axis, turned into a scroll, as its own
// Begin would turn it.
float target_of(const ImGuiWindow* w, int a) {
  const float deco = a == 0 ? w->DecoOuterSizeX1 + w->DecoInnerSizeX1 + w->DecoOuterSizeX2
                            : w->DecoOuterSizeY1 + w->DecoInnerSizeY1 + w->DecoOuterSizeY2;
  const float view = w->SizeFull[a] - deco;
  // Not held to the scroll's range yet: that is known only once the region
  // has begun this frame (a page just come up, or rescaled, still has last
  // frame's), and begin_scroll holds it there then.
  return scroll_from_target(w->ScrollTarget[a], w->ScrollTargetCenterRatio[a], w->ScrollTargetEdgeSnapDist[a], view,
                            w->ScrollMax[a] + view, FLT_MAX);
}

}  // namespace

float wheel_step(float font_px, float inner) {
  return std::trunc(std::min(5.0f * font_px, std::max(0.0f, inner) * 0.67f));
}

float scroll_glide(float shown, float target, float dt) {
  if (std::fabs(target - shown) < 0.5f) return target;
  return approach(shown, target, dt, kTau, 0.5f);
}

float scroll_from_target(float target, float centre_ratio, float snap, float view, float content, float max_scroll) {
  if (snap > 0.0f) {
    if (target <= snap) target = ImLerp(0.0f, target, centre_ratio);
    else if (target >= content - snap) target = ImLerp(target, content, centre_ratio);
  }
  return std::clamp(target - centre_ratio * view, 0.0f, std::max(0.0f, max_scroll));
}

bool begin_scroll(const char* id, ImVec2 size, const ScrollOpts& opts, ImGuiWindowFlags flags) {
  ImGuiContext& g = *GImGui;
  const ImGuiID key = ImGui::GetID(id);
  Region& r = g_regions[key];
  // A region not drawn last frame starts where ImGui has it, as does every
  // region while its page is coming up: a page opens already scrolled to its
  // focus, not gliding there.
  ImGuiWindow* live = r.win ? ImGui::FindWindowByID(r.win) : nullptr;
  const bool fresh = live == nullptr || r.frame < g.FrameCount - 1 || r.frame > g.FrameCount;
  const bool snap = fresh || reduce_motion() || page_age() < 3;
  const float dt = anim_dt();
  ImVec2 next(-1.0f, -1.0f);
  const bool steered = live && live->WasActive;
  if (steered) {
    ImGuiWindow* w = live;
    const bool wheeled = g.WheelingWindow == w && g.WheelingWindowScrolledFrame == g.FrameCount;
    // The right stick, while the focus is in this region or, with no focus
    // shown, the pointer is over it.
    float stick = 0.0f;
    if (opts.stick) {
      const bool mine = (g.NavWindow && within(g.NavWindow, w)) ||
                        (!g.NavCursorVisible && g.HoveredWindow && within(g.HoveredWindow, w));
      if (mine) {
        stick = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue - ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
        stick = std::clamp(stick, -1.0f, 1.0f);
        if (std::fabs(stick) < 0.12f) stick = 0.0f;
      }
    }
    for (int a = 0; a < 2; ++a) {
      if (a == 0 ? !opts.x : !opts.y) continue;
      Axis& ax = r.axis[a];
      if (fresh) ax = Axis{w->Scroll[a], w->Scroll[a], w->Scroll[a]};
      // Moved by something other than this: the scrollbar being dragged,
      // which is followed exactly.
      if (w->ScrollTarget[a] == FLT_MAX && ax.set >= 0 && std::fabs(w->Scroll[a] - ax.set) > 0.5f) {
        ax.target = ax.shown = w->Scroll[a];
      }
      if (w->ScrollTarget[a] < FLT_MAX) {
        // A wheel notch is a step from where the region is going, so notches
        // in quick succession add up; anything else says where to be.
        if (wheeled && w->ScrollTargetCenterRatio[a] == 0.0f && w->ScrollTargetEdgeSnapDist[a] == 0.0f) {
          ax.target += w->ScrollTarget[a] - w->Scroll[a];
        } else {
          ax.target = target_of(w, a);
        }
        w->ScrollTarget[a] = FLT_MAX;
      }
      if (a == 1 && stick != 0.0f) ax.target += stick * std::fabs(stick) * px(1800) * dt;
      ax.target = std::max(0.0f, ax.target);
      ax.shown = snap ? ax.target : scroll_glide(ax.shown, ax.target, dt);
      ax.set = std::round(ax.shown);
      next[a] = ax.set;
    }
    if (next.x >= 0 || next.y >= 0) ImGui::SetNextWindowScroll(next);
  }
  if (opts.no_scrollbar) flags |= ImGuiWindowFlags_NoScrollbar;
  if (opts.x && !opts.no_scrollbar) flags |= ImGuiWindowFlags_HorizontalScrollbar;
  const ImGuiChildFlags child = ImGuiChildFlags_NavFlattened | (opts.padded ? ImGuiChildFlags_AlwaysUseWindowPadding : 0);
  const bool open = ImGui::BeginChild(id, size, child, flags);
  // Now the range is this frame's: where the region is going is held to it,
  // and what ImGui made of the scroll it was given is what it was set to.
  // A region that was not up last frame starts from wherever ImGui put it.
  for (int a = 0; a < 2; ++a) {
    Axis& ax = r.axis[a];
    const float top = std::max(0.0f, g.CurrentWindow->ScrollMax[a]);
    if (!steered) ax.target = ax.shown = g.CurrentWindow->Scroll[a];
    ax.target = std::min(ax.target, top);
    ax.shown = std::min(ax.shown, top);
    ax.set = g.CurrentWindow->Scroll[a];
  }
  r.win = g.CurrentWindow->ID;
  r.frame = g.FrameCount;
  // Left over from a frame a page threw in, and ended by ImGui's recovery.
  while (!g_open.empty() && g_open.back().frame != g.FrameCount) g_open.pop_back();
  g_open.push_back(Open{opts, g.FrameCount});
  return open;
}

void end_scroll() {
  if (g_open.empty()) {
    ImGui::EndChild();
    return;
  }
  const Open o = g_open.back();
  g_open.pop_back();
  edge_fades(o.opts.bg, o.opts.edge_fade, o.opts.x, o.opts.y);
  ImGui::EndChild();
}

void edge_fades(const ImVec4& bg, float design_px, bool x, bool y) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (design_px <= 0.0f || w->SkipItems) return;
  const ImRect in = w->InnerRect;
  if (in.GetWidth() <= 0 || in.GetHeight() <= 0) return;
  ImDrawList* dl = w->DrawList;
  const ImU32 clear = u32(bg, 0.0f);
  dl->PushClipRect(in.Min, in.Max, false);
  if (y) {
    const float h = std::min(std::round(px(design_px)), std::floor(in.GetHeight() * 0.5f));
    // A little scrolled is a little faded, so the fade grows in rather than
    // appearing whole the moment the region moves.
    const float top = std::clamp(w->Scroll.y / std::max(h, 1.0f), 0.0f, 1.0f);
    const float bottom = std::clamp((w->ScrollMax.y - w->Scroll.y) / std::max(h, 1.0f), 0.0f, 1.0f);
    if (top > 0) dl->AddRectFilledMultiColor(in.Min, ImVec2(in.Max.x, in.Min.y + h), u32(bg, top), u32(bg, top), clear, clear);
    if (bottom > 0) {
      dl->AddRectFilledMultiColor(ImVec2(in.Min.x, in.Max.y - h), in.Max, clear, clear, u32(bg, bottom), u32(bg, bottom));
    }
  }
  if (x) {
    const float wd = std::min(std::round(px(design_px)), std::floor(in.GetWidth() * 0.5f));
    const float left = std::clamp(w->Scroll.x / std::max(wd, 1.0f), 0.0f, 1.0f);
    const float right = std::clamp((w->ScrollMax.x - w->Scroll.x) / std::max(wd, 1.0f), 0.0f, 1.0f);
    if (left > 0) dl->AddRectFilledMultiColor(in.Min, ImVec2(in.Min.x + wd, in.Max.y), u32(bg, left), clear, clear, u32(bg, left));
    if (right > 0) dl->AddRectFilledMultiColor(ImVec2(in.Max.x - wd, in.Min.y), in.Max, clear, u32(bg, right), u32(bg, right), clear);
  }
  dl->PopClipRect();
}

void scroll_to_item(float centre_ratio) {
  ImGuiContext& g = *GImGui;
  ImGuiWindow* w = g.CurrentWindow;
  if (centre_ratio < 0.0f) {
    ImGui::ScrollToRectEx(w, g.LastItemData.Rect, ImGuiScrollFlags_KeepVisibleEdgeY);
  } else {
    const ImRect& r = g.LastItemData.Rect;
    ImGui::SetScrollFromPosY(w, r.Min.y + r.GetHeight() * centre_ratio - w->Pos.y, centre_ratio);
  }
}

}  // namespace kg::gui
