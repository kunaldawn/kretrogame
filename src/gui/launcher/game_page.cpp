// One game: its cover behind its name, Play, the way to its saves, display
// and controls, its sessions, and why it last would not play. And the question asked before a
// game is unpacked on a machine with no FUSE.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

#include "../../session/journal.h"
#include "../../util/format.h"
#include "../blocks.h"
#include "../format.h"
#include "../palette.h"
#include "../session_log.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {

void GamePage::draw() {
  const bundle::GameMeta& g = ctx_.p.game(ctx_.selected);
  const bool solo = ctx_.ids.size() == 1;
  // The hero band says the game's name, so the header only says where it is:
  // in a one-game player, the bundle is the game.
  const std::string& bundle = ctx_.p.bundle().meta.title;
  set_page_trail(solo ? bundle : bundle + " \xe2\x80\xba " + g.name);
  PageWindow page(g.name.c_str(), solo ? "Esc quit" : "Esc back", ctx_.w.fonts().big());
  // Read when the page comes up and after a session, not on every frame.
  if (disk_.due(g.id + "\n" + std::to_string(ctx_.generation))) journal_ = session::journal(g.id);
  const std::vector<session::Record>& j = journal_;

  // One region scrolls the whole page, the hero band with it.
  begin_page_scroll("page");

  HeroSpec hs;
  hs.art = ctx_.cover(g.id);
  hs.backdrop = hs.art ? ctx_.cover_backdrop(g.id) : nullptr;
  hs.id = g.id;
  hs.title = g.name;
  // With no picture the band names the id itself, so the kicker does not.
  const std::string year = g.year ? std::to_string(g.year) : std::string();
  if (!hs.art) hs.kicker = year;
  else hs.kicker = year.empty() ? g.id : year + " \xc2\xb7 " + g.id;
  hero(hs);

  action_bar_begin("actions");
  if (play_button("Play", PlayKind::Play)) ctx_.play(g.id);
  default_focus();
  // How much it has been played, beside the button that plays it.
  const float stat_gap = std::round(px(32));
  auto figure = [&](const char* label, const std::string& value) {
    action_bar_next(stat_size(label, value), stat_gap);
    stat(label, value);
  };
  if (!j.empty()) {
    double total = 0;
    for (const session::Record& r : j) total += static_cast<double>(r.ended - r.started);
    figure("last played", ago(j.front().ended));
    figure("in total", human_time(total));
    figure("sessions", std::to_string(j.size()));
  } else {
    figure("played", "never");
  }
  // A one-game player has no grid to go back to for the bundle's own
  // settings, so they are here too.
  const char* const more[] = {"Saves", "Display", "Controls", "Settings"};
  const int pressed = action_bar_ghosts(more, solo ? 4 : 3);
  if (pressed == 0) ctx_.go(Screen::Saves);
  if (pressed == 1) ctx_.go(Screen::Display);
  if (pressed == 2) ctx_.go(Screen::Controls);
  if (pressed == 3) ctx_.go(Screen::Bundle);
  action_bar_end();
  if (!ctx_.status.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", ctx_.status.c_str());
  }

  const ImGuiStyle& st = ImGui::GetStyle();
  if (!ctx_.failure.empty() && ctx_.failure_game == g.id) {
    vgap(6);
    section("last try");
    badge_line(BadgeKind::Fail, ctx_.failure, kWarn);
    if (!ctx_.failure_log.empty()) {
      vgap(2);
      // As tall as its lines, wrapped to the panel so none runs into its
      // border, and no taller than a screenful: past that it scrolls.
      const ImVec2 pad = px(12, 10);
      const float wrap = ImGui::GetContentRegionAvail().x - pad.x * 2 - st.ScrollbarSize;
      const float text_h = ImGui::CalcTextSize(ctx_.failure_log.c_str(), nullptr, false, wrap).y;
      const float max_h = std::max(ImGui::GetTextLineHeight() * 4.0f, px(260));
      const float log_h = std::round(std::min(text_h, max_h) + pad.y * 2);
      begin_text_panel("log", ImVec2(0, log_h), pad, kLine);
      ImGui::PushStyleColor(ImGuiCol_Text, kDim);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(ctx_.failure_log.c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();
      end_panel();
    }
    if (ImGui::Button("Save diagnostics report")) ctx_.save_report();
  }

  vgap(6);
  section("history");
  if (j.empty()) ImGui::TextDisabled("never played");
  else session_log("journal", j, 8);

  end_page_scroll();
}

void consent_modal(LauncherContext& ctx) {
  ImGui::OpenPopup("unpack");
  const player::UnpackPlan& u = ctx.consent_plan;
  const std::string name = ctx.p.game(ctx.consent_game).name;
  if (dialog_begin("unpack", 720, "Unpack " + name + "?")) {
    ImGui::TextWrapped("This machine cannot mount games (it has no FUSE), so %s has to be unpacked "
                       "to play.", name.c_str());
    vgap(4);
    // The numbers and the place as a table, the place in the colour paths
    // are shown in, so that none of them is lost in the middle of a sentence.
    if (kv_begin("unpack")) {
      kv("needs", fmt::gigabytes(u.need));
      kv("free", fmt::gigabytes(u.free), u.fits() ? kText : kWarn);
      kv("where", u.where.parent_path().string(), kCyan);
      kv_end();
    }
    ImGui::TextDisabled("It is done once; later launches use the copy.");
    bool close = false;
    if (!u.fits()) {
      vgap(8);
      // The command on a line of its own, in the colour of a thing to type.
      begin_badge_block(BadgeKind::Warn);
      colored_text(kWarn,
                   "There is not enough room. Free some space there, or unpack it somewhere else from a "
                   "terminal:");
      colored_text(kCyan, ctx.p.bundle().self.filename().string() + " --extract-to DIR " + ctx.consent_game);
      end_badge_block();
    }
    dialog_footer();
    if (!u.fits()) {
      ImGui::BeginDisabled();
      dialog_button("Extract", DialogButton::Primary);
      ImGui::EndDisabled();
    } else if (dialog_button("Extract", DialogButton::Primary)) {
      const std::string id = ctx.consent_game;
      ctx.run_job("Unpacking " + name, [&ctx, id] { ctx.p.unpack(id); },
                  [&ctx, id](const std::string& err) {
                    if (!err.empty()) ctx.fail(id, err, false);
                    else ctx.play_now(id);
                  });
      close = true;
    }
    // Escape and the pad's B answer as this button does: as they went back
    // before the modal took them, which from a one-game player was quitting.
    // It takes the focus only when Extract cannot be pressed; otherwise the
    // focus starts on Extract, the one asked for.
    const char* later = ctx.ids.size() > 1 ? "Not now" : "Quit";
    bool declined = dialog_button(later, DialogButton::Secondary, false);
    if (!u.fits()) ImGui::SetItemDefaultFocus();
    declined = declined || modal_cancelled();
    if (declined) {
      if (ctx.ids.size() == 1) ctx.quit = true;
      close = true;
    }
    if (close) {
      ctx.consent = false;
      ImGui::CloseCurrentPopup();
    }
    dialog_end();
  }
}

}  // namespace kg::gui::launcher
