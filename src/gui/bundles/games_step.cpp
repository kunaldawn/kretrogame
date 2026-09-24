#include <algorithm>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

#include "../../util/env.h"
#include "../../util/paths.h"
#include "../format.h"
#include "../widgets.h"
#include "bundles_page.h"

namespace kg::gui {
namespace fs = std::filesystem;
using namespace kg::bundle;

// ---- 2. games ---------------------------------------------------------------------------

void Bundles::games_step() {
  ImGui::TextWrapped("The games go into the file in this order, and the launcher shows them in it.");
  ImGui::Spacing();
  int move = -1, dir = 0, drop = -1;
  for (size_t i = 0; i < draft_.games.size(); ++i) {
    const DraftGame& g = draft_.games[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::Text("%zu.  %s", i + 1, g.name.c_str());
    const PackFacts* f = facts_for(g.id);
    ImGui::SameLine(420);
    if (!f) {
      warn_text("no longer on the shelf");
    } else {
      std::string discs = f->discs_carried ? "carries " + std::to_string(f->discs_carried) + " disc" +
                                                 (f->discs_carried == 1 ? "" : "s")
                                           : "carries no discs";
      if (f->discs_named) discs += ", needs " + std::to_string(f->discs_named) + " it does not carry";
      ImGui::TextDisabled("%s, %s", human_size(f->bytes).c_str(), discs.c_str());
      // Installed before packs stored what is already compressed raw, in
      // 4 MiB blocks: it plays, from a mount that reads more slowly.
      if (packed_before_faster_loading(*f)) {
        ImGui::SameLine();
        if (repacking_ == g.id) {
          ImGui::TextDisabled("repacking...");
        } else {
          ImGui::BeginDisabled(repacker_.joinable() || worker_.joinable());
          if (ImGui::SmallButton("Repack for faster loading")) start_repack(g.id);
          ImGui::EndDisabled();
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Packed before kretro stored packs for fast reads. Repacking keeps the game "
                              "exactly as it is - the same files, the same Merkle root, the same saves - and "
                              "changes only how its pack stores them.");
          }
        }
      }
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 260);
    ImGui::BeginDisabled(i == 0);
    if (ImGui::SmallButton("up")) { move = static_cast<int>(i); dir = -1; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 == draft_.games.size());
    if (ImGui::SmallButton("down")) { move = static_cast<int>(i); dir = 1; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("remove")) drop = static_cast<int>(i);
    ImGui::PopID();
  }
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
    if (!err.empty()) warn_text(err);
    if (!note.empty()) good_text(note);
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("On your shelf");
  bool any = false;
  for (const Entry& e : shelf_) {
    bool chosen = std::any_of(draft_.games.begin(), draft_.games.end(), [&](const DraftGame& g) { return g.id == e.id; });
    if (chosen) continue;
    const PackFacts* f = facts_for(e.id);
    if (!f) continue;
    any = true;
    ImGui::PushID(e.id.c_str());
    if (ImGui::SmallButton("add")) {
      // The shelf's own title screen, when it has one, is the first cover.
      draft_.games.push_back(game_from_pack(*f, e.title_png.empty() ? e.last_png : e.title_png));
      dirty_ = true;
      probe_imports();
    }
    ImGui::SameLine();
    ImGui::Text("%s", e.name.c_str());
    ImGui::SameLine(420);
    ImGui::TextDisabled("%s%s", human_size(f->bytes).c_str(),
                        f->discs_carried ? ", with its discs" : ", no discs");
    ImGui::PopID();
  }
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
