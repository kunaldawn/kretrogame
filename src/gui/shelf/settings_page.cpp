// Settings: the window, scaling, and the library folders.
#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

#include "../../config/config.h"
#include "../../config/scaling.h"
#include "../../pack/kgpack.h"
#include "../../util/env.h"
#include "../palette.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {
namespace fs = std::filesystem;

// No save button. Every change is written as it is made: a settings screen
// with a save button is a settings screen you can lose work in.
void SettingsPage::draw() {
  set_page_trail("kretro \xe2\x80\xba settings");
  PageWindow page("Settings", "Esc back", ctx_.fonts.big());
  // The config and the game's own size, read when the page comes up rather
  // than on every frame; what is changed here is written straight back, so
  // the copy is never behind it.
  const Entry* game = nullptr;
  if (!ctx_.entries.empty()) game = &ctx_.entries[ctx_.selected < ctx_.entries.size() ? ctx_.selected : 0];
  if (disk_.due(game ? game->pack.string() : std::string())) {
    cfg_ = config::load(config::config_file());
    // Measured as a play would measure it, and only when the page is read:
    // under a Wayland host that is another process.
    panel_ = desktop_size(false);
    game_w_ = 640;
    game_h_ = 480;
    std::error_code e2;
    if (game && fs::exists(game->pack, e2)) {
      try {
        Meta mm = Pack::open(game->pack).game(game->id);
        if (mm.run.width) {
          game_w_ = mm.run.width;
          game_h_ = mm.run.height;
        }
      } catch (const std::exception&) {}
    }
  }
  config::Config& cfg = cfg_;
  bool dirty = false;

  // One region scrolls the page; the settings are rows in a column of a
  // readable width down the middle of it, a name and its help at the left of
  // each and the control at the right.
  begin_scroll("settings", ImVec2(0, 0));
  centre_column(content_max_w(Content::Form));
  step_heading("Settings");

  nav_section_begin("window");
  section("window");
  const char* wmodes[] = {"windowed", "borderless", "fullscreen"};
  int wm = static_cast<int>(cfg.window.mode);
  setting_begin("mode");
  if (step_combo("##mode", &wm, wmodes, 3)) {
    cfg.window.mode = static_cast<config::WindowMode>(wm);
    dirty = true;
  }
  default_focus();
  setting_end();
  setting_begin("position");
  if (ImGui::Checkbox("remember where the window was", &cfg.window.remember_geometry)) dirty = true;
  setting_end();
  nav_section_end();

  nav_section_begin("scaling");
  section("scaling");
  const char* smodes[] = {"integer", "fit", "native"};
  int sm = static_cast<int>(cfg.display.mode);
  setting_begin("how",
                "integer keeps every game pixel an exact square. fit is the largest whole "
                "number that fits, fullscreen. native asks the game for the screen's own size.");
  if (step_combo("##how", &sm, smodes, 3)) {
    cfg.display.mode = static_cast<config::ScaleMode>(sm);
    dirty = true;
  }
  setting_end();
  int sc = static_cast<int>(cfg.display.scale);
  setting_begin("factor");
  if (ImGui::SliderInt("##factor", &sc, 0, 6, sc == 0 ? "automatic" : "%dx")) {
    cfg.display.scale = static_cast<uint32_t>(sc);
    dirty = true;
  }
  static const char* const steps[] = {"auto", "1x", "2x", "3x", "4x", "5x", "6x"};
  step_labels(steps, 6, sc);
  setting_end();

  // What that means, for the game you were last looking at.
  if (!ctx_.entries.empty()) {
    const Entry& en = ctx_.entries[ctx_.selected < ctx_.entries.size() ? ctx_.selected : 0];
    const uint32_t gw = game_w_, gh = game_h_;
    config::Geometry g = config::compute_geometry(
        gw, gh,
        {panel_.w, panel_.h, panel_.usable_w, panel_.usable_h,
         config::weston_window_frame(env_nonempty("WAYLAND_DISPLAY"))},
        config::for_game(cfg, en.id));
    char would[160];
    std::snprintf(would, sizeof would, "would run at %ux%u at %ux -> %ux%u%s", gw, gh, g.scale,
                  g.logical_w * g.scale, g.logical_h * g.scale, g.fullscreen ? ", fullscreen" : "");
    setting_begin(en.name.c_str(), would);
    if (ghost_button("use these for this game only")) {
      cfg.per_game[en.id] = cfg.display;
      dirty = true;
    }
    if (cfg.per_game.count(en.id)) {
      same_line_if_fits("clear its override");
      if (ghost_button("clear its override")) {
        cfg.per_game.erase(en.id);
        dirty = true;
      }
    }
    setting_end();
  }
  nav_section_end();

  nav_section_begin("folders");
  section("library folders");
  int remove_at = -1;
  for (size_t i = 0; i < cfg.library_paths.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    setting_begin(cfg.library_paths[i].c_str(), nullptr, kCyan);
    if (ghost_button("remove")) remove_at = static_cast<int>(i);
    setting_end();
    ImGui::PopID();
  }
  if (remove_at >= 0) {
    cfg.library_paths.erase(cfg.library_paths.begin() + remove_at);
    dirty = true;
  }
  vgap(4);
  ImGui::TextDisabled("drop a folder on this window to add one");
  nav_section_end();
  end_centre_column();
  end_scroll();

  if (dirty) config::save(config::config_file(), cfg);
}

}  // namespace kg::gui::shelf
