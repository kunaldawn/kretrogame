// One game's display and controls, each kept in its settings.toml as soon as
// it changes.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {

void spaced_text(const std::string& text, const ImVec4& colour, float max_w) {
  ImFont* f = ImGui::GetFont();
  const float scale = ImGui::GetFontSize() / f->FontSize;
  const float wrap = std::min(ImGui::GetContentRegionAvail().x, max_w);
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, std::round(px(4))));
  const char* at = text.c_str();
  const char* end = at + text.size();
  while (at < end) {
    const char* cut = f->CalcWordWrapPositionA(scale, at, end, wrap);
    if (cut <= at) cut = at + 1;
    ImGui::TextUnformatted(at, cut);
    at = cut;
    while (at < end && (*at == ' ' || *at == '\n')) ++at;
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

void set_trail(const LauncherContext& ctx, const std::string& game, const char* leaf) {
  const char* sep = " \xe2\x80\xba ";
  std::string trail = ctx.p.bundle().meta.title;
  // A one-game player's bundle is its game, so the game is not said twice.
  if (!game.empty() && ctx.ids.size() > 1) trail += sep + game;
  set_page_trail(trail + sep + leaf);
}

void DisplayPage::draw() {
  const bundle::GameMeta& g = ctx_.p.game(ctx_.selected);
  set_chrome_path("~/" + path_part(g.name) + "/display");
  set_trail(ctx_, g.name, "display");
  PageWindow page((g.name + " - display").c_str(), "Esc back", ctx_.w.fonts().big());
  player::GameSettings s = ctx_.p.settings(g.id);
  bool dirty = false;
  // Rows in a column of a readable width down the middle of the page, each
  // a name at the left and its control at the right, as a console's
  // settings are.
  begin_scroll("rows", ImVec2(0, 0));
  centre_column(content_max_w(Content::Form));
  step_heading("Display");
  section("window and scale");
  const char* modes[] = {"integer", "fit", "native"};
  int m = static_cast<int>(s.display.mode);
  setting_begin("scaling",
                "Integer keeps every game pixel an exact square. Fit is the largest whole "
                "number that fits, fullscreen. Native asks the game for the screen's own size.");
  if (step_combo("##scaling", &m, modes, 3)) {
    s.display.mode = static_cast<config::ScaleMode>(m);
    dirty = true;
  }
  // The page opens on its first setting.
  default_focus();
  setting_end();
  int sc = static_cast<int>(s.display.scale);
  setting_begin("scale");
  if (ImGui::SliderInt("##scale", &sc, 0, 6, sc == 0 ? "the largest that fits" : "%dx")) {
    s.display.scale = static_cast<uint32_t>(sc);
    dirty = true;
  }
  static const char* const steps[] = {"auto", "1x", "2x", "3x", "4x", "5x", "6x"};
  step_labels(steps, 6, sc);
  setting_end();
  int fs_mode = s.fullscreen ? 1 : 0;
  const char* wmodes[] = {"windowed", "fullscreen"};
  setting_begin("window");
  if (step_combo("##window", &fs_mode, wmodes, 2)) {
    s.fullscreen = fs_mode == 1;
    dirty = true;
  }
  setting_end();
  vgap(8);
  badge(BadgeKind::Info);
  ImGui::SameLine(0, badge_gap());
  ImGui::TextDisabled("The author chose %s%s.", g.display.c_str(), g.fullscreen ? ", fullscreen" : "");
  end_centre_column();
  end_scroll();
  if (dirty) ctx_.p.save_settings(g.id, s);
}

void ControlsPage::draw() {
  const bundle::GameMeta& g = ctx_.p.game(ctx_.selected);
  set_chrome_path("~/" + path_part(g.name) + "/controls");
  set_trail(ctx_, g.name, "controls");
  PageWindow page((g.name + " - controls").c_str(), "Esc back", ctx_.w.fonts().big());
  player::GameSettings s = ctx_.p.settings(g.id);
  begin_scroll("rows", ImVec2(0, 0));
  centre_column(content_max_w(Content::Form));
  step_heading("Controls");
  section("button map");
  int preset = s.gamepad == "kretro" ? 1 : 0;
  const char* presets[] = {"the author's map", "kretro's own map"};
  setting_begin("gamepad", g.gamepad.empty() ? "The author set no map of their own, so both are kretro's."
                                             : "The author's map, over kretro's own.");
  if (step_combo("##gamepad", &preset, presets, 2)) {
    s.gamepad = preset == 1 ? "kretro" : "author";
    ctx_.p.save_settings(g.id, s);
  }
  default_focus();
  setting_end();
  vgap(8);
  if (g.gamepad.empty()) {
    // The same table as an author's map below, so the page shows what the
    // buttons do whichever map is in use.
    if (kv_begin("map", ImGui::CalcTextSize("sticks, d-pad").x + std::round(px(16)))) {
      kv("sticks, d-pad", "move", kCyan);
      kv("A", "Enter", kCyan);
      kv("B", "Escape", kCyan);
      kv("the rest", "follow the keyboard", kCyan);
      kv_end();
    }
  } else {
    // The map is written "button=key", one a line: shown as a table of the
    // two, and a line of any other shape as it is.
    if (kv_begin("map")) {
      size_t at = 0;
      while (at < g.gamepad.size()) {
        size_t nl = g.gamepad.find('\n', at);
        if (nl == std::string::npos) nl = g.gamepad.size();
        const std::string line = g.gamepad.substr(at, nl - at);
        at = nl + 1;
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) kv("", line);
        else kv(line.substr(0, eq).c_str(), line.substr(eq + 1), kCyan);
      }
      kv_end();
    }
  }
  end_centre_column();
  end_scroll();
}

}  // namespace kg::gui::launcher
