// One game: where you left off, Play, its snapshots and its sessions, or,
// for a game not installed yet, what the shelf can see of its discs and the
// way into the wizard.
#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "../../install/collection.h"
#include "../../session/journal.h"
#include "../../session/saves.h"
#include "../../util/format.h"
#include "../../util/paths.h"
#include "../format.h"
#include "../palette.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {
namespace fs = std::filesystem;

void GamePage::draw() {
  if (ctx_.entries.empty()) { ctx_.go(Screen::Shelf); return; }
  const Entry& e = ctx_.entries[std::min(ctx_.selected, ctx_.entries.size() - 1)];
  PageWindow page(e.name.c_str(), "Esc back", ctx_.fonts.big);

  ImGui::BeginChild("left", ImVec2(ImGui::GetContentRegionAvail().x * 0.58f, 0), false);
  const Texture* hero = ctx_.texture(e.last_png.empty() ? e.title_png : e.last_png);
  if (hero) {
    float w = ImGui::GetContentRegionAvail().x;
    float h = w * static_cast<float>(hero->h) / static_cast<float>(hero->w);
    ImGui::Image(reinterpret_cast<ImTextureID>(hero->tex), ImVec2(w, h));
    ImGui::TextDisabled("where you left off");
  } else if (e.installed) {
    ImGui::TextDisabled("No screenshot yet. One is taken automatically while you play.");
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("right", ImVec2(0, 0), false);

  if (e.installed) {
    ImGui::PushFont(ctx_.fonts.big);
    if (ImGui::Button("Play", ImVec2(-1, 60))) ctx_.play(e.id);
    ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::Button("Timeline")) ctx_.go(Screen::Timeline);
    ImGui::SameLine();
    if (ImGui::Button("Settings")) ctx_.go(Screen::Settings);
    ImGui::SameLine();
    if (ImGui::Button("Uninstall")) confirm_uninstall_ = true;
    ImGui::Spacing();
    uninstall_modal(e);

    if (e.sessions) {
      ImGui::Text("%zu session%s, %s in total", e.sessions, e.sessions == 1 ? "" : "s",
                  human_time(e.total_seconds).c_str());
      ImGui::TextDisabled("last played %s", ago(e.last_played).c_str());
      if (!e.last_note.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
        ImGui::TextWrapped("\"%s\"", e.last_note.c_str());
        ImGui::PopStyleColor();
      }
    } else {
      ImGui::TextDisabled("never played");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%zu files, %s, packed to %s", e.files, human_size(e.tree_bytes).c_str(),
                        human_size(e.pack_bytes).c_str());
    ImGui::TextDisabled("one file: %s", e.pack.filename().c_str());

    // The state Meta.discs has been able to describe since the format
    // existed, finally said out loud. Play still works - most of these games
    // only check for the disc when they start a new campaign or play their
    // video - so this is a fact about the pack, not a refusal, and it sits
    // with the other facts about the pack rather than beside the Play button.
    if (!e.absent_discs.empty()) {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
      if (e.flat_body) {
        ImGui::TextWrapped("Needs the original disc. This pack was made before packs carried "
                           "their discs - rebuild it to include them.");
      } else if (e.absent_discs.size() == 1) {
        ImGui::TextWrapped("Needs the original disc: this pack names %s but does not carry it.",
                           e.absent_discs[0].c_str());
      } else {
        ImGui::TextWrapped("Needs the original discs: this pack names %zu it does not carry.",
                           e.absent_discs.size());
      }
      ImGui::PopStyleColor();
      for (const std::string& d : e.absent_discs) ImGui::TextDisabled("  %s", d.c_str());
    }

    ImGui::Spacing();
    std::vector<std::string> gens = session::generations(e.id);
    if (!gens.empty()) {
      ImGui::Separator();
      ImGui::Text("snapshots");
      ImGui::TextDisabled("every session that wrote something left one");
      ImGui::BeginChild("gens", ImVec2(0, 140), true);
      for (auto it = gens.rbegin(); it != gens.rend(); ++it) {
        ImGui::PushID(it->c_str());
        ImGui::TextUnformatted(it->c_str());
        ImGui::SameLine(120);
        if (ImGui::SmallButton("restore")) {
          try { session::restore(e.id, *it); ctx_.status = "restored " + *it; }
          catch (const std::exception& ex) { ctx_.status = ex.what(); }
        }
        ImGui::PopID();
      }
      ImGui::EndChild();
    }

    ImGui::Spacing();
    std::vector<session::Record> j = session::journal(e.id);
    if (!j.empty()) {
      ImGui::Separator();
      ImGui::Text("timeline");
      ImGui::BeginChild("journal", ImVec2(0, 160), true);
      for (const session::Record& r : j) {
        ImGui::Text("%s  %s", fmt::local_minute(r.started).c_str(),
                    human_time(static_cast<double>(r.ended - r.started)).c_str());
        if (!r.note.empty()) ImGui::TextDisabled("  \"%s\"", r.note.c_str());
      }
      ImGui::EndChild();
    }
  } else {
    // A game we have a manifest for and have not installed. The discs it
    // names may not be in the collection, and that is no reason for a dead
    // end: the manifest is not a gate, and the wizard never reads iso_dir()
    // as a requirement - you hand it the files. So what we cannot see is a
    // note above the same button.
    if (!e.missing_discs.empty()) {
      std::string what = e.missing_discs[0];
      for (size_t i = 1; i < e.missing_discs.size(); ++i) what += ", " + e.missing_discs[i];
      ImGui::TextWrapped("Not installed. Nothing in %s answers to %s.",
                         install::iso_dir().c_str(), what.c_str());
      ImGui::Spacing();
      ImGui::TextDisabled(
          "That directory is where the shelf looks, and it is the only place the shelf "
          "looks. The wizard takes the files themselves - an .iso, a .bin and its .cue, a "
          "zip, a folder, a bare setup .exe - from anywhere on this machine, or dropped on "
          "this window. What the manifest knows is offered either way.");
    } else if (e.blocked) {
      ImGui::TextWrapped("Not installed, and this game's manifest does not load: %s",
                         e.blocked_reason.c_str());
      ImGui::Spacing();
      ImGui::TextDisabled(
          "The wizard opens with nothing prefilled, which is how it opens for a disc "
          "nobody has written a manifest for.");
    } else {
      ImGui::TextWrapped("Not installed yet.");
    }
    ImGui::Spacing();
    ImGui::PushFont(ctx_.fonts.big);
    // The manifest is not a gate, it is a head start: everything
    // it knows about this game becomes the wizard's first answer, and every
    // one of those answers is still editable.
    if (ImGui::Button("Install", ImVec2(-1, 60))) wizard_.open_for(e.id);
    ImGui::PopFont();
  }

  if (!ctx_.status.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", ctx_.status.c_str());
  }
  ImGui::EndChild();
}

