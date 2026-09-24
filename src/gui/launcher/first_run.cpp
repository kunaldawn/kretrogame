// The first run: the machine checked without a word, what stops every game
// said here, a warning shown once, and the applications-menu entry offered
// once. A one-game player plays as soon as none of that is in the way.
#include <string>

#include "../../util/paths.h"
#include "../palette.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {

void LauncherContext::begin_check() {
  go(Screen::Checking);
  run_job("Checking this machine", [this] { report = p.doctor_report(); },
          [this](const std::string& err) {
            if (!err.empty()) {
              blocking = {"The check of this machine failed: " + err};
              go(Screen::Blocked);
              return;
            }
            checked = true;
            for (const gpu::Problem& pr : report.problems) {
              if (pr.blocking()) blocking.push_back(pr.line());
            }
            for (const std::string& warning : player::unseen_warnings(report, state)) notes.push_back(warning);
            if (!p.state().refused_portable.empty() && !state.portable_fallback_said) {
              notes.push_back(p.state().refused_portable.string() + " is there, but it " +
                              p.state().refused_why + ", so this player keeps its saves in " +
                              state_dir().string() + " instead.");
              state.portable_fallback_said = true;
            }
            if (!blocking.empty()) {
              go(Screen::Blocked);
              return;
            }
            offer_desktop = !state.desktop_offered;
            save_state();
            if (ids.size() == 1) {
              go(Screen::Game);
              // Started once the notes are read and the menu entry is
              // answered - and at once when there is neither, which is every
              // launch after the first: waiting for a modal that will never
              // open left a one-game player on its page instead of playing.
              auto_play = true;
              maybe_auto_play();
            } else {
              go(Screen::Grid);
            }
          });
}

void LauncherContext::maybe_auto_play() {
  if (auto_play && notes.empty() && !offer_desktop) {
    auto_play = false;
    play(ids.front());
  }
}

void CheckingPage::draw() {
  PageWindow page(ctx_.p.bundle().meta.title.c_str(), "", ctx_.w.big);
  ImGui::TextDisabled("checking this machine...");
}

void BlockedPage::draw() {
  PageWindow page(ctx_.p.bundle().meta.title.c_str(), "Esc quit", ctx_.w.big);
  ImGui::PushFont(ctx_.w.big);
  ImGui::TextWrapped("This machine cannot play these games yet.");
  ImGui::PopFont();
  ImGui::Spacing();
  for (const std::string& b : ctx_.blocking) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", b.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
  ImGui::Spacing();
  if (ImGui::Button("Show the full report")) ctx_.go(Screen::About);
  ImGui::SameLine();
  if (ImGui::Button("Quit")) ctx_.quit = true;
}

void notes_modal(LauncherContext& ctx) {
  ImGui::OpenPopup("worth knowing");
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(720, 0));
  if (ImGui::BeginPopupModal("worth knowing", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::TextWrapped("Worth knowing about this machine. You will not be told again.");
    ImGui::Spacing();
    for (const std::string& n : ctx.notes) {
      ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
      ImGui::TextWrapped("%s", n.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(-1, 44))) {
      ctx.notes.clear();
      ImGui::CloseCurrentPopup();
      ctx.maybe_auto_play();
    }
    ImGui::EndPopup();
  }
}

void desktop_modal(LauncherContext& ctx) {
  ImGui::OpenPopup("applications menu");
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(640, 0));
  if (ImGui::BeginPopupModal("applications menu", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::TextWrapped("Add %s to your applications menu?", ctx.p.bundle().meta.title.c_str());
    ImGui::TextDisabled("It points at this file, where it is now. Bundle settings removes it.");
    ImGui::Spacing();
    bool answered = false;
    if (ImGui::Button("Add it")) {
      ctx.add_desktop_entry();
      answered = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("No thanks")) answered = true;
    if (answered) {
      ctx.state.desktop_offered = true;
      ctx.offer_desktop = false;
      ctx.save_state();
      ImGui::CloseCurrentPopup();
      ctx.maybe_auto_play();
    }
    ImGui::EndPopup();
  }
}

}  // namespace kg::gui::launcher
