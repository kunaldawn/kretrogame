#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>

#include "../../util/env.h"
#include "../../util/paths.h"
#include "../format.h"
#include "../widgets.h"
#include "bundles_page.h"
#include "choices.h"

namespace kg::gui {
namespace fs = std::filesystem;
using namespace kg::bundle;

// ---- 2. games ---------------------------------------------------------------------------

namespace {

// A table's header row the terminal way: dim words over the columns, and a
// rule under them. `right` marks the columns whose numbers sit to the right.
void header_row(std::initializer_list<const char*> words, uint32_t right = 0) {
  ImGui::TableNextRow();
  int i = 0;
  for (const char* w : words) {
    ImGui::TableSetColumnIndex(i);
    if (right & (1u << i)) right_aligned(w, kDim);
    else ImGui::TextDisabled("%s", w);
    ++i;
  }
}

// The sizing both tables here share: rows a little apart, rules between them.
constexpr ImGuiTableFlags kListFlags =
    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings;

// The columns both tables share, so that a game's size and discs sit at the
// same place whether it is in the bundle or still on the shelf: a lead column
// (the number, or the button that adds it), the game, its size, its discs,
// and the bundle's order buttons, left empty on the shelf.
void list_columns() {
  const float cell = ImGui::CalcTextSize("0").x;
  ImGui::TableSetupColumn("lead", ImGuiTableColumnFlags_WidthFixed, std::max(cell * 3, small_button_width("add")));
  ImGui::TableSetupColumn("game", ImGuiTableColumnFlags_WidthStretch, 3.0f);
  ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, cell * 9);
  ImGui::TableSetupColumn("discs", ImGuiTableColumnFlags_WidthStretch, 2.0f);
  ImGui::TableSetupColumn("order", ImGuiTableColumnFlags_WidthFixed,
                          small_button_width("up") + small_button_width("down") + small_button_width("remove") +
                              ImGui::GetStyle().ItemSpacing.x * 2);
}

}  // namespace

