// Settings: the window, scaling, and the library folders.
#include <SDL.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

#include "../../config/config.h"
#include "../../config/scaling.h"
#include "../../pack/kgpack.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {
namespace fs = std::filesystem;

// No save button. Every change is written as it is made: a settings screen
// with a save button is a settings screen you can lose work in.
void SettingsPage::draw() {
  PageWindow page("Settings", "Esc back", ctx_.fonts.big);
  config::Config cfg = config::load(config::config_file());
  bool dirty = false;

  ImGui::Text("window");
  const char* wmodes[] = {"windowed", "borderless", "fullscreen"};
  int wm = static_cast<int>(cfg.window.mode);
  if (ImGui::Combo("mode", &wm, wmodes, 3)) {
    cfg.window.mode = static_cast<config::WindowMode>(wm);
    dirty = true;
  }
  if (ImGui::Checkbox("remember where the window was", &cfg.window.remember_geometry)) dirty = true;

  ImGui::Spacing();
  ImGui::Text("scaling");
  const char* smodes[] = {"integer", "fit", "native"};
  int sm = static_cast<int>(cfg.display.mode);
  if (ImGui::Combo("how", &sm, smodes, 3)) {
    cfg.display.mode = static_cast<config::ScaleMode>(sm);
    dirty = true;
  }
  int sc = static_cast<int>(cfg.display.scale);
  if (ImGui::SliderInt("factor", &sc, 0, 6, sc == 0 ? "automatic" : "%dx")) {
    cfg.display.scale = static_cast<uint32_t>(sc);
    dirty = true;
  }
  ImGui::TextDisabled(
      "integer keeps every game pixel an exact square. fit is the largest whole");
  ImGui::TextDisabled(
      "number that fits, fullscreen. native asks the game for the screen's own size.");

  // What that means, for the game you were last looking at.
  if (!ctx_.entries.empty()) {
    const Entry& en = ctx_.entries[ctx_.selected < ctx_.entries.size() ? ctx_.selected : 0];
    uint32_t pw = 0, ph = 0;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { pw = dm.w; ph = dm.h; }
    uint32_t gw = 640, gh = 480;
    std::error_code e2;
    if (fs::exists(en.pack, e2)) {
      try {
        Meta mm = Pack::open(en.pack).meta();
        if (mm.run.width) { gw = mm.run.width; gh = mm.run.height; }
      } catch (const std::exception&) {}
    }
    config::Geometry g =
        config::compute_geometry(gw, gh, pw, ph, config::for_game(cfg, en.id));
    ImGui::Spacing();
    ImGui::Text("%s would run at %ux%u at %ux -> %ux%u%s", en.name.c_str(), gw, gh, g.scale,
                g.logical_w * g.scale, g.logical_h * g.scale,
                g.fullscreen ? ", fullscreen" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton("use these for this game only")) {
      cfg.per_game[en.id] = cfg.display;
      dirty = true;
    }
    if (cfg.per_game.count(en.id)) {
      ImGui::SameLine();
      if (ImGui::SmallButton("clear its override")) {
        cfg.per_game.erase(en.id);
        dirty = true;
      }
    }
  }

  ImGui::Spacing();
  ImGui::Text("library folders");
  int remove_at = -1;
  for (size_t i = 0; i < cfg.library_paths.size(); ++i) {
    ImGui::TextDisabled("  %s", cfg.library_paths[i].c_str());
    ImGui::SameLine();
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton("remove")) remove_at = static_cast<int>(i);
    ImGui::PopID();
  }
  if (remove_at >= 0) {
    cfg.library_paths.erase(cfg.library_paths.begin() + remove_at);
    dirty = true;
  }
  ImGui::TextDisabled("  drop a folder on this window to add one");

  if (dirty) config::save(config::config_file(), cfg);
}

}  // namespace kg::gui::shelf
