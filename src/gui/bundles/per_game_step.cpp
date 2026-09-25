#include <algorithm>
#include <cfloat>
#include <cmath>
#include <initializer_list>
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
  section("each game");
  if (draft_.games.empty()) {
    ImGui::TextDisabled("Choose games first.");
    return;
  }
  if (game_pick_ >= draft_.games.size()) game_pick_ = 0;
  ImGui::SetNextItemWidth(field_width(420));
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
  ImGui::SameLine(0, std::round(px(12)));
  ImGui::AlignTextToFramePadding();
  elided_text(g.id, kCyan);
  ImGui::PushID(g.id.c_str());

  // Every form on this step shares one label column, so their fields line up.
  const std::initializer_list<const char*> labels = {"Name", "Year", "Cover", "Graphics",
                                                     "Display", "Gamepad", "Registry key", "Value name"};
  if (form_begin("game", labels)) {
    form_row("Name");
    ImGui::SetNextItemWidth(field_width(420));
    if (input_string("##name", g.name, 128)) dirty_ = true;
    form_row("Year");
    int year = static_cast<int>(g.year);
    ImGui::SetNextItemWidth(field_width(160));
    if (ImGui::InputInt("##year", &year, 0)) {
      g.year = static_cast<uint32_t>(std::clamp(year, 0, 9999));
      dirty_ = true;
    }
    form_row("Cover");
    // Full-height buttons, as tall as the fields above them.
    if (ImGui::Button("Choose PNG...")) browse_ = Browse::Cover;
    if (!g.cover.empty()) {
      ImGui::SameLine();
      if (ImGui::Button("Remove")) {
        g.cover.clear();
        g.cover_from.clear();
        dirty_ = true;
      }
    }
    ImGui::SameLine(0, std::round(px(12)));
    if (g.cover.empty()) ImGui::TextDisabled("none");
    else elided_text(picture_words(g.cover) + "  (" + fs::path(g.cover_from).filename().string() + ")");
    ImGui::EndTable();
  }

  // Graphics, with what auto would pick said beside it.
  section("graphics");
  if (form_begin("graphics", labels)) {
    form_row("Graphics");
    int b = index_of(kBackendNames, g.backend);
    ImGui::SetNextItemWidth(field_width(300));
    if (ImGui::Combo("##backend", &b, kBackendWords, 5)) {
      g.backend = std::string(kBackendNames[b]);
      dirty_ = true;
    }
    std::optional<pe::Imports> im = imports_for(g.id);
    if (!im && !probing_) probe_imports();
    if (!im) {
      if (probing_) {
        spinner();
        ImGui::SameLine();
      }
      ImGui::TextDisabled(probing_ ? "reading the game's executable..." : "the executable has not been read");
    } else if (!im->ok) {
      colored_text(kDim, "auto cannot read the executable (" + im->error + "); the player falls back to WineD3D");
    } else {
      AutoBackend a = auto_backend(*im);
      std::string dlls;
      for (const std::string& d : im->dlls) dlls += (dlls.empty() ? "" : " ") + d;
      ImGui::TextWrapped("auto: %s. %s.", a.backend.c_str(), a.reason.c_str());
      ImGui::TextDisabled("imports:");
      ImGui::SameLine();
      ImGui::BeginGroup();
      colored_text(kCyan, dlls);
      ImGui::EndGroup();
    }
    if (tick_box("Needs a GPU (refuse to start with software rendering)", &g.needs_gpu, true)) dirty_ = true;

    form_row("Display");
    int dm = index_of(kDisplayModes, g.display);
    ImGui::SetNextItemWidth(field_width(300));
    if (ImGui::Combo("##display", &dm, kDisplayWords, 3)) {
      g.display = std::string(kDisplayModes[dm]);
      dirty_ = true;
    }
    // Set well apart from the combo, so it reads as a choice of its own and
    // not as part of the scaling.
    {
      const float w = ImGui::GetFrameHeight() + ImGui::CalcTextSize("fullscreen").x + ImGui::GetStyle().ItemInnerSpacing.x;
      ImGui::SameLine(0, std::round(px(24)));
      if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
    }
    if (tick_box("fullscreen", &g.fullscreen)) dirty_ = true;

    form_row("Gamepad", false);
    ImGui::TextDisabled(g.gamepad.empty() ? "kretro's default map" : "this map, one button=key a line");
    if (f && !f->meta.input.empty()) {
      same_line_if_fits(small_button_width("the map from its pack"));
      if (small_button("the map from its pack")) {
        g.gamepad = format_gamepad(f->meta.input);
        dirty_ = true;
      }
    }
    if (!g.gamepad.empty()) {
      same_line_if_fits(small_button_width("kretro's default"));
      if (small_button("kretro's default")) {
        g.gamepad.clear();
        dirty_ = true;
      }
      const float h = ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2;
      if (input_string_multiline("##pad", g.gamepad, 2048, ImVec2(-FLT_MIN, h))) dirty_ = true;
    }
    ImGui::EndTable();
  }

  // dgVoodoo: never ours to ship, sometimes the author's.
  section("dgVoodoo and other files for this game");
  ImGui::TextWrapped(
      "kretro never bundles dgVoodoo: its licence forbids shipping it with a launcher for general use. "
      "You may add its files for this one game, as its author allows; they are placed beside the game's "
      "executable. Whether you may distribute them is yours to know.");
  ImGui::Spacing();
  int drop = -1;
  const float cell = ImGui::CalcTextSize("0").x;
  if (!g.extra_dlls.empty() &&
      ImGui::BeginTable("files", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
    ImGui::TableSetupColumn("file", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, cell * 9);
    ImGui::TableSetupColumn("remove", ImGuiTableColumnFlags_WidthFixed, small_button_width("remove"));
    for (size_t i = 0; i < g.extra_dlls.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      elided_text(g.extra_dlls[i].name, kCyan);
      ImGui::TableSetColumnIndex(1);
      right_aligned(human_size(g.extra_dlls[i].data.size()));
      ImGui::TableSetColumnIndex(2);
      if (small_button("remove")) drop = static_cast<int>(i);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (drop >= 0) {
    g.extra_dlls.erase(g.extra_dlls.begin() + drop);
    dirty_ = true;
  }
  if (small_button("Add files...")) browse_ = Browse::Dll;

  // The CD key.
  section("CD key");
  std::string vault = install::key_for(keys_, g.id);
  std::string fragment = f ? f->meta.registry.fragment : std::string();
  if (vault.empty()) {
    colored_text(kDim, "The keys vault has no key for " + g.id + " (kretro key " + g.id + " <key> stores one).");
  }
  bool embed = g.embed_key;
  if (tick_box("Embed my key in this bundle", &embed)) {
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
  if (dialog_begin("embed-key", 600, "Embed your key?")) {
    badge_line(BadgeKind::Warn,
               "Your key will be in every copy of this bundle, and anyone who has a copy can read it. If it "
               "is shared further, it is your key that is shared.");
    dialog_footer();
    if (dialog_button("Embed it", DialogButton::Danger)) {
      g.embed_key = true;
      if (g.key_path.empty()) {
        KeySpot spot = suggest_key_spot(fragment, vault);
        g.key_path = spot.path;
        g.key_value = spot.value;
      }
      dirty_ = true;
      ImGui::CloseCurrentPopup();
    }
    // The answer that leaves the key where it is, which Escape and the pad's
    // B give too, and where the focus starts.
    if (dialog_button("Keep it out", DialogButton::Secondary, true)) ImGui::CloseCurrentPopup();
    dialog_end();
  }
  if (g.embed_key) {
    badge_line(BadgeKind::Warn, "Embedded: in every copy, readable by anyone.");
    if (form_begin("key", labels)) {
      form_row("Registry key");
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (input_string("##keypath", g.key_path, 512)) dirty_ = true;
      form_row("Value name");
      ImGui::SetNextItemWidth(field_width(300));
      if (input_string("##keyvalue", g.key_value, 128)) dirty_ = true;
      ImGui::EndTable();
    }
    colored_text(kDim,
                 "Where the game's installer keeps its key. This pack does not record the name it "
                 "used; the game's own key under Software is the guess.");
    // Said here because it changes where the key lands: the player writes a
    // 32-bit game's HKLM\Software key under Wow6432Node, where it reads it.
    std::optional<pe::Imports> im = imports_for(g.id);
    if (im && im->ok) {
      colored_text(kDim, im->is64 ? "The game is 64-bit: the key is written at this path as it is."
                                  : "The game is 32-bit: a key under HKLM\\Software is written under "
                                    "Software\\Wow6432Node, which is where a 32-bit program reads it.");
    }
  }
  ImGui::PopID();
}

}  // namespace kg::gui
