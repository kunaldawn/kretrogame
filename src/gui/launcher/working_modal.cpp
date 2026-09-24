// The launcher's own "working" modal: smaller than the shelf's job modal, with
// no log, only the job's title and how far it has got.
#include <string>

#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {

void working_modal(LauncherContext& ctx) {
  ImGui::OpenPopup("working");
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(640, 0));
  if (ImGui::BeginPopupModal("working", nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
    ImGui::PushFont(ctx.w.big);
    ImGui::TextWrapped("%s", ctx.job.title().c_str());
    ImGui::PopFont();
    std::string p = ctx.progress_text();
    ImGui::TextDisabled("%s", p.empty() ? "working..." : p.c_str());
    ImGui::EndPopup();
  }
}

}  // namespace kg::gui::launcher
