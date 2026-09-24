#include <exception>
#include <string>

#include "../../util/env.h"
#include "../../util/paths.h"
#include "../widgets.h"
#include "bundles_page.h"

namespace kg::gui {
namespace fs = std::filesystem;
using namespace kg::bundle;

// ---- 8. preview -------------------------------------------------------------------------------

void Bundles::start_preview() {
  preview_log_.clear();
  try {
    fs::path scratch = preview_scratch(cache_dir(), draft_.id);
    preview_.start(draft_.last_built, {}, preview_env(current_env(), env_or_empty("KRETRO_RUNTIME"), scratch), scratch);
    preview_log_.push_back("started " + draft_.last_built + " with an empty HOME at " + (scratch / "home").string());
  } catch (const std::exception& ex) {
    preview_log_.push_back(ex.what());
  }
}

void Bundles::preview_step() {
  ImGui::TextWrapped(
      "Runs the built file as somebody who downloaded it would: an empty home directory, none of kretro's "
      "state and none of its environment. You see the first-run check, the launcher and a fresh prefix; "
      "all of it is thrown away when it ends.");
  ImGui::Spacing();
  if (draft_.last_built.empty()) {
    ImGui::TextDisabled("Build it first.");
    return;
  }
  ImGui::Text("%s", draft_.last_built.c_str());
  if (preview_.running()) {
    if (ImGui::Button("Stop", ImVec2(200, 44))) preview_.stop();
  } else {
    if (ImGui::Button("Run it", ImVec2(200, 44))) start_preview();
    if (preview_.started()) {
      ImGui::SameLine();
      ImGui::TextDisabled("ended with status %d", preview_.status());
    }
  }
  ImGui::BeginChild("preview-log", ImVec2(0, 0), true);
  for (const std::string& l : preview_log_) ImGui::TextUnformatted(l.c_str());
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}

}  // namespace kg::gui
