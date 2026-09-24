#include <algorithm>
#include <optional>
#include <string>

#include "../../bundle/gamepad.h"
#include "../format.h"
#include "../widgets.h"
#include "bundles_page.h"
#include "choices.h"

namespace kg::gui {
namespace fs = std::filesystem;
using namespace kg::bundle;
using bundles_detail::index_of;
using bundles_detail::kBackendWords;
using bundles_detail::kDisplayWords;
using bundles_detail::picture_words;

// ---- 3. each game ---------------------------------------------------------------------------

void Bundles::per_game_step() {
  if (draft_.games.empty()) {
    ImGui::TextDisabled("Choose games first.");
    return;
  }
  if (game_pick_ >= draft_.games.size()) game_pick_ = 0;
  ImGui::SetNextItemWidth(420);
  if (ImGui::BeginCombo("##game", draft_.games[game_pick_].name.c_str())) {
    for (size_t i = 0; i < draft_.games.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(draft_.games[i].name.c_str(), i == game_pick_)) game_pick_ = i;
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  DraftGame& g = draft_.games[game_pick_];
  const PackFacts* f = facts_for(g.id);
  ImGui::PushID(g.id.c_str());
  ImGui::Separator();

  ImGui::TextUnformatted("Name");
  ImGui::SameLine(200);
  ImGui::SetNextItemWidth(420);
  if (input_string("##name", g.name, 128)) dirty_ = true;
  ImGui::TextUnformatted("Year");
  ImGui::SameLine(200);
  int year = static_cast<int>(g.year);
  ImGui::SetNextItemWidth(160);
  if (ImGui::InputInt("##year", &year, 0)) {
    g.year = static_cast<uint32_t>(std::clamp(year, 0, 9999));
    dirty_ = true;
  }
  ImGui::TextUnformatted("Cover");
  ImGui::SameLine(200);
  if (g.cover.empty()) ImGui::TextDisabled("none");
  else ImGui::Text("%s  (%s)", picture_words(g.cover).c_str(), fs::path(g.cover_from).filename().c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Choose PNG...")) browse_ = Browse::Cover;
  if (!g.cover.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) {
      g.cover.clear();
      g.cover_from.clear();
      dirty_ = true;
    }
  }

  // Graphics, with what auto would pick said beside it.
  ImGui::Spacing();
  ImGui::TextUnformatted("Graphics");
  ImGui::SameLine(200);
  int b = index_of(kBackendNames, g.backend);
  ImGui::SetNextItemWidth(300);
  if (ImGui::Combo("##backend", &b, kBackendWords, 5)) {
    g.backend = std::string(kBackendNames[b]);
    dirty_ = true;
  }
  std::optional<pe::Imports> im = imports_for(g.id);
  if (!im && !probing_) probe_imports();
  if (!im) {
    ImGui::TextDisabled(probing_ ? "reading the game's executable..." : "the executable has not been read");
  } else if (!im->ok) {
    ImGui::TextDisabled("auto cannot read the executable (%s); the player falls back to WineD3D", im->error.c_str());
  } else {
    AutoBackend a = auto_backend(*im);
    std::string dlls;
    for (const std::string& d : im->dlls) dlls += (dlls.empty() ? "" : " ") + d;
    ImGui::TextWrapped("auto: %s. %s.", a.backend.c_str(), a.reason.c_str());
    ImGui::TextDisabled("imports: %s", dlls.c_str());
  }
  if (ImGui::Checkbox("Needs a GPU (refuse to start with software rendering)", &g.needs_gpu)) dirty_ = true;

  ImGui::TextUnformatted("Display");
  ImGui::SameLine(200);
  int dm = index_of(kDisplayModes, g.display);
  ImGui::SetNextItemWidth(300);
  if (ImGui::Combo("##display", &dm, kDisplayWords, 3)) {
    g.display = std::string(kDisplayModes[dm]);
    dirty_ = true;
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("fullscreen", &g.fullscreen)) dirty_ = true;

  ImGui::TextUnformatted("Gamepad");
  ImGui::SameLine(200);
  ImGui::TextDisabled(g.gamepad.empty() ? "kretro's default map" : "this map, one button=key a line");
  if (f && !f->meta.input.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("the map from its pack")) {
      g.gamepad = format_gamepad(f->meta.input);
      dirty_ = true;
    }
  }
  if (!g.gamepad.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("kretro's default")) {
      g.gamepad.clear();
      dirty_ = true;
    }
    if (input_string_multiline("##pad", g.gamepad, 2048, ImVec2(-1, 90))) dirty_ = true;
  }

