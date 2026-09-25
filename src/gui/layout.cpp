#include "layout.h"

#include <algorithm>
#include <cmath>

#include "imgui_internal.h"
#include "scale.h"

namespace kg::gui {

namespace {

constexpr float kCompactBelow = 1100.0f, kWideAbove = 2200.0f;

// What centre_column changed, to put back.
struct Column {
  bool open = false;
  float indent = 0;
  float work_max = 0, content_max = 0;
};
Column g_column;

}  // namespace

Breakpoint breakpoint_for(float design_w) {
  if (design_w < kCompactBelow) return Breakpoint::Compact;
  if (design_w > kWideAbove) return Breakpoint::Wide;
  return Breakpoint::Regular;
}

Breakpoint breakpoint() {
  if (!ImGui::GetCurrentContext()) return Breakpoint::Regular;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float w = vp->WorkSize.x > 0 ? vp->WorkSize.x : ImGui::GetIO().DisplaySize.x;
  if (w <= 0) return Breakpoint::Regular;
  return breakpoint_for(w / std::max(ui_scale(), 0.01f));
}

float content_max_w_for(Content c, Breakpoint bp, float avail, float scale) {
  avail = std::max(avail, 0.0f);
  // The widest each kind reads well at, in design pixels, on each size
  // class; 0 is the whole room.
  auto by = [bp](float compact, float regular, float wide) {
    if (bp == Breakpoint::Compact) return compact;
    return bp == Breakpoint::Wide ? wide : regular;
  };
  float design = 0;
  switch (c) {
    case Content::Reading: design = by(0, 1100, 1200); break;
    case Content::Form: design = by(0, 1040, 1100); break;
    case Content::Grid: design = by(0, 0, 2000); break;
    case Content::Dialog:
      // A dialog keeps a margin round it on a small window, and never
      // stretches past what a sentence reads comfortably at.
      if (bp == Breakpoint::Compact) return std::max(0.0f, std::min(avail - 32.0f * scale, 560.0f * scale));
      design = 720.0f;
      break;
  }
  return design > 0 ? std::min(avail, std::round(design * scale)) : avail;
}

float content_max_w(Content c) {
  const float avail = ImGui::GetCurrentContext() && ImGui::GetCurrentWindowRead() ? ImGui::GetContentRegionAvail().x : 0.0f;
  return content_max_w_for(c, breakpoint(), avail, ui_scale());
}

float hero_height_for(Breakpoint bp, float window_h, float scale) {
  switch (bp) {
    case Breakpoint::Compact: {
      // A small window is often a short one too (800x480): there the band
      // keeps to under a third of the height, so what is under it - Play and
      // the page's own content - still has most of the room. Never so short
      // that the kicker and a title in the hero type do not fit.
      const float most = window_h > 0 ? std::round(window_h * 0.3f) : std::round(220.0f * scale);
      return std::max(std::round(150.0f * scale), std::min(std::round(220.0f * scale), most));
    }
    case Breakpoint::Wide: return std::round(340.0f * scale);
    default: return std::round(300.0f * scale);
  }
}

float hero_height() {
  const float h = ImGui::GetCurrentContext() ? ImGui::GetMainViewport()->WorkSize.y : 0.0f;
  return hero_height_for(breakpoint(), h, ui_scale());
}

float sidebar_width() {
  switch (breakpoint()) {
    case Breakpoint::Compact: return std::round(px(64));
    case Breakpoint::Wide: return std::round(px(260));
    default: return std::round(px(236));
  }
}

void centre_column(float max_w) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  // Columns do not nest; one still open is left over from a page that threw.
  g_column = Column{};
  const float avail = ImGui::GetContentRegionAvail().x;
  const float width = std::max(1.0f, std::min(max_w, avail));
  g_column.open = true;
  g_column.indent = std::floor((avail - width) * 0.5f);
  g_column.work_max = w->WorkRect.Max.x;
  g_column.content_max = w->ContentRegionRect.Max.x;
  if (g_column.indent > 0) ImGui::Indent(g_column.indent);
  // The right edge moves in too, so that text wraps, fields stretch and
  // align_right lands inside the column rather than at the window's edge.
  const float right = w->DC.CursorPos.x + width;
  w->WorkRect.Max.x = std::min(w->WorkRect.Max.x, right);
  w->ContentRegionRect.Max.x = std::min(w->ContentRegionRect.Max.x, right);
}

void end_centre_column() {
  if (!g_column.open) return;
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  w->WorkRect.Max.x = g_column.work_max;
  w->ContentRegionRect.Max.x = g_column.content_max;
  if (g_column.indent > 0) ImGui::Unindent(g_column.indent);
  g_column = Column{};
}

}  // namespace kg::gui
