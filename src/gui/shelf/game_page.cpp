// One game: where you left off, Play, its snapshots and its sessions, or,
// for a game not installed yet, what the shelf can see of its discs and the
// way into the wizard.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../../install/collection.h"
#include "../../session/journal.h"
#include "../../session/saves.h"
#include "../../util/format.h"
#include "../../util/paths.h"
#include "../blocks.h"
#include "../format.h"
#include "../layout.h"
#include "../palette.h"
#include "../session_log.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {
namespace fs = std::filesystem;

namespace {

// The bytes in the regular files under `dir`, and 0 when it is not there.
uint64_t tree_bytes(const fs::path& dir) {
  std::error_code ec;
  uint64_t bytes = 0;
  for (const auto& de : fs::recursive_directory_iterator(dir, ec)) {
    if (de.is_regular_file(ec)) bytes += de.file_size(ec);
  }
  return bytes;
}

// A fact about the pack in a line of them, "files 5 · size 12 KB": the key
// dim, the value in `colour`, a dot between it and the one before while they
// share a line, and a new line when it would not fit.
void fact(const char* key, const std::string& value, const ImVec4& colour, bool first) {
  const float space = ImGui::CalcTextSize(" ").x;
  const float w = ImGui::CalcTextSize(key).x + space + ImGui::CalcTextSize(value.c_str()).x;
  if (!first) {
    const char* dot = "\xc2\xb7";
    ImGui::SameLine(0, space);
    if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(dot).x + space * 2 + w) {
      ImGui::NewLine();
    } else {
      ImGui::TextDisabled("%s", dot);
      ImGui::SameLine(0, space);
    }
  }
  ImGui::TextDisabled("%s", key);
  ImGui::SameLine(0, space);
  ImGui::TextColored(colour, "%s", value.c_str());
}

}  // namespace

