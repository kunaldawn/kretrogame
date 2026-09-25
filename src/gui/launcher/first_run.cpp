// The first run: the machine checked without a word, what stops every game
// said here, a warning shown once, and the applications-menu entry offered
// once. A one-game player plays as soon as none of that is in the way.
#include <algorithm>
#include <cmath>
#include <string>

#include "../../util/paths.h"
#include "../draw.h"
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

namespace {

// The bundle's banner, blurred and darkened across the whole window behind a
// screen that is only a message, as a console shows a game's art behind its
// loading card; the window's own colour when there is no banner.
void backdrop(LauncherContext& ctx) {
  const bundle::BundleMeta& m = ctx.p.bundle().meta;
  const Texture* back = m.banner.empty() ? nullptr : ctx.textures.backdrop_png("banner", m.banner);
  if (!back || back->w <= 0 || back->h <= 0) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
  const ImVec2 b(a.x + size.x, a.y + size.y);
  ImVec2 uv0, uv1;
  cover_uv(static_cast<float>(back->w), static_cast<float>(back->h), b.x - a.x, b.y - a.y, &uv0, &uv1);
  dl->PushClipRect(a, b, false);
  draw_image(dl, *back, a, b, uv0, uv1);
  dl->AddRectFilled(a, b, u32(kBg0, 0.72f));
  dl->PopClipRect();
}

}  // namespace

void CheckingPage::draw() {
  // No header: the card in the middle says what is going on, over the
  // bundle's own picture.
  set_page_bare();
  PageWindow page(ctx_.p.bundle().meta.title.c_str(), "", ctx_.w.fonts().big());
  backdrop(ctx_);
  // The working modal says the check is running while it is, and a second
  // card behind it would only repeat it; the page's own card is for the
  // moment between the job and the screen it leads to.
  if (ctx_.job.running()) return;
  centred_card_begin("checking", 560);
  ImGui::PushFont(ctx_.w.fonts().big());
  ImGui::TextColored(kAccent, "Checking this machine");
  ImGui::PopFont();
  title_rule();
  vgap(6);
  spinner();
  ImGui::SameLine(0, std::round(px(10)));
  block_progress(-1.0f);
  centred_card_end();
}

void BlockedPage::draw() {
  // No header either: the card says it all, over the bundle's picture, and
  // the status bar still names the bundle.
  set_page_bare();
  PageWindow page(ctx_.p.bundle().meta.title.c_str(), "Esc quit", ctx_.w.fonts().big());
  backdrop(ctx_);
  // A card in the middle of the page, as a question is: what stops every
  // game, and the two ways on from here.
  centred_card_begin("blocked", 720);
  ImGui::PushFont(ctx_.w.fonts().big());
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::TextWrapped("This machine cannot play these games yet.");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  title_rule();
  vgap(8);
  // Each reason with its badge in a column of its own, the text wrapping
  // beside it.
  for (const std::string& b : ctx_.blocking) {
    badge_line(BadgeKind::Fail, b, kWarn);
    vgap(4);
  }
  vgap(8);
  const bool report = primary_button("Show the full report");
  default_focus();
  if (report) ctx_.go(Screen::About);
  ImGui::SameLine();
  if (ImGui::Button("Quit")) ctx_.quit = true;
  centred_card_end();
}

void notes_modal(LauncherContext& ctx) {
  ImGui::OpenPopup("worth knowing");
  if (dialog_begin("worth knowing", 720, "Worth knowing")) {
    ImGui::TextWrapped("About this machine. You will not be told again.");
    vgap(8);
    for (const std::string& n : ctx.notes) {
      badge_line(BadgeKind::Warn, n, kWarm);
      vgap(2);
    }
    dialog_footer();
    // The one way on, which Escape and the pad's B also take.
    if (dialog_button("OK", DialogButton::Primary, true)) {
      ctx.notes.clear();
      ImGui::CloseCurrentPopup();
      ctx.maybe_auto_play();
    }
    dialog_end();
  }
}

void desktop_modal(LauncherContext& ctx) {
  ImGui::OpenPopup("applications menu");
  if (dialog_begin("applications menu", 700, "Applications menu")) {
    ImGui::TextWrapped("Add %s to your applications menu?", ctx.p.bundle().meta.title.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("It points at this file, where it is now. Bundle settings removes it.");
    ImGui::PopStyleColor();
    dialog_footer();
    bool answered = false;
    if (dialog_button("Add it", DialogButton::Primary)) {
      ctx.add_desktop_entry();
      answered = true;
    }
    // Declining is what Escape and the pad's B answer, and where the focus
    // starts.
    if (dialog_button("No thanks", DialogButton::Secondary, true)) answered = true;
    if (answered) {
      ctx.state.desktop_offered = true;
      ctx.offer_desktop = false;
      ctx.save_state();
      ImGui::CloseCurrentPopup();
      ctx.maybe_auto_play();
    }
    dialog_end();
  }
}

}  // namespace kg::gui::launcher
