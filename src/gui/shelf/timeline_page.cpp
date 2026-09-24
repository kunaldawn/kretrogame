// One game's timeline: every snapshot, newest first, with what the session
// that left it did, and going back to one.
#include <exception>
#include <string>
#include <vector>

#include "../../session/journal.h"
#include "../../session/saves.h"
#include "../format.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {

// The overlay already captured every write and the harvester already
// photographed every session. Putting them on one axis is a rewind button for
// games that never had one.
void TimelinePage::draw() {
  if (ctx_.entries.empty()) { ctx_.go(Screen::Shelf); return; }
  const Entry& en = ctx_.entries[ctx_.selected < ctx_.entries.size() ? ctx_.selected : 0];
  PageWindow page((en.name + " - timeline").c_str(), "Esc back", ctx_.fonts.big);

  std::vector<std::string> gens = session::generations(en.id);
  std::vector<session::Record> recs = session::journal(en.id);
  if (gens.empty()) {
    ImGui::TextWrapped(
        "Nothing to go back to yet. Every time you play, whatever the game writes is kept "
        "as a snapshot, and they appear here newest first - when it was, how long you "
        "played, what it wrote - with the last picture taken of the game beside the "
        "newest of them.");
    return;
  }

  for (size_t i = gens.size(); i-- > 0;) {
    const std::string& g = gens[i];
    ImGui::PushID(g.c_str());
    const session::Record* rec = nullptr;
    for (const session::Record& r : recs) if (r.generation == g) rec = &r;

    if (const Texture* t = ctx_.texture(en.last_png); t && i + 1 == gens.size()) {
      ImGui::Image(reinterpret_cast<ImTextureID>(t->tex), ImVec2(160, 120));
      ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::Text("snapshot %s", g.c_str());
    if (rec) {
      ImGui::TextDisabled("%s, played %s, %zu files written", ago(rec->ended).c_str(),
                          human_time(static_cast<double>(rec->ended - rec->started)).c_str(),
                          rec->files_written);
      if (!rec->note.empty()) ImGui::TextDisabled("\"%s\"", rec->note.c_str());
    }
    if (ImGui::SmallButton("Restore")) {
      restore_gen_ = g;
      confirm_restore_ = true;
    }
    ImGui::EndGroup();
    ImGui::Separator();
    ImGui::PopID();
  }

  if (confirm_restore_) {
    ImGui::OpenPopup("Restore?");
    confirm_restore_ = false;
  }
  if (ImGui::BeginPopupModal("Restore?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Restore snapshot %s?", restore_gen_.c_str());
    ImGui::TextWrapped(
        "Everything the game has written since then is replaced by what it had "
        "written by then. The current state is kept as a new snapshot first, so "
        "this is reversible.");
    ImGui::Spacing();
    if (ImGui::Button("Restore")) {
      try {
        session::restore(en.id, restore_gen_);
        ctx_.status = "restored " + restore_gen_;
      } catch (const std::exception& ex) {
        ctx_.status = std::string("could not restore: ") + ex.what();
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

}  // namespace kg::gui::shelf