void GamePage::draw() {
  if (ctx_.entries.empty()) { ctx_.go(Screen::Shelf); return; }
  const Entry& e = ctx_.entries[std::min(ctx_.selected, ctx_.entries.size() - 1)];
  // The hero band says the game's name, so the header only says where it is.
  set_page_trail("kretro \xe2\x80\xba " + e.name);
  PageWindow page(e.name.c_str(), "Esc back", ctx_.fonts.big());

  // Read when the page comes up, when the collection is read again (after a
  // session, a build, an uninstall) and after a restore here, rather than on
  // every frame: a long journal is a file read per session per frame.
  if (disk_.due(e.id + "\n" + std::to_string(ctx_.generation))) {
    journal_ = e.installed ? session::journal(e.id) : std::vector<session::Record>{};
    gens_ = e.installed ? session::generations(e.id) : std::vector<std::string>{};
  }
  const std::vector<session::Record>& journal = journal_;

  // One region scrolls the whole page, the hero band with it, so nothing on
  // the page is in a box of its own that has to be scrolled apart from it.
  begin_page_scroll("page");

  // The game's picture behind its name: where you left off, or its title
  // screen, and blurred across the band behind them.
  const fs::path& shot = e.last_png.empty() ? e.title_png : e.last_png;
  HeroSpec hs;
  hs.art = ctx_.texture(shot);
  hs.backdrop = hs.art ? ctx_.textures.backdrop_file(shot) : nullptr;
  hs.id = e.id;
  hs.title = e.name;
  // With no picture the band names the id itself, so the kicker does not.
  const std::string year = e.year ? std::to_string(e.year) : std::string();
  if (!hs.art) hs.kicker = year;
  else hs.kicker = year.empty() ? e.id : year + " \xc2\xb7 " + e.id;
  if (hs.art) hs.kicker += " \xc2\xb7 where you left off";
  else if (e.installed) hs.kicker = "No screenshot yet. One is taken automatically while you play.";
  hero(hs);

  // What the last thing done here came to - a session just played, a
  // snapshot restored - under the bar whose button did it, where it is seen.
  auto status_line = [&] {
    if (ctx_.status.empty()) return;
    ImGui::Spacing();
    ImGui::TextColored(kAccentDim, ">");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", ctx_.status.c_str());
  };

  const float stat_gap = std::round(px(32));
  if (e.installed) {
    action_bar_begin("actions");
    if (play_button("Play", PlayKind::Play)) ctx_.play(e.id);
    default_focus();
    // What the shelf knows of the playing, beside the button that adds to it.
    auto figure = [&](const char* label, const std::string& value) {
      action_bar_next(stat_size(label, value), stat_gap);
      stat(label, value);
    };
    if (e.sessions) {
      figure("last played", ago(e.last_played));
      figure("play time", human_time(e.total_seconds));
      figure("sessions", std::to_string(e.sessions));
    } else {
      figure("played", "never");
    }
    const char* const more[] = {"Timeline", "Settings", "Uninstall"};
    const int pressed = action_bar_ghosts(more, 3);
    if (pressed == 0) ctx_.go(Screen::Timeline);
    if (pressed == 1) ctx_.go(Screen::Settings);
    if (pressed == 2) confirm_uninstall_ = true;
    uninstall_modal(e);
    action_bar_end();
    status_line();
    vgap(6);

    // The sessions at the left, and what can be done with what they left
    // behind at the right; one above the other on a narrow page, the things
    // to press first.
    auto activity = [&] {
      section("activity");
      if (journal.empty()) ImGui::TextDisabled("never played");
      else session_log("journal", journal, 12);
    };
    auto details = [&] {
      const std::vector<std::string>& gens = gens_;
      if (!gens.empty()) {
        section("snapshots");
        ImGui::TextDisabled("every session that wrote something left one");
        // A row is as tall as its restore button.
        const float row_h = small_button_height() + ImGui::GetStyle().ItemSpacing.y;
        const float rows = std::min(static_cast<float>(gens.size()), 5.0f);
        const ImVec2 pad(std::round(px(10)), std::round(px(6)));
        const float h = rows * row_h - ImGui::GetStyle().ItemSpacing.y + pad.y * 2;
        if (begin_panel("gens", ImVec2(0, h), pad, kFrameLine)) {
          const float restore_w = small_button_width("restore");
          for (auto it = gens.rbegin(); it != gens.rend(); ++it) {
            ImGui::PushID(it->c_str());
            align_to_small_button();
            ImGui::TextColored(kCyan, "%s", it->c_str());
            ImGui::SameLine();
            align_right(restore_w);
            if (small_button("restore")) {
              try {
                session::restore(e.id, *it);
                ctx_.status = "restored " + *it;
              } catch (const std::exception& ex) { ctx_.status = ex.what(); }
              // A restore keeps what was there as a snapshot of its own.
              disk_.invalidate();
            }
            ImGui::PopID();
          }
          edge_fades(kBg1);
        }
        end_panel();
      }

      section("pack");
      fact("files", std::to_string(e.files), kText, true);
      fact("size", human_size(e.tree_bytes), kText, false);
      fact("packed to", human_size(e.pack_bytes), kText, false);
      ImGui::TextDisabled("one file");
      ImGui::SameLine();
      elided_text(e.pack.filename().string(), kCyan);
      // The state Meta.discs has been able to describe since the format
      // existed, finally said out loud. Play still works - most of these games
      // only check for the disc when they start a new campaign or play their
      // video - so this is a fact about the pack, not a refusal, and it sits
      // with the other facts about the pack rather than beside the Play button.
      if (!e.absent_discs.empty()) {
        ImGui::Spacing();
        if (e.flat_body) {
          badge_line(BadgeKind::Warn,
                     "Needs the original disc. This pack was made before packs carried "
                     "their discs - rebuild it to include them.",
                     kWarm);
        } else if (e.absent_discs.size() == 1) {
          badge_line(BadgeKind::Warn,
                     "Needs the original disc: this pack names " + e.absent_discs[0] +
                         " but does not carry it.",
                     kWarm);
        } else {
          badge_line(BadgeKind::Warn,
                     "Needs the original discs: this pack names " +
                         std::to_string(e.absent_discs.size()) + " it does not carry.",
                     kWarm);
        }
        // The discs under the sentence, lined up with it rather than the badge.
        const float hang = ImGui::CalcTextSize("[WARN]").x + badge_gap();
        ImGui::Indent(hang);
        for (const std::string& d : e.absent_discs) ImGui::TextColored(kCyan, "%s", d.c_str());
        ImGui::Unindent(hang);
      }

      if (!e.last_note.empty()) {
        section("note");
        ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
        ImGui::TextWrapped("\"%s\"", e.last_note.c_str());
        ImGui::PopStyleColor();
      }
    };

    if (breakpoint() != Breakpoint::Compact) {
      // Columns in a table rather than child windows, so the page's one
      // region scrolls them both and the keys move between them freely.
      ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(std::round(px(14)), 0.0f));
      if (ImGui::BeginTable("details", 2, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupColumn("activity", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("more", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        activity();
        ImGui::TableNextColumn();
        details();
        ImGui::EndTable();
      }
      ImGui::PopStyleVar();
    } else {
      details();
      activity();
    }
  } else {
    action_bar_begin("actions");
    // The manifest is not a gate, it is a head start: everything it knows
    // about this game becomes the wizard's first answer, and every one of
    // those answers is still editable. So Install is offered even when the
    // shelf cannot see the discs, or the manifest does not load.
    if (play_button("Install", PlayKind::Install)) wizard_.open_for(e.id);
    default_focus();
    action_bar_end();
    status_line();
    vgap(6);

    // A game we have a manifest for and have not installed. The discs it
    // names may not be in the collection, and that is no reason for a dead
    // end: the manifest is not a gate, and the wizard never reads iso_dir()
    // as a requirement - you hand it the files. So what we cannot see is a
    // note under the same button.
    if (!e.missing_discs.empty()) {
      std::string what = e.missing_discs[0];
      for (size_t i = 1; i < e.missing_discs.size(); ++i) what += ", " + e.missing_discs[i];
      badge_line(BadgeKind::Warn,
                 "Not installed. Nothing in " + install::iso_dir().string() + " answers to " +
                     what + ".",
                 kText);
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, kDim);
      ImGui::TextWrapped(
          "That directory is where the shelf looks, and it is the only place the shelf "
          "looks. The wizard takes the files themselves - an .iso, a .bin and its .cue, a "
          "zip, a folder, a bare setup .exe - from anywhere on this machine, or dropped on "
          "this window. What the manifest knows is offered either way.");
      ImGui::PopStyleColor();
    } else if (e.blocked) {
      badge_line(BadgeKind::Fail,
                 "Not installed, and this game's manifest does not load: " + e.blocked_reason,
                 kText);
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, kDim);
      ImGui::TextWrapped(
          "The wizard opens with nothing prefilled, which is how it opens for a disc "
          "nobody has written a manifest for.");
      ImGui::PopStyleColor();
    } else {
      badge_line(BadgeKind::Info, "Not installed yet.", kText);
    }
  }

  end_page_scroll();
  // Past the last use of `e`, which points into the list this replaces.
  if (reload_after_draw_) {
    reload_after_draw_ = false;
    ctx_.reload();
  }
}

