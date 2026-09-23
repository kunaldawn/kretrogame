// Tier 1 unit tests for settings and scaling: no display, no GPU, no game.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>

#include "config/config.h"
#include "config/scaling.h"

namespace fs = std::filesystem;
using namespace kg;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++checks;                                                                \
    if (!(cond)) {                                                           \
      ++failures;                                                            \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    ++checks;                                                                            \
    auto va_ = (a);                                                                      \
    auto vb_ = (b);                                                                      \
    if (!(va_ == vb_)) {                                                                 \
      ++failures;                                                                        \
      std::ostringstream os_;                                                            \
      os_ << va_ << " != " << vb_;                                                       \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, os_.str().c_str()); \
    }                                                                                    \
  } while (0)

#define CHECK_THROWS(expr)                                                \
  do {                                                                    \
    ++checks;                                                             \
    bool threw_ = false;                                                  \
    try {                                                                 \
      (void)(expr);                                                       \
    } catch (const std::exception&) {                                     \
      threw_ = true;                                                      \

static void test_defaults_when_missing(const fs::path& tmp) {
  // A machine with no settings file is not an error; it is a new machine.
  config::Config c = config::load(tmp / "nothing.toml");
  CHECK(c.window.mode == config::WindowMode::Windowed);
  CHECK_EQ(c.window.width, 1280u);
  CHECK_EQ(c.window.height, 800u);
  CHECK(c.display.mode == config::ScaleMode::Integer);
  CHECK_EQ(c.display.scale, 0u);            // 0 means "largest that fits"
  CHECK(c.scan_on_start);
  CHECK_EQ(c.library_paths.size(), 0u);
  CHECK_EQ(c.per_game.size(), 0u);
}

static void test_round_trip(const fs::path& tmp) {
  config::Config c;
  c.window.mode = config::WindowMode::Borderless;
  c.window.width = 1920;
  c.window.height = 1080;
  c.window.x = 40;
  c.window.y = 60;
  c.window.remember_geometry = true;
  c.display.mode = config::ScaleMode::Fit;
  c.display.scale = 3;
  c.library_paths = {"/home/me/iso", "/mnt/discs"};
  c.scan_on_start = false;
  c.per_game["example-game"] = config::Display{config::ScaleMode::Native, 0};
  c.per_game["classic1"] = config::Display{config::ScaleMode::Integer, 2};

  fs::path f = tmp / "config.toml";
  config::save(f, c);
  config::Config back = config::load(f);

  CHECK(back.window.mode == config::WindowMode::Borderless);
  CHECK_EQ(back.window.width, 1920u);
  CHECK_EQ(back.window.x, 40);
  CHECK(back.display.mode == config::ScaleMode::Fit);
  CHECK_EQ(back.display.scale, 3u);
  CHECK_EQ(back.library_paths.size(), 2u);
  CHECK_EQ(back.library_paths[1], std::string("/mnt/discs"));
  CHECK(!back.scan_on_start);
  CHECK_EQ(back.per_game.size(), 2u);
  CHECK(back.per_game.at("example-game").mode == config::ScaleMode::Native);
  CHECK_EQ(back.per_game.at("classic1").scale, 2u);
}

static void test_per_game_overrides() {
  config::Config c;
  c.display.mode = config::ScaleMode::Integer;
  c.display.scale = 2;
  c.per_game["example"] = config::Display{config::ScaleMode::Native, 0};

  // A game with an override gets it; a game without gets the global setting.
  CHECK(config::for_game(c, "example").mode == config::ScaleMode::Native);
  CHECK(config::for_game(c, "classic1").mode == config::ScaleMode::Integer);
  CHECK_EQ(config::for_game(c, "classic1").scale, 2u);
}

static void test_names_round_trip() {
  for (config::WindowMode m : {config::WindowMode::Windowed, config::WindowMode::Borderless,
                               config::WindowMode::Fullscreen}) {
    CHECK(config::parse_window_mode(config::name_of(m)) == m);
  }
  for (config::ScaleMode m : {config::ScaleMode::Integer, config::ScaleMode::Fit,
                              config::ScaleMode::Native}) {
    CHECK(config::parse_scale_mode(config::name_of(m)) == m);
  }
  // An unknown name falls back rather than throwing: a hand-edited settings
  // file with a typo should still open the program.
  CHECK(config::parse_window_mode("nonsense") == config::WindowMode::Windowed);
  CHECK(config::parse_scale_mode("nonsense") == config::ScaleMode::Integer);
}

static void test_malformed_file_yields_defaults(const fs::path& tmp) {
  fs::path f = tmp / "broken.toml";
  { std::ofstream o(f); o << "window.mode = = = \n[[[\n"; }
  config::Config c = config::load(f);
  CHECK(c.window.mode == config::WindowMode::Windowed);
  CHECK_EQ(c.window.width, 1280u);
}

static void test_largest_fitting_scale() {
  // 640x480 on a 4K panel: 3840/640 = 6, 2160/480 = 4.5 -> 4.
  CHECK_EQ(config::largest_fitting_scale(640, 480, 3840, 2160), 4u);
  // 1024x768 on 1920x1080: 1.87 and 1.40 -> 1.
  CHECK_EQ(config::largest_fitting_scale(1024, 768, 1920, 1080), 1u);
  // A game bigger than the panel still gets 1: never zero, never negative.
  CHECK_EQ(config::largest_fitting_scale(2560, 1440, 1280, 720), 1u);
  // Degenerate input must not divide by zero.
  CHECK_EQ(config::largest_fitting_scale(0, 0, 1920, 1080), 1u);
}

static void test_geometry_integer() {
  config::Display d;
  d.mode = config::ScaleMode::Integer;
  d.scale = 0;   // automatic
  config::Geometry g = config::compute_geometry(640, 480, 3840, 2160, d);
  CHECK_EQ(g.logical_w, 640u);
  CHECK_EQ(g.logical_h, 480u);
  CHECK_EQ(g.scale, 4u);       // 2560x1920 of a 3840x2160 panel
  CHECK(!g.fullscreen);
  CHECK(!g.desktop_is_panel);

  d.scale = 2;   // asked for explicitly, and honoured even though 4 would fit
  CHECK_EQ(config::compute_geometry(640, 480, 3840, 2160, d).scale, 2u);

  d.scale = 99;  // absurd, clamped to what the panel can show
  CHECK_EQ(config::compute_geometry(640, 480, 3840, 2160, d).scale, 4u);
}

static void test_geometry_fit() {
  config::Display d;
  d.mode = config::ScaleMode::Fit;
  d.scale = 0;
  config::Geometry g = config::compute_geometry(640, 480, 3840, 2160, d);
  // Fit is the largest whole number that fits, then fullscreen - Weston's
  // nested backends have no fractional output scale to ask for.
  CHECK_EQ(g.scale, 4u);
  CHECK(g.fullscreen);
  CHECK_EQ(g.logical_w, 640u);
  CHECK(!g.desktop_is_panel);
}

static void test_geometry_native() {
  config::Display d;
  d.mode = config::ScaleMode::Native;
  config::Geometry g = config::compute_geometry(640, 480, 3840, 2160, d);
  // The game is asked to render at the panel's resolution, so there is nothing
  // left to scale.
  CHECK_EQ(g.logical_w, 3840u);
  CHECK_EQ(g.logical_h, 2160u);
  CHECK_EQ(g.scale, 1u);
  CHECK(g.fullscreen);
  CHECK(g.desktop_is_panel);
}

static void test_geometry_unknown_panel() {
  // With no panel size known, nothing is assumed.
  config::Display d;
  d.mode = config::ScaleMode::Integer;
  d.scale = 0;
  config::Geometry g = config::compute_geometry(640, 480, 0, 0, d);
  CHECK_EQ(g.scale, 1u);
  CHECK(!g.fullscreen);
}

int main() {
  fs::path tmp = fs::temp_directory_path() / "kretro-test-config";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  try {
    test_defaults_when_missing(tmp);
    test_round_trip(tmp);
    test_per_game_overrides();
    test_names_round_trip();
    test_malformed_file_yields_defaults(tmp);
    test_largest_fitting_scale();
    test_geometry_integer();
    test_geometry_fit();
    test_geometry_native();
    test_geometry_unknown_panel();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  FAIL unexpected exception: %s\n", e.what());
    ++failures;
  }

  fs::remove_all(tmp);
  std::fprintf(stderr, "\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
