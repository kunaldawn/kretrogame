#include "job_modal.h"

#include <string>
#include <vector>

namespace kg::gui {

JobModalAction draw_job_modal(Job& job, ImFont* big, const JobModalHooks& hooks) {
  JobModalAction action = JobModalAction::None;
  ImGui::OpenPopup("busy");
  ImVec2 c = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(720, 380));
  if (ImGui::BeginPopupModal("busy", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::PushFont(big);
    ImGui::TextUnformatted(job.title().c_str());
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 260), true);
    for (const std::string& l : job.lines()) ImGui::TextWrapped("%s", l.c_str());
    // Follow the newest line, unless the reader has scrolled up to look back.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    if (job.finished()) {
      if (hooks.on_finished_frame) hooks.on_finished_frame();
      if (ImGui::Button("Close", ImVec2(-1, 44))) {
        if (hooks.on_close) hooks.on_close();
        ImGui::CloseCurrentPopup();
        action = JobModalAction::Close;
      }
    } else if (hooks.footer_while_running) {
      hooks.footer_while_running();
    }
    ImGui::EndPopup();
  }
  return action;
}

}  // namespace kg::gui
