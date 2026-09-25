#include "session_log.h"

#include <algorithm>
#include <cmath>

#include "../util/format.h"
#include "format.h"
#include "imgui.h"
#include "widgets.h"

namespace kg::gui {

void session_log(const char* id, const std::vector<session::Record>& journal, float max_lines) {
  const ImGuiStyle& st = ImGui::GetStyle();
  const float row_h = ImGui::GetTextLineHeightWithSpacing();
  float lines = 0;
  for (const session::Record& r : journal) lines += r.note.empty() ? 1.0f : 2.0f;
  const float shown = std::max(1.0f, std::min(lines, max_lines));
  // The rows are spaced by ItemSpacing, so the last has none under it and the
  // panel ends on its last line rather than on a gap.
  const ImVec2 pad(std::round(px(10)), std::round(px(6)));
  const float h = shown * row_h - st.ItemSpacing.y + pad.y * 2;
  if (begin_text_panel(id, ImVec2(0, h), pad, kFrameLine)) {
    const char* bullet = "\xe2\x97\x8f";
    const float indent = ImGui::CalcTextSize("\xe2\x97\x8f ").x;
    for (const session::Record& r : journal) {
      ImGui::TextColored(kAccentDim, "%s", bullet);
      ImGui::SameLine(0, 0);
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent - ImGui::CalcTextSize(bullet).x);
      ImGui::TextDisabled("%s", fmt::local_minute(r.started).c_str());
      ImGui::SameLine();
      ImGui::TextUnformatted(human_time(static_cast<double>(r.ended - r.started)).c_str());
      if (!r.note.empty()) {
        ImGui::Indent(indent);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("\"%s\"", r.note.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent(indent);
      }
    }
    edge_fades(kBg1);
  }
  end_panel();
}

}  // namespace kg::gui