// A game can be reinstalled from its disc in minutes; a save cannot be
// reinstalled at all. So the saves are kept unless you say otherwise, and the
// dialog says what will be freed before it frees it.
void GamePage::uninstall_modal(const Entry& e) {
  if (confirm_uninstall_) {
    ImGui::OpenPopup("Uninstall?");
    confirm_uninstall_ = false;
    also_saves_ = false;
  }
  if (!ImGui::BeginPopupModal("Uninstall?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  std::error_code ec;
  uint64_t prefix_bytes = 0;
  fs::path prefix = prefixes_dir() / e.id;
  for (const auto& de : fs::recursive_directory_iterator(prefix, ec)) {
    if (de.is_regular_file(ec)) prefix_bytes += de.file_size(ec);
  }
  uint64_t save_bytes = 0;
  fs::path saves = saves_dir() / e.id;
  for (const auto& de : fs::recursive_directory_iterator(saves, ec)) {
    if (de.is_regular_file(ec)) save_bytes += de.file_size(ec);
  }

  ImGui::Text("Uninstall %s?", e.name.c_str());
  ImGui::Spacing();
  ImGui::TextDisabled("  the game            %s", human_size(e.pack_bytes).c_str());
  ImGui::TextDisabled("  its Wine prefix     %s", human_size(prefix_bytes).c_str());
  ImGui::TextDisabled("  saves and snapshots %s", human_size(save_bytes).c_str());
  ImGui::Checkbox("delete the saves too", &also_saves_);
  ImGui::Spacing();
  if (ImGui::Button("Uninstall")) {
    fs::remove(e.pack, ec);
    fs::remove_all(prefix, ec);
    if (also_saves_) fs::remove_all(saves, ec);
    ctx_.status = e.name + " uninstalled";
    ctx_.reload();
    ctx_.go(Screen::Shelf);
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button("Keep it")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

}  // namespace kg::gui::shelf
