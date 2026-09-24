// One game's display and controls, each kept in its settings.toml as soon as
// it changes.
#include <cstdint>
#include <string>

#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {

void DisplayPage::draw() {
  const bundle::GameMeta& g = ctx_.p.game(ctx_.selected);
  PageWindow page((g.name + " - display").c_str(), "Esc back", ctx_.w.big);
  player::GameSettings s = ctx_.p.settings(g.id);
  bool dirty = false;
  const char* modes[] = {"integer", "fit", "native"};
  int m = static_cast<int>(s.display.mode);
  if (ImGui::Combo("scaling", &m, modes, 3)) {
    s.display.mode = static_cast<config::ScaleMode>(m);
    dirty = true;
  }
  int sc = static_cast<int>(s.display.scale);
  if (ImGui::SliderInt("scale", &sc, 0, 6, sc == 0 ? "the largest that fits" : "%dx")) {
    s.display.scale = static_cast<uint32_t>(sc);
    dirty = true;
  }
  int fs_mode = s.fullscreen ? 1 : 0;
  const char* wmodes[] = {"windowed", "fullscreen"};
  if (ImGui::Combo("window", &fs_mode, wmodes, 2)) {
    s.fullscreen = fs_mode == 1;
    dirty = true;
  }
  ImGui::Spacing();
  ImGui::TextDisabled("integer keeps every game pixel an exact square. fit is the largest whole");
  ImGui::TextDisabled("number that fits, fullscreen. native asks the game for the screen's own size.");
  ImGui::TextDisabled("The author chose %s%s.", g.display.c_str(), g.fullscreen ? ", fullscreen" : "");
  if (dirty) ctx_.p.save_settings(g.id, s);
}

void ControlsPage::draw() {
  const bundle::GameMeta& g = ctx_.p.game(ctx_.selected);
  PageWindow page((g.name + " - controls").c_str(), "Esc back", ctx_.w.big);
  player::GameSettings s = ctx_.p.settings(g.id);
  int preset = s.gamepad == "kretro" ? 1 : 0;
  const char* presets[] = {"the author's map", "kretro's own map"};
  if (ImGui::Combo("gamepad", &preset, presets, 2)) {
    s.gamepad = preset == 1 ? "kretro" : "author";
    ctx_.p.save_settings(g.id, s);
  }
  ImGui::Spacing();
  if (g.gamepad.empty()) {
    ImGui::TextDisabled("The author set no map of their own, so both are kretro's: sticks and the");
    ImGui::TextDisabled("d-pad move, A is Enter, B is Escape, and the rest follow the keyboard.");
  } else {
    ImGui::TextDisabled("The author's map, over kretro's own:");
    ImGui::TextWrapped("%s", g.gamepad.c_str());
  }
}

}  // namespace kg::gui::launcher
