// One game's saves: exporting and importing them, and the snapshots every
// session leaves, newest first, to go back to.
#include <algorithm>
#include <cmath>
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
  set_chrome_path("~/" + path_part(g.name) + "/saves");
  set_trail(ctx_, g.name, "saves");
  PageWindow page((g.name + " - saves").c_str(), "Esc back", ctx_.w.fonts().big());
  // Read when the page comes up, after a session, an import or a restore,
  // not on every frame.
  if (disk_.due(g.id + "\n" + std::to_string(ctx_.generation))) {
    gens_ = session::generations(g.id);
    recs_ = session::journal(g.id);
  }
  const std::vector<std::string>& gens = gens_;
  const std::vector<session::Record>& recs = recs_;

  // One region scrolls the page: the way the saves go out and come in, in a
  // band across it, and the snapshots under that as cards.
  begin_page_scroll("page");
  // The page's things in a column down the middle, as the other settings
  // pages have them; the band behind the actions still runs edge to edge.
  centre_column(content_max_w(Content::Form));
  step_heading("Saves");
  nav_section_begin("actions");
  action_bar_begin("transfer");
  if (export_path_.empty()) {
    export_path_ = (state_dir() / "exports" / (g.id + ".saves.kgpack")).string();
  }
  // Both buttons as wide as the wider one, so the two fields end together,
  // and the fields no wider than reads well.
  const ImGuiStyle& st = ImGui::GetStyle();
  const float bw = std::max(ImGui::CalcTextSize("Export").x, ImGui::CalcTextSize("Import").x) + st.FramePadding.x * 2;
  const float field = std::max(std::round(px(120)), ImGui::GetContentRegionAvail().x - bw - st.ItemSpacing.x);
  ImGui::SetNextItemWidth(field);
  input_string("##export", export_path_, 1024);
  ImGui::SameLine();
  const bool exported = ImGui::Button("Export", ImVec2(bw, 0));
  // The page opens on Export rather than on the field before it, which a
  // keyboard would otherwise start typing into.
  default_focus();
  if (exported) {
    try {
      std::error_code ec;
      fs::create_directories(fs::path(export_path_).parent_path(), ec);
      fs::path f = ctx_.p.export_saves(g.id, export_path_);
      ctx_.status = "exported to " + f.string();
    } catch (const std::exception& ex) {
      ctx_.status = ex.what();
    }
  }
  ImGui::SetNextItemWidth(field);
  input_string("##import", import_path_, 1024);
  // What goes in the empty field, dim inside it the way a hint is, until
  // something is typed.
  if (import_path_.empty() && !ImGui::IsItemActive()) {
    const ImVec2 in = ImGui::GetItemRectMin();
    const ImVec2 fp = st.FramePadding;
    ImGui::GetWindowDrawList()->AddText(ImVec2(in.x + fp.x, in.y + fp.y), u32(kDim), "path to a .kgpack to import");
  }
  ImGui::SameLine();
  if (ImGui::Button("Import", ImVec2(bw, 0))) {
    try {
      ctx_.p.import_saves(g.id, import_path_);
      ctx_.status = "imported; what was there before is a snapshot below";
    } catch (const std::exception& ex) {
      ctx_.status = ex.what();
    }
    disk_.invalidate();
  }
  ImGui::TextDisabled("importing keeps what is there now as a snapshot first");
  action_bar_end();
  nav_section_end();
  if (!ctx_.status.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", ctx_.status.c_str());
  }
  vgap(6);
  section("timeline");
  vgap(4);
  if (gens.empty()) {
    empty_state(
        "Nothing to go back to yet. Every session that writes something leaves a "
        "snapshot here, newest first.",
        nullptr, 0);
    end_centre_column();
    end_page_scroll();
    return;
  }

  // A card per snapshot, newest first, on a rail down the left, as the
  // timeline on kretro's shelf has them; the Restore buttons one column down
  // the right, so Up and Down go from card to card.
  nav_section_begin("snapshots");
  const float column = std::min(ImGui::GetContentRegionAvail().x, content_max_w(Content::Reading));
  for (size_t i = gens.size(); i-- > 0;) {
    const std::string& gen = gens[i];
    ImGui::PushID(gen.c_str());
    SnapshotCard card;
    card.gen = gen;
    card.id = g.id;
    card.width = column;
    card.first = i + 1 == gens.size();
    card.last = i == 0;
    for (const session::Record& r : recs) {
      if (r.generation != gen) continue;
      card.when = ago(r.ended);
      card.played = human_time(static_cast<double>(r.ended - r.started));
      card.files = std::to_string(r.files_written) + " files written";
      card.note = r.note;
    }
    // The popup is opened and drawn inside this PushID, so each snapshot has
    // its own.
    if (snapshot_card(card)) {
      restore_gen_ = gen;
      ImGui::OpenPopup("restore?");
    }
    if (dialog_begin("restore?", 600, "Restore snapshot " + restore_gen_ + "?")) {
      ImGui::TextWrapped("What the game has written since is kept as a new snapshot first, so this "
                         "can be undone.");
      dialog_footer();
      // What is there now is replaced, so the button says it in the colour
      // that means care, and the green is kept for going ahead.
      if (dialog_button("Restore", DialogButton::Danger)) {
        try {
          session::restore(g.id, restore_gen_);
          ctx_.status = "restored " + restore_gen_;
        } catch (const std::exception& ex) {
          ctx_.status = ex.what();
        }
        disk_.invalidate();
        ImGui::CloseCurrentPopup();
      }
      // Cancel is what Escape and the pad's B answer, and where the focus
      // starts.
      if (dialog_button("Cancel", DialogButton::Secondary, true)) ImGui::CloseCurrentPopup();
      dialog_end();
    }
    ImGui::PopID();
  }
  nav_section_end();
  end_centre_column();
  end_page_scroll();
}

}  // namespace kg::gui::launcher
