// One game: its cover, Play, the way to its saves, display and controls, its
// sessions, and why it last would not play. And the question asked before a
// game is unpacked on a machine with no FUSE.
#include <string>
#include <vector>

#include "../../session/journal.h"
#include "../../util/format.h"
#include "../format.h"
#include "../palette.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {

void GamePage::draw() {
  const bundle::GameMeta& g = ctx_.p.game(ctx_.selected);
  PageWindow page(g.name.c_str(), ctx_.ids.size() > 1 ? "Esc back" : "Esc quit", ctx_.w.big);
  ImGui::BeginChild("left", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), false);
  if (const Texture* t = ctx_.cover(g.id)) {
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::Image(reinterpret_cast<ImTextureID>(t->tex), ImVec2(w, w * static_cast<float>(t->h) / static_cast<float>(t->w)));
  }
  if (g.year) ImGui::TextDisabled("%u", g.year);
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("right", ImVec2(0, 0), false);
  ImGui::PushFont(ctx_.w.big);
  if (ImGui::Button("Play", ImVec2(-1, 60))) ctx_.play(g.id);
  ImGui::PopFont();
  ImGui::Spacing();
  if (ImGui::Button("Saves")) ctx_.go(Screen::Saves);
  ImGui::SameLine();
  if (ImGui::Button("Display")) ctx_.go(Screen::Display);
  ImGui::SameLine();
  if (ImGui::Button("Controls")) ctx_.go(Screen::Controls);
  if (ctx_.ids.size() == 1) {
    ImGui::SameLine();
    if (ImGui::Button("Settings")) ctx_.go(Screen::Bundle);
  }
  ImGui::Spacing();

  std::vector<session::Record> j = session::journal(g.id);
  if (!j.empty()) {
    double total = 0;
    for (const session::Record& r : j) total += static_cast<double>(r.ended - r.started);
    ImGui::Text("%zu session%s, %s in total", j.size(), j.size() == 1 ? "" : "s", human_time(total).c_str());
    ImGui::TextDisabled("last played %s", ago(j.front().ended).c_str());
  } else {
    ImGui::TextDisabled("never played");
  }

  if (!ctx_.failure.empty() && ctx_.failure_game == g.id) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", ctx_.failure.c_str());
    ImGui::PopStyleColor();
    if (!ctx_.failure_log.empty()) {
      ImGui::BeginChild("log", ImVec2(0, 260), true, ImGuiWindowFlags_HorizontalScrollbar);
      ImGui::TextUnformatted(ctx_.failure_log.c_str());
      ImGui::EndChild();
    }
    if (ImGui::Button("Save diagnostics report")) ctx_.save_report();
  }
  if (!ctx_.status.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", ctx_.status.c_str());
  }
  ImGui::EndChild();
}

void consent_modal(LauncherContext& ctx) {
  ImGui::OpenPopup("unpack");
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(720, 0));
  if (ImGui::BeginPopupModal("unpack", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    const player::UnpackPlan& u = ctx.consent_plan;
    const std::string name = ctx.p.game(ctx.consent_game).name;
    ImGui::TextWrapped("This machine cannot mount games (it has no FUSE), so %s has to be unpacked "
                       "to play.", name.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("Needs %s in %s to play without FUSE.", fmt::gigabytes(u.need).c_str(),
                       u.where.parent_path().c_str());
    ImGui::TextDisabled("%s free there. It is done once; later launches use the copy.", fmt::gigabytes(u.free).c_str());
    ImGui::Spacing();
    bool close = false;
    if (!u.fits()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      ImGui::TextWrapped("There is not enough room. Free some space there, or unpack it somewhere "
                         "else from a terminal: this file --extract-to DIR %s", ctx.consent_game.c_str());
      ImGui::PopStyleColor();
      ImGui::BeginDisabled();
      ImGui::Button("Extract");
      ImGui::EndDisabled();
    } else if (ImGui::Button("Extract")) {
      const std::string id = ctx.consent_game;
      ctx.run_job("Unpacking " + name, [&ctx, id] { ctx.p.unpack(id); },
                  [&ctx, id](const std::string& err) {
                    if (!err.empty()) ctx.fail(id, err, false);
                    else ctx.play_now(id);
                  });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(ctx.ids.size() > 1 ? "Not now" : "Quit")) {
      if (ctx.ids.size() == 1) ctx.quit = true;
      close = true;
    }
    if (close) {
      ctx.consent = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace kg::gui::launcher
