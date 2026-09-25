#include <cmath>
#include <exception>
#include <string>

#include "../../util/env.h"
#include "../../util/paths.h"
#include "../widgets.h"
#include "bundles_page.h"
#include "choices.h"

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
  section("preview");
  ImGui::TextWrapped(
      "Runs the built file as somebody who downloaded it would: an empty home directory, none of kretro's "
      "state and none of its environment. You see the first-run check, the launcher and a fresh prefix; "
      "all of it is thrown away when it ends.");
  ImGui::Spacing();
  if (draft_.last_built.empty()) {
    ImGui::TextDisabled("Build it first.");
    return;
  }
  // Labelled, because it is the file the last build wrote, which need not be
  // the version the other steps now name.
  ImGui::TextDisabled("runs");
  ImGui::SameLine();
  elided_text(draft_.last_built, kCyan);
  ImGui::Spacing();
  const ImVec2 size(0, std::round(ImGui::GetFrameHeight() * 1.4f));
  if (preview_.running()) {
    if (ImGui::Button("Stop", ImVec2(std::round(ImGui::CalcTextSize("Stop").x + px(48)), size.y))) preview_.stop();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    spinner();
  } else {
    if (primary_button("Run it", size)) start_preview();
    if (preview_.started()) {
      ImGui::SameLine(0, std::round(px(14)));
      ImGui::AlignTextToFramePadding();
      badge(preview_.status() == 0 ? BadgeKind::Ok : BadgeKind::Fail);
      ImGui::SameLine();
      ImGui::TextDisabled("ended with status %d", preview_.status());
    }
  }
  ImGui::Spacing();
  // The log as a terminal shows one: a panel of its own, the newest line in
  // full colour and the ones before it dim.
  // Until something has run there is only the prompt to show: a panel a few
  // lines tall, not an empty box down the rest of the pane.
  const float log_h =
      preview_log_.empty()
          ? std::round(ImGui::GetTextLineHeightWithSpacing() * 3 + ImGui::GetStyle().WindowPadding.y * 2)
          : 0.0f;
  begin_text_panel("preview-log", ImVec2(0, log_h), ImVec2(-1, -1), kFrameLine);
  for (size_t i = 0; i < preview_log_.size(); ++i) {
    colored_text(i + 1 == preview_log_.size() ? kText : kDim, preview_log_[i]);
  }
  if (preview_log_.empty()) ImGui::TextDisabled("$ _");
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeight() * 2) ImGui::SetScrollHereY(1.0f);
  end_panel();
}

}  // namespace kg::gui
