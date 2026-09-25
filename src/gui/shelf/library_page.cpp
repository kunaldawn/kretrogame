// The Library screen: every disc in the library folders, and whether it can
// be installed. The scan runs on the shelf's worker.
#include <algorithm>
#include <cmath>
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
#include "../palette.h"
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
  set_page_trail("kretro \xe2\x80\xba library");
  PageWindow page("Library", "Esc back", ctx_.fonts.big());

  config::Config cfg = config::load(config::config_file());
  std::vector<std::string> where = cfg.library_paths;
  if (where.empty()) where.push_back(install::iso_dir().string());

  // A column of a readable width down the middle of the page: the folders
  // and what to do with them on a card, and the discs under it.
  centre_column(content_max_w(Content::Reading));
  step_heading("Library");
  nav_section_begin("folders");
  card_begin("folders", "folders");
  for (const std::string& w : where) {
    ImGui::TextColored(kCyan, "%s", elide(nullptr, w, ImGui::GetContentRegionAvail().x).c_str());
  }
  vgap(4);

  if (ctx_.job.running()) {
    spinner();
    ImGui::SameLine();
    ImGui::TextDisabled("scanning...");
    card_end();
    nav_section_end();
    end_centre_column();
    return;
  }
  // Before the first scan, scanning is the thing to do on this page, and the
  // button says so. Either way the page opens on it.
  const float bh = std::round(px(breakpoint() == Breakpoint::Compact ? 36 : 40));
  const bool scan = scanned_ ? ghost_button("Scan again") : primary_button("Scan", ImVec2(0, bh));
  default_focus();
  if (scan) scan_library(where);
  ImGui::SameLine();
  // Import is reachable from the shelf, from here, and by dropping a pack on
  // the window. A pack somebody handed you is part of a collection, and this
  // is the screen where a collection is looked at; with nothing dropped yet
  // the page says so and asks for one.
  if (ghost_button("Import...")) ctx_.go(Screen::Import);
  if (!drop_note_.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", elide(nullptr, drop_note_, ImGui::GetContentRegionAvail().x).c_str());
  }
  card_end();
  nav_section_end();

  if (!scanned_) {
    section("discs");
    empty_state(
        "Nothing scanned yet. Scanning reads every archive in those folders and "
        "works out what discs are inside; on a large collection that takes a "
        "minute or two.");
    end_centre_column();
    return;
  }

  section("discs");
  // Names and titles take the room that is left and are cut short with an
  // ellipsis; the label, the size and the verdict are as wide as they need,
  // and what the database calls a disc gets more of the room than its file
  // name, as it is where a warning is said. The table scrolls in a region of
  // its own, so a long collection never runs off the bottom of the window,
  // and the keys go through it as through the page.
  nav_section_begin("discs");
  begin_scroll("discs-scroll", ImVec2(0, 0));
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
                                ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_PadOuterX;
  if (ImGui::BeginTable("discs", 5, flags)) {
    ImGui::TableSetupColumn("file", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("disc");
    ImGui::TableSetupColumn("size");
    ImGui::TableSetupColumn("known as", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("state");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    // The headings are labels, not places to stop: the arrows go from the
    // buttons above straight to the rows.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::TableHeadersRow();
    ImGui::PopItemFlag();
    ImGui::PopStyleColor();
    const float size_w = ImGui::CalcTextSize("000.0 MB").x;
    // Every row as tall as one with an Install button in it, with its text on
    // that button's baseline, so the stripes are even and the columns read
    // across on one line.
    const float row_h = small_button_height();
    for (size_t i = 0; i < rows_.size(); ++i) {
      const DiscRow& r = rows_[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
      ImGui::TableNextColumn();
      // The whole row is one stop for Up and Down, as a console's list is,
      // and the Install button in it is Right from there. Pressing the row
      // does what its Install button does, when it has one.
      const bool ready = r.state == "ready" && !r.game_id.empty();
      const float x = ImGui::GetCursorPosX();
      ImGui::PushID(static_cast<int>(i));
      const bool row = ImGui::Selectable("##row", false,
                                         ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                         ImVec2(0, row_h));
      ImGui::PopID();
      if (row && ready) wizard_.open_for(r.game_id);
      // The row's first cell drawn over the start of it.
      ImGui::SameLine(0, 0);
      ImGui::SetCursorPosX(x);
      align_to_small_button();
      ImGui::TextDisabled("%s", elide(nullptr, r.archive, ImGui::GetContentRegionAvail().x).c_str());
      ImGui::TableNextColumn();
      align_to_small_button();
      ImGui::TextColored(kCyan, "%s", r.label.empty() ? "-" : r.label.c_str());
      ImGui::TableNextColumn();
      align_to_small_button();
      // Right-aligned, so the sizes line up on their units.
      const std::string size = human_size(r.bytes);
      const float sw = ImGui::CalcTextSize(size.c_str()).x;
      ImGui::Dummy(ImVec2(std::max(0.0f, size_w - sw), 0));
      ImGui::SameLine(0, 0);
      ImGui::TextUnformatted(size.c_str());
      ImGui::TableNextColumn();
      align_to_small_button();
      {
        const std::string known = r.known_as.empty() ? "not in the database" : r.known_as;
        // A title the database knows whose bytes differ is worth a second
        // look, so it is said in the warning colour.
        const bool odd = known.find("(bytes differ") != std::string::npos;
        ImGui::TextColored(r.known_as.empty() ? kDim : (odd ? kWarm : kText), "%s",
                           elide(nullptr, known, ImGui::GetContentRegionAvail().x).c_str());
      }
      ImGui::TableNextColumn();
      if (ready) {
        ImGui::PushID(r.game_id.c_str());
        if (small_button("Install")) wizard_.open_for(r.game_id);
        ImGui::PopID();
      } else if (r.state == "installed") {
        align_to_small_button();
        badge(BadgeKind::Ok, "installed");
      } else {
        align_to_small_button();
        ImGui::TextDisabled("%s", r.state.c_str());
      }
    }
    ImGui::EndTable();
  }
  end_scroll();
  nav_section_end();
  end_centre_column();
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
