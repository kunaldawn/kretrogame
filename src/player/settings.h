// The few small files a player keeps in its state: launcher.toml, what the
// launcher remembers, and each game's settings.toml.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../bundle/meta.h"
#include "../config/config.h"
#include "doctor.h"

namespace kg::player {

// launcher.toml: what the launcher remembers between runs.
struct LauncherState {
  uint32_t window_w = 1280, window_h = 800;
  bool window_fullscreen = false;
  bool desktop_offered = false;     // "Add to applications menu" asked once
  bool desktop_installed = false;
  bool portable_fallback_said = false;
  // warning_key of each warning already shown. A hash rather than the text,
  // because the text is a stranger's machine talking - paths, device names -
  // and this file is read back by a TOML reader that is not a general one.
  std::set<std::string> warnings_seen;
};

std::string warning_key(const std::string& line);

LauncherState load_launcher(const std::filesystem::path& file);
// Written to a temporary and renamed, so a crash cannot leave half of it.
void save_launcher(const std::filesystem::path& file, const LauncherState& s);

// <state>/<game>/settings.toml: how one game is shown and played. Absent
// fields come from the author's defaults in bundle.meta.
struct GameSettings {
  config::Display display;
  bool fullscreen = false;
  // "author" is the map the bundle's author set, over kretro's own; "kretro"
  // is kretro's own alone.
  std::string gamepad = "author";
};

GameSettings default_settings(const std::string& author_display, bool author_fullscreen);
GameSettings load_game_settings(const std::filesystem::path& file, const GameSettings& defaults);
void save_game_settings(const std::filesystem::path& file, const GameSettings& s);

// The gamepad bindings the helper gives a game: the pack's own [input], with
// the author's map from bundle.meta - bundle::parse_gamepad of what the
// Bundles page wrote - over it when the game's Controls say "author".
// "kretro" is the pack's own alone. Buttons neither names keep the helper's
// defaults.
std::map<std::string, std::string> gamepad_bindings(const std::map<std::string, std::string>& pack_input,
                                                    const bundle::GameMeta& g, const GameSettings& s);

// The warnings of the silent check not yet shown on this machine, each marked
// as shown in `seen`, which the caller saves. Once each, ever, wherever it is
// said - the launcher's window or a terminal's `play` - since a warning true
// of a machine today is true of it tomorrow, and said at every start it
// teaches a person to look past what the player says.
std::vector<std::string> unseen_warnings(const doctor::Report& rep, LauncherState& seen);

}  // namespace kg::player