// A game can be reinstalled from its disc in minutes; a save cannot be
// reinstalled at all. So the saves are kept unless you say otherwise, and the
// dialog says what will be freed before it frees it.
void GamePage::uninstall_modal(const Entry& e) {
  const fs::path prefix = prefixes_dir() / e.id;
  const fs::path saves = saves_dir() / e.id;
  if (confirm_uninstall_) {
    ImGui::OpenPopup("Uninstall?");
    confirm_uninstall_ = false;
    also_saves_ = false;
    // What goes, measured once as the question comes up: a Wine prefix is
    // thousands of files, and walking it on every frame the question stays
    // up is a stall on every one of them. Nothing changes the two while it
    // is up.
    prefix_bytes_ = tree_bytes(prefix);
    save_bytes_ = tree_bytes(saves);
  }
  // Headed with the question as its title; the popup's name is only its ID.
  if (!dialog_begin("Uninstall?", 520, "Uninstall " + e.name + "?")) return;

  std::error_code ec;
  const uint64_t prefix_bytes = prefix_bytes_, save_bytes = save_bytes_;

  // What goes, with the sizes in a column of their own so they line up.
  if (ImGui::BeginTable("frees", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
    const std::pair<const char*, uint64_t> rows[] = {{"the game", e.pack_bytes},
                                                     {"its Wine prefix", prefix_bytes},
                                                     {"saves and snapshots", save_bytes}};
    float size_w = 0;
    for (const auto& row : rows) size_w = std::max(size_w, ImGui::CalcTextSize(human_size(row.second).c_str()).x);
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, size_w);
    for (const auto& [what, bytes] : rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("  %s", what);
      ImGui::TableNextColumn();
      const std::string size = human_size(bytes);
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                           ImGui::CalcTextSize(size.c_str()).x);
      ImGui::TextUnformatted(size.c_str());
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  ImGui::Checkbox("delete the saves too", &also_saves_);
  dialog_footer();
  // The button that deletes things says so in the colour errors are said in.
  if (dialog_button("Uninstall", DialogButton::Danger)) {
    fs::remove(e.pack, ec);
    fs::remove_all(prefix, ec);
    if (also_saves_) fs::remove_all(saves, ec);
    ctx_.status = e.name + " uninstalled";
    reload_after_draw_ = true;
    ctx_.go(Screen::Shelf);
    ImGui::CloseCurrentPopup();
    dialog_end();
    return;
  }
  // Keeping it is what Escape and the pad's B answer, and where the focus
  // starts, so a key pressed without reading deletes nothing.
  if (dialog_button("Keep it", DialogButton::Secondary, true)) ImGui::CloseCurrentPopup();
  dialog_end();
}

}  // namespace kg::gui::shelf