void Bundles::games_step() {
  section("games");
  ImGui::TextWrapped("The games go into the file in this order, and the launcher shows them in it.");
  ImGui::Spacing();
  int move = -1, dir = 0, drop = -1;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(std::round(px(8)), std::round(px(5))));
  const bool table = !draft_.games.empty() && ImGui::BeginTable("games", 5, kListFlags);
  ImGui::PopStyleVar();
  if (table) {
    list_columns();
    header_row({"#", "game", "size", "discs", ""}, 1u << 2);
  }
  for (size_t i = 0; table && i < draft_.games.size(); ++i) {
    const DraftGame& g = draft_.games[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    right_aligned(std::to_string(i + 1) + ".", kDim);
    ImGui::TableSetColumnIndex(1);
    elided_text(g.name);
    const PackFacts* f = facts_for(g.id);
    if (!f) {
      ImGui::TableSetColumnIndex(3);
      warn_text("no longer on the shelf");
    } else {
      // Installed before packs stored what is already compressed raw, in
      // 4 MiB blocks: it plays, from a mount that reads more slowly.
      if (packed_before_faster_loading(*f)) {
        if (repacking_ == g.id) {
          spinner();
          ImGui::SameLine();
          ImGui::TextDisabled("repacking...");
        } else {
          // A quiet button, dim and unfilled, under the title: an offer that
          // can wait, not the row's subject.
          ImGui::BeginDisabled(repacker_.joinable() || worker_.joinable());
          ImGui::PushStyleColor(ImGuiCol_Text, kDim);
          ImGui::PushStyleColor(ImGuiCol_Button, alpha(kBg0, 0.0f));
          ImGui::PushStyleColor(ImGuiCol_Border, kLine);
          const bool repack = small_button("Repack for faster loading");
          ImGui::PopStyleColor(3);
          if (repack) start_repack(g.id);
          ImGui::EndDisabled();
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Packed before kretro stored packs for fast reads. Repacking keeps the game "
                              "exactly as it is - the same files, the same Merkle root, the same saves - and "
                              "changes only how its pack stores them.");
          }
        }
      }
      ImGui::TableSetColumnIndex(2);
      right_aligned(human_size(f->bytes));
      ImGui::TableSetColumnIndex(3);
      std::string discs = f->discs_carried ? "carries " + std::to_string(f->discs_carried) + " disc" +
                                                 (f->discs_carried == 1 ? "" : "s")
                                           : "carries no discs";
      if (f->discs_named) discs += ", needs " + std::to_string(f->discs_named) + " it does not carry";
      colored_text(kDim, discs);
    }
    ImGui::TableSetColumnIndex(4);
    ImGui::BeginDisabled(i == 0);
    if (small_button("up")) {
      move = static_cast<int>(i);
      dir = -1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 == draft_.games.size());
    if (small_button("down")) {
      move = static_cast<int>(i);
      dir = 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (small_button("remove")) drop = static_cast<int>(i);
    ImGui::PopID();
  }
  if (table) ImGui::EndTable();
  if (move >= 0) {
    std::swap(draft_.games[move], draft_.games[move + dir]);
    dirty_ = true;
  }
  if (drop >= 0) {
    draft_.games.erase(draft_.games.begin() + drop);
    game_pick_ = 0;
    dirty_ = true;
  }
  if (draft_.games.empty()) ImGui::TextDisabled("No games yet: add some from the shelf below.");
  {
    std::string err, note = repack_note_;
    {
      std::lock_guard<std::mutex> lk(build_mutex_);
      err = repack_error_;
    }
    if (!err.empty()) badge_line(BadgeKind::Fail, err);
    if (!note.empty()) badge_line(BadgeKind::Ok, note);
  }

  section("on your shelf");
  auto offered = [&](const Entry& e) {
    const bool chosen =
        std::any_of(draft_.games.begin(), draft_.games.end(), [&](const DraftGame& g) { return g.id == e.id; });
    return !chosen && facts_for(e.id) != nullptr;
  };
  // Counted first, so an empty shelf shows its sentence without a header
  // over no rows.
  const bool any = std::any_of(shelf_.begin(), shelf_.end(), offered);
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(std::round(px(8)), std::round(px(5))));
  const bool shelf = any && ImGui::BeginTable("shelf", 5, kListFlags);
  ImGui::PopStyleVar();
  if (shelf) {
    list_columns();
    header_row({"", "game", "size", "discs", ""}, 1u << 2);
  }
  for (const Entry& e : shelf_) {
    if (!shelf) break;
    if (!offered(e)) continue;
    const PackFacts* f = facts_for(e.id);
    ImGui::PushID(e.id.c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (small_button("add")) {
      // The shelf's own title screen, when it has one, is the first cover.
      draft_.games.push_back(game_from_pack(*f, e.title_png.empty() ? e.last_png : e.title_png));
      dirty_ = true;
      probe_imports();
    }
    ImGui::TableSetColumnIndex(1);
    elided_text(e.name);
    ImGui::TableSetColumnIndex(2);
    right_aligned(human_size(f->bytes));
    ImGui::TableSetColumnIndex(3);
    ImGui::TextDisabled("%s", f->discs_carried ? "with its discs" : "no discs");
    ImGui::PopID();
  }
  if (shelf) ImGui::EndTable();
  if (!any) ImGui::TextDisabled(shelf_.empty() ? "Nothing is installed yet." : "Every installed game is in this bundle.");
}

void Bundles::start_repack(const std::string& id) {
  if (repacker_.joinable() || worker_.joinable()) return;
  const PackFacts* f = facts_for(id);
  if (!f) return;
  repacking_ = id;
  repack_note_.clear();
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    repack_error_.clear();
  }
  repack_done_ = false;
  cancel_ = false;
  fs::path pack = f->path, tool = env_or_empty("KRETRO_DWARFS"), scratch = cache_dir() / "repack";
  repacker_ = std::thread([this, pack, tool, scratch] {
    std::string err;
    try {
      Callbacks cb;
      cb.cancelled = [this] { return cancel_.load(); };
      repack_for_faster_loading(pack, tool, scratch, cb);
    } catch (const Cancelled&) {
      err = "Repack cancelled; the pack is as it was.";
    } catch (const std::exception& ex) {
      err = ex.what();
    }
    std::lock_guard<std::mutex> lk(build_mutex_);
    repack_error_ = err;
    repack_done_ = true;
  });
}

void Bundles::pump_repack() {
  if (!repacker_.joinable() || !repack_done_) return;
  repacker_.join();
  repack_done_ = false;
  const std::string id = repacking_;
  repacking_.clear();
  std::string err;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    err = repack_error_;
  }
  // The pack's size and body changed; its tree did not.
  if (auto it = facts_.find(id); it != facts_.end()) {
    try {
      it->second = read_pack_facts(it->second.path);
    } catch (const std::exception&) {
    }
    if (err.empty()) repack_note_ = "Repacked " + it->second.meta.name + " for faster loading: " +
                                    human_size(it->second.bytes) + ", the same game.";
  }
}

}  // namespace kg::gui
