// The Library screen: every disc in the library folders, and whether it can
// be installed. The scan runs on the shelf's worker.
#include <algorithm>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../../config/config.h"
#include "../../disc/container.h"
#include "../../disc/database.h"
#include "../../disc/disc.h"
#include "../../install/collection.h"
#include "../../install/discs.h"
#include "../../install/manifest.h"
#include "../../util/paths.h"
#include "../../util/text.h"
#include "../format.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {
namespace fs = std::filesystem;

// The front door: every disc in your folders, what it is, and whether it can
// be installed. A multi-disc game whose first disc is not in any folder
// appears here as a set missing its first disc, which is the honest answer
// and more use than an install that fails a minute in.
void LibraryPage::draw() {
  PageWindow page("Library", "Esc back", ctx_.fonts.big);

  config::Config cfg = config::load(config::config_file());
  std::vector<std::string> where = cfg.library_paths;
  if (where.empty()) where.push_back(install::iso_dir().string());

  for (const std::string& w : where) ImGui::TextDisabled("  %s", w.c_str());
  ImGui::Spacing();

  if (ctx_.job.running()) {
    ImGui::TextDisabled("scanning...");
    return;
  }
  if (ImGui::Button(scanned_ ? "Scan again" : "Scan")) {
    scan_library(where);
  }
  ImGui::SameLine();
  // Import is reachable from the shelf, from here, and by dropping a pack on
  // the window. A pack somebody handed you is part of a collection, and this
  // is the screen where a collection is looked at; with nothing dropped yet
  // the page says so and asks for one.
  if (ImGui::Button("Import...")) ctx_.go(Screen::Import);
  if (!drop_note_.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", drop_note_.c_str());
  }
  ImGui::Spacing();

  if (!scanned_) {
    ImGui::TextWrapped(
        "Nothing scanned yet. Scanning reads every archive in those folders and "
        "works out what discs are inside; on a large collection that takes a "
        "minute or two.");
    return;
  }

  if (ImGui::BeginTable("discs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("file");
    ImGui::TableSetupColumn("disc");
    ImGui::TableSetupColumn("size");
    ImGui::TableSetupColumn("known as");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (const DiscRow& r : rows_) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", r.archive.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%s", r.label.empty() ? "-" : r.label.c_str());
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", human_size(r.bytes).c_str());
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", r.known_as.empty() ? "not in the database" : r.known_as.c_str());
      ImGui::TableNextColumn();
      if (r.state == "ready" && !r.game_id.empty()) {
        ImGui::PushID(r.game_id.c_str());
        if (ImGui::SmallButton("Install")) wizard_.open_for(r.game_id);
        ImGui::PopID();
      } else {
        ImGui::TextDisabled("%s", r.state.c_str());
      }
    }
    ImGui::EndTable();
  }
}

void LibraryPage::scan_library(const std::vector<std::string>& where) {
  // A scan of a real collection takes minutes, so it runs where every other
  // long job in this program runs: on the worker, with the log modal up.
  rows_.clear();
  std::vector<fs::path> files;
  std::error_code ec;
  for (const std::string& w : where) {
    for (const auto& de : fs::directory_iterator(w, ec)) {
      if (de.is_regular_file(ec)) files.push_back(de.path());
    }
  }
  std::sort(files.begin(), files.end());
  if (!library_only_.empty()) {
    files.clear();
    files.push_back(library_only_);
  }

  ctx_.run_job("Scanning your collection", [this, files]() {
    std::vector<iso::Known> db = iso::load_database(ctx_.env);
    std::vector<std::string> manifests = install::known_games(ctx_.env);
    std::vector<DiscRow> rows;
    for (const fs::path& f : files) {
      // A scan of a real collection is minutes of reading archives. Nothing
      // here was interruptible before, so the only way out was to close the
      // window and lose the scan.
      if (ctx_.job.cancelled()) { ctx_.job.log("stopped"); break; }
      std::vector<disc::Candidate> cands;
      try {
        cands = disc::probe(ctx_.env, f);
      } catch (const std::exception&) { continue; }
      if (cands.empty()) continue;
      ctx_.job.log(f.filename().string());

      fs::path work = fs::temp_directory_path() / "kretro-gui-scan";
      std::error_code e2;
      std::vector<disc::Disc> opened;
      for (size_t i = 0; i < cands.size(); ++i) {
        try {
          opened.push_back(disc::open(ctx_.env, cands[i], work / std::to_string(i), false));
        } catch (const std::exception&) {}
        fs::remove_all(work / std::to_string(i), e2);
      }
      disc::DiscSet set = disc::assemble(disc::set_id_from(f.stem().string()),
                                         f.stem().string(), std::move(opened));
      for (const disc::Disc& d : set.discs) {
        DiscRow r;
        r.archive = f.filename().string();
        r.label = d.label;
        r.bytes = d.info.size;
        iso::Match m = iso::identify(db, d.info);
        if (m.entry) {
          r.known_as = m.entry->name;
          if (m.suspect_bad_dump) r.known_as += "  (bytes differ - a bad dump?)";
        }
        r.state = "no manifest wants this";
        for (const std::string& id : manifests) {
          fs::path mp = install::find_manifest(ctx_.env, id);
          if (mp.empty()) continue;
          Meta mm;
          try { mm = install::load_manifest(mp); } catch (const std::exception&) { continue; }
          bool wants = false;
          for (const std::string& ref : mm.recipe.discs) {
            install::DiscRef dr = install::parse_disc_ref(ref);
            if (dr.archive != r.archive) continue;
            if (dr.label.empty() || to_lower(dr.label) == to_lower(r.label)) wants = true;
          }
          if (!wants) continue;
          r.game_id = id;
          r.state = fs::exists(game_pack(id), e2) ? "installed" : "ready";
          break;
        }
        rows.push_back(r);
      }
      fs::remove_all(work, e2);
    }
    ctx_.job.locked([&] {
      rows_ = rows;
      scanned_ = true;
    });
  });
}

void LibraryPage::add_folder(const fs::path& p) {
  config::Config c = config::load(config::config_file());
  if (std::find(c.library_paths.begin(), c.library_paths.end(), p.string()) ==
      c.library_paths.end()) {
    c.library_paths.push_back(p.string());
    config::save(config::config_file(), c);
  }
  drop_note_ = p.filename().string() + " added to your library folders";
  scanned_ = false;
  ctx_.go(Screen::Library);
}

void LibraryPage::scan_only(const fs::path& p) {
  // library_only_ is what the Library screen reads; drop_path_ belongs to
  // the Import page and nothing else. Setting it here set nothing looking
  // at it and left an .iso behind for the next visit to Import, which
  // opens on whatever drop_path_ holds and can only say it cannot read it.
  drop_note_ = "dropped: " + p.filename().string();
  library_only_ = p;
  scanned_ = false;
  ctx_.go(Screen::Library);
}

}  // namespace kg::gui::shelf