  // dgVoodoo: never ours to ship, sometimes the author's.
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("dgVoodoo and other files for this game");
  ImGui::TextWrapped(
      "kretro never bundles dgVoodoo: its licence forbids shipping it with a launcher for general use. "
      "You may add its files for this one game, as its author allows; they are placed beside the game's "
      "executable. Whether you may distribute them is yours to know.");
  int drop = -1;
  for (size_t i = 0; i < g.extra_dlls.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    ImGui::Text("%s  %s", g.extra_dlls[i].name.c_str(), human_size(g.extra_dlls[i].data.size()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("remove")) drop = static_cast<int>(i);
    ImGui::PopID();
  }
  if (drop >= 0) {
    g.extra_dlls.erase(g.extra_dlls.begin() + drop);
    dirty_ = true;
  }
  if (ImGui::SmallButton("Add files...")) browse_ = Browse::Dll;

  // The CD key.
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("CD key");
  std::string vault = install::key_for(keys_, g.id);
  std::string fragment = f ? f->meta.registry.fragment : std::string();
  if (vault.empty()) {
    ImGui::TextDisabled("The keys vault has no key for %s (kretro key %s <key> stores one).", g.id.c_str(),
                        g.id.c_str());
  }
  bool embed = g.embed_key;
  if (ImGui::Checkbox("Embed my key in this bundle", &embed)) {
    if (embed) {
      confirm_key_ = true;  // only after the warning
    } else {
      g.embed_key = false;
      dirty_ = true;
    }
  }
  if (confirm_key_) {
    ImGui::OpenPopup("embed-key");
    confirm_key_ = false;
  }
  if (ImGui::BeginPopupModal("embed-key", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushTextWrapPos(560);
    warn_text("Your key will be in every copy of this bundle, and anyone who has a copy can read it. If it "
              "is shared further, it is your key that is shared.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Embed it")) {
      g.embed_key = true;
      if (g.key_path.empty()) {
        KeySpot spot = suggest_key_spot(fragment, vault);
        g.key_path = spot.path;
        g.key_value = spot.value;
      }
      dirty_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep it out")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (g.embed_key) {
    warn_text("Embedded: in every copy, readable by anyone.");
    ImGui::TextUnformatted("Registry key");
    ImGui::SameLine(200);
    ImGui::SetNextItemWidth(-1);
    if (input_string("##keypath", g.key_path, 512)) dirty_ = true;
    ImGui::TextUnformatted("Value name");
    ImGui::SameLine(200);
    ImGui::SetNextItemWidth(300);
    if (input_string("##keyvalue", g.key_value, 128)) dirty_ = true;
    ImGui::TextDisabled("Where the game's installer keeps its key. This pack does not record the name it "
                        "used; the game's own key under Software is the guess.");
    // Said here because it changes where the key lands: the player writes a
    // 32-bit game's HKLM\Software key under Wow6432Node, where it reads it.
    std::optional<pe::Imports> im = imports_for(g.id);
    if (im && im->ok) {
      ImGui::TextDisabled(im->is64 ? "The game is 64-bit: the key is written at this path as it is."
                                   : "The game is 32-bit: a key under HKLM\\Software is written under "
                                     "Software\\Wow6432Node, which is where a 32-bit program reads it.");
    }
  }
  ImGui::PopID();
}

}  // namespace kg::gui
