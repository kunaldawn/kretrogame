// Settings.
//
// One file, in the state directory, read by both the terminal and the shelf so
// that a preference set in one is honoured by the other. Everything in it has a
// working default, and a file that does not exist is a new machine rather than
// an error.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace kg::config {

enum class WindowMode { Windowed, Borderless, Fullscreen };

// How a game's pixels reach the panel. Weston's nested backends scale by whole
// numbers only, so `Fit` is the largest whole number that fits shown
// fullscreen, not a smooth stretch.
enum class ScaleMode { Integer, Fit, Native };

const char* name_of(WindowMode m);
const char* name_of(ScaleMode m);
WindowMode parse_window_mode(std::string_view s);   // unknown -> Windowed
ScaleMode parse_scale_mode(std::string_view s);     // unknown -> Integer

struct Display {
  ScaleMode mode = ScaleMode::Integer;
  uint32_t scale = 0;   // 0 means the largest whole number that fits
};

struct Window {
  WindowMode mode = WindowMode::Windowed;
  uint32_t width = 1280;
  uint32_t height = 800;
  int x = -1;           // -1 means centred
  int y = -1;
  bool remember_geometry = true;
};

struct Config {
  Window window;
  Display display;
  std::vector<std::string> library_paths;
  bool scan_on_start = true;
  std::map<std::string, Display> per_game;
};

std::filesystem::path config_file();

// A missing file yields defaults. A malformed one yields defaults too, because
// refusing to start over a typo in a settings file helps nobody.
Config load(const std::filesystem::path& p);

// Written to a temporary and renamed, so an interrupted save cannot leave a
// half-written settings file behind.
void save(const std::filesystem::path& p, const Config& c);

Display for_game(const Config& c, const std::string& id);

}  // namespace kg::config
