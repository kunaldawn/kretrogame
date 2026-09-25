#include "dialog.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "anim.h"
#include "draw.h"
#include "focus.h"
#include "imgui_internal.h"
#include "palette.h"
#include "scale.h"
#include "widgets.h"

namespace kg::gui {

namespace {

// What a dialog measured of itself on the frame before, which this frame lays
// out by: the height of what its band held, and how wide each line of
// buttons came out, to put the line against the right edge.
struct Memo {
  int frame = -1;
  float footer_h = 0;
  std::vector<float> rows;
};
std::unordered_map<ImGuiID, Memo> g_memo;

// The dialog being drawn.
struct Current {
  bool open = false;
  ImGuiID id = 0;
  bool footer = false;
  float footer_top = 0;
  int row = -1;
  float row_y = 0, next_x = 0, after_row = 0;
  std::vector<float> rows;
};
Current g_cur;

// How the dialog comes up: faded in over 120 ms from the frame it opens.
float fade_of(Memo& m) {
  const int now = ImGui::GetFrameCount();
  static std::unordered_map<ImGuiID, float> shown;
  float& t = shown[g_cur.id];
  if (m.frame < now - 1) t = 0.0f;
  if (reduce_motion()) t = 1.0f;
  else t = std::min(1.0f, t + anim_dt() / 0.12f);
  return ease_out_cubic(t);
}

// A soft shadow round the dialog, four frames each fainter, so it stands off
// the dimmed page rather than lying flat on it.
void shadow(ImGuiWindow* w, float a) {
  ImDrawList* dl = w->DrawList;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  dl->PushClipRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), false);
  const float step = std::max(1.0f, std::round(px(6)));
  const float alphas[] = {0.25f, 0.14f, 0.07f, 0.03f};
  float in = 0.0f;
  for (float al : alphas) {
    const float out = in + step;
    frame_rect(dl, ImVec2(w->Pos.x - out, w->Pos.y - out), ImVec2(w->Pos.x + w->Size.x + out, w->Pos.y + w->Size.y + out),
               u32(kBg0, al * a), step);
    in = out;
  }
  dl->PopClipRect();
}

bool begin_common(const char* popup_id, const std::string& title) {
  // Dialogs do not nest; one still open here is left over from a page that
  // threw inside it, whose window ImGui's error recovery has already ended.
  g_cur = Current{};
  g_cur.id = ImGui::GetID(popup_id);
  Memo& m = g_memo[g_cur.id];
  const float a = fade_of(m);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * a);
  if (!ImGui::BeginPopupModal(popup_id, nullptr,
                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::PopStyleVar();
    return false;
  }
  g_cur.open = true;
  m.frame = ImGui::GetFrameCount();
  shadow(ImGui::GetCurrentWindow(), a);
  if (!title.empty()) modal_title(title);
  return true;
}

// The room a band's line of buttons is laid in: from the left of the
// dialog's content to its right.
float content_right() { return ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x; }

float button_h() { return std::max(std::round(px(40)), ImGui::GetFontSize() + std::round(px(8)) * 2); }

}  // namespace

bool dialog_begin(const char* popup_id, float design_w, const std::string& title) {
  next_modal(design_w);
  return begin_common(popup_id, title);
}

bool dialog_begin_px(const char* popup_id, float w, float max_h, const std::string& title) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, max_h));
  return begin_common(popup_id, title);
}

void dialog_footer() {
  if (!g_cur.open || g_cur.footer) return;
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  const Memo& m = g_memo[g_cur.id];
  const float pad = w->WindowPadding.y;
  vgap(8);
  g_cur.footer = true;
  g_cur.footer_top = std::round(ImGui::GetCursorScreenPos().y);
  // The band, as tall as what it held last frame and down to the dialog's
  // foot, behind what is drawn in it now; a hairline along its top.
  const float border = w->WindowBorderSize;
  const ImVec2 a(w->Pos.x + border, g_cur.footer_top);
  const ImVec2 b(w->Pos.x + w->Size.x - border, g_cur.footer_top + pad * 2 + m.footer_h);
  w->DrawList->PushClipRect(w->Rect().Min, w->Rect().Max, false);
  w->DrawList->AddRectFilled(a, ImVec2(b.x, std::min(b.y, w->Pos.y + w->Size.y - border)), u32(kPanelRaised));
  w->DrawList->AddRectFilled(a, ImVec2(b.x, a.y + hairline()), u32(kLine));
  w->DrawList->PopClipRect();
  ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, g_cur.footer_top + pad));
  // Every button in the band as tall as a console's, and text on its line
  // lined up with them (AlignTextToFramePadding).
  const float fp = std::max(0.0f, std::round((button_h() - ImGui::GetFontSize()) * 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, fp));
}

bool dialog_button(const char* label, DialogButton kind, bool safe) {
  if (!g_cur.open) return ImGui::Button(label);
  const Memo& m = g_memo[g_cur.id];
  const ImGuiStyle& st = ImGui::GetStyle();
  const float h = g_cur.footer ? ImGui::GetFrameHeight() : button_h();
  const float bracket = kind == DialogButton::Primary ? ImGui::CalcTextSize("[ ").x * 2 : 0.0f;
  const float w = std::max(std::round(px(120)), ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2 + bracket);
  const float gap = std::round(px(10));
  const ImVec2 cur = ImGui::GetCursorScreenPos();
  if (g_cur.row < 0 || cur.y != g_cur.after_row) {
    // A new line of buttons, against the right edge by last frame's width
    // of the line.
    ++g_cur.row;
    g_cur.rows.push_back(0.0f);
    g_cur.row_y = cur.y;
    const float known = g_cur.row < static_cast<int>(m.rows.size()) ? m.rows[g_cur.row] : 0.0f;
    g_cur.next_x = std::max(cur.x, std::round(content_right() - known));
  }
  ImGui::SetCursorScreenPos(ImVec2(g_cur.next_x, g_cur.row_y));
  bool pressed = false;
  switch (kind) {
    case DialogButton::Primary: pressed = primary_button(label, ImVec2(w, h)); break;
    case DialogButton::Danger:
      ImGui::PushStyleColor(ImGuiCol_Text, kError);
      pressed = ImGui::Button(label, ImVec2(w, h));
      ImGui::PopStyleColor();
      break;
    case DialogButton::Secondary: pressed = ImGui::Button(label, ImVec2(w, h)); break;
  }
  if (safe) {
    ImGui::SetItemDefaultFocus();
    pressed = pressed || modal_cancelled();
  }
  float& row_w = g_cur.rows.back();
  row_w += (row_w > 0 ? gap : 0.0f) + w;
  g_cur.next_x += w + gap;
  g_cur.after_row = ImGui::GetCursorScreenPos().y;
  return pressed;
}

void dialog_end() {
  if (!g_cur.open) return;
  Memo& m = g_memo[g_cur.id];
  if (g_cur.footer) {
    ImGui::PopStyleVar();
    const float pad = ImGui::GetCurrentWindow()->WindowPadding.y;
    const ImGuiWindow* w = ImGui::GetCurrentWindow();
    const float content_bottom = std::max(w->DC.CursorMaxPos.y, w->DC.CursorPos.y - ImGui::GetStyle().ItemSpacing.y);
    m.footer_h = std::max(0.0f, content_bottom - (g_cur.footer_top + pad));
  }
  m.rows = g_cur.rows;
  g_cur = Current{};
  ImGui::EndPopup();
  ImGui::PopStyleVar();
}

}  // namespace kg::gui
