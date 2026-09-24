// One game's saves: exporting and importing them, and the snapshots every
// session leaves, newest first, to go back to.
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "../../session/journal.h"
#include "../../session/saves.h"
#include "../../util/paths.h"
#include "../format.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {
namespace fs = std::filesystem;

void SavesPage::draw() {
  const bundle::GameMeta& g = ctx_.p.game(ctx_.selected);
  PageWindow page((g.name + " - saves").c_str(), "Esc back", ctx_.w.big);
  std::vector<std::string> gens = session::generations(g.id);
  std::vector<session::Record> recs = session::journal(g.id);

  ImGui::Text("export and import");
  if (export_path_.empty()) {
    export_path_ = (state_dir() / "exports" / (g.id + ".saves.kgpack")).string();
  }
  ImGui::SetNextItemWidth(-160);
  input_string("##export", export_path_, 1024);
  ImGui::SameLine();
  if (ImGui::Button("Export")) {
    try {
      std::error_code ec;
      fs::create_directories(fs::path(export_path_).parent_path(), ec);
      fs::path f = ctx_.p.export_saves(g.id, export_path_);
      ctx_.status = "exported to " + f.string();
    } catch (const std::exception& ex) {
      ctx_.status = ex.what();
    }
  }
  ImGui::SetNextItemWidth(-160);
  input_string("##import", import_path_, 1024);
  ImGui::SameLine();
  if (ImGui::Button("Import")) {
    try {
      ctx_.p.import_saves(g.id, import_path_);
      ctx_.status = "imported; what was there before is a snapshot below";
    } catch (const std::exception& ex) {
      ctx_.status = ex.what();
    }
  }
  ImGui::TextDisabled("importing keeps what is there now as a snapshot first");
  if (!ctx_.status.empty()) ImGui::TextWrapped("%s", ctx_.status.c_str());
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text("timeline");
  if (gens.empty()) {
    ImGui::TextWrapped("Nothing to go back to yet. Every session that writes something leaves a "
                       "snapshot here, newest first.");
  }
  ImGui::BeginChild("gens", ImVec2(0, 0), false);
  for (size_t i = gens.size(); i-- > 0;) {
    const std::string& gen = gens[i];
    ImGui::PushID(gen.c_str());
    const session::Record* rec = nullptr;
    for (const session::Record& r : recs) {
      if (r.generation == gen) rec = &r;
    }
    ImGui::Text("snapshot %s", gen.c_str());
    if (rec) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s, played %s, %zu files written", ago(rec->ended).c_str(),
                          human_time(static_cast<double>(rec->ended - rec->started)).c_str(),
                          rec->files_written);
    }
    ImGui::SameLine();
    // The popup is opened and drawn inside this PushID, so each snapshot has
    // its own.
    if (ImGui::SmallButton("Restore")) {
      restore_gen_ = gen;
      ImGui::OpenPopup("restore?");
    }
    if (ImGui::BeginPopupModal("restore?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Restore snapshot %s?", restore_gen_.c_str());
      ImGui::TextWrapped("What the game has written since is kept as a new snapshot first, so this "
                         "can be undone.");
      if (ImGui::Button("Restore")) {
        try {
          session::restore(g.id, restore_gen_);
          ctx_.status = "restored " + restore_gen_;
        } catch (const std::exception& ex) {
          ctx_.status = ex.what();
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
}

}  // namespace kg::gui::launcher
