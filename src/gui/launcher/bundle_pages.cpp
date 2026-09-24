// The bundle's own settings: the applications-menu entry, the data folder,
// the licences, and About, which is the doctor with a Save button.
#include <SDL.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "../../util/paths.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {
namespace fs = std::filesystem;

void BundlePage::draw() {
  const bundle::BundleMeta& m = ctx_.p.bundle().meta;
  PageWindow page((m.title + " - settings").c_str(), "Esc back", ctx_.w.big);
  std::error_code ec;
  const bool installed = fs::exists(ctx_.desktop_paths().entry, ec);
  if (installed) {
    if (ImGui::Button("Remove from the applications menu")) {
      player::remove_desktop_entry(ctx_.desktop_paths());
      ctx_.state.desktop_installed = false;
      ctx_.save_state();
      ctx_.status = "removed from your applications menu";
    }
  } else if (ImGui::Button("Add to the applications menu")) {
    ctx_.add_desktop_entry();
  }
  ImGui::Spacing();
  if (ImGui::Button("Open the data folder")) {
    std::string url = "file://" + state_dir().string();
    if (SDL_OpenURL(url.c_str()) != 0) ctx_.status = "could not open it: " + std::string(SDL_GetError());
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", state_dir().c_str());
  ImGui::Spacing();
  if (ImGui::Button("Licences")) ctx_.go(Screen::Licenses);
  ImGui::SameLine();
  if (ImGui::Button("About and diagnostics")) ctx_.go(Screen::About);
  ImGui::Spacing();
  ImGui::TextDisabled("%s %s%s", m.id.c_str(), m.version.c_str(),
                      m.kretro_version.empty() ? "" : (", built by kretro " + m.kretro_version).c_str());
  if (!ctx_.status.empty()) ImGui::TextWrapped("%s", ctx_.status.c_str());
}

void LicensesPage::draw() {
  PageWindow page("Licences", "Esc back", ctx_.w.big);
  if (licenses_text_.empty()) licenses_text_ = ctx_.p.licenses_text();
  ImGui::BeginChild("text", ImVec2(0, 0), false);
  ImGui::TextWrapped("%s", licenses_text_.c_str());
  ImGui::EndChild();
}

void AboutPage::draw() {
  PageWindow page("About and diagnostics", "Esc back", ctx_.w.big);
  if (!ctx_.checked && !ctx_.job.running()) {
    ImGui::TextDisabled("not checked yet");
    return;
  }
  if (report_text_.empty()) report_text_ = player::doctor::render(ctx_.report);
  if (ImGui::Button("Save report")) ctx_.save_report();
  ImGui::SameLine();
  ImGui::TextDisabled("the saved copy has no home directory and no user name in it");
  if (!ctx_.status.empty()) ImGui::TextWrapped("%s", ctx_.status.c_str());
  ImGui::BeginChild("report", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::TextUnformatted(report_text_.c_str());
  ImGui::EndChild();
}

}  // namespace kg::gui::launcher
