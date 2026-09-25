// Tier 1 unit tests for settings and scaling: no display, no GPU, no game.
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>

#include "config/config.h"
#include "config/scaling.h"
#include "support/check.h"
#include "util/format.h"
#include "util/fs_ci.h"
#include "util/text.h"

namespace fs = std::filesystem;
using namespace kg;

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

// This host's panel under a Wayland desktop at 125%: 3072x1728 logical, and a
// work area of 3017x1696 once the top bar and a fixed dock are taken off.
static void test_geometry_work_area() {
  const config::Panel panel{3072, 1728, 3017, 1696};
  config::Display d;
  d.mode = config::ScaleMode::Integer;
  d.scale = 0;
  // 1024x768 fits twice in the work area, and 2048x1536 is inside it.
  config::Geometry g = config::compute_geometry(1024, 768, panel, d);
  CHECK_EQ(g.scale, 2u);
  CHECK(!g.fullscreen);

  // 640x480: the whole panel would take 3 (1728/480 = 3.6), the work area
  // too (1696/480 = 3.53); a panel whose work area is short of 1440 lines
  // takes it down to 2.
  CHECK_EQ(config::compute_geometry(640, 480, panel, d).scale, 3u);
  CHECK_EQ(config::compute_geometry(640, 480, config::Panel{1920, 1440, 1920, 1400}, d).scale, 2u);
  CHECK_EQ(config::compute_geometry(640, 480, 1920, 1440, d).scale, 3u);

  // An explicit 4 is clamped to what the work area shows, not the panel.
  d.scale = 4;
  CHECK_EQ(config::compute_geometry(1024, 768, panel, d).scale, 2u);
  d.scale = 1;
  CHECK_EQ(config::compute_geometry(1024, 768, panel, d).scale, 1u);

  // A work area of zero is the whole panel.
  d.scale = 0;
  CHECK_EQ(config::compute_geometry(640, 480, config::Panel{1920, 1440, 0, 0}, d).scale, 3u);

  // Fullscreen has the whole panel: Fit and Native ignore the work area.
  d.mode = config::ScaleMode::Fit;
  g = config::compute_geometry(640, 480, config::Panel{1920, 1440, 1920, 1400}, d);
  CHECK_EQ(g.scale, 3u);
  CHECK(g.fullscreen);
  d.mode = config::ScaleMode::Native;
  g = config::compute_geometry(1024, 768, panel, d);
  CHECK_EQ(g.logical_w, 3072u);
  CHECK_EQ(g.logical_h, 1728u);
  CHECK(g.fullscreen && g.desktop_is_panel);

  // The panel as X saw it under native Xwayland scaling, which gave 4x and a
  // window twice the size of the screen. Kept to show what the units were.
  d.mode = config::ScaleMode::Integer;
  CHECK_EQ(config::compute_geometry(1024, 768, 6144, 3456, d).scale, 4u);
}

// The exact text config::save writes, pinned so that moving the code that
// writes it can be shown to change nothing in a settings file already on disk.
static void test_golden_save(const fs::path& tmp) {
  config::Config c;
  c.window.mode = config::WindowMode::Borderless;
  c.window.width = 1920;
  c.window.height = 1080;
  c.window.x = 40;
  c.window.y = -1;
  c.window.remember_geometry = false;
  c.display.mode = config::ScaleMode::Fit;
  c.display.scale = 3;
  c.library_paths = {"/home/me/iso", "/mnt/discs"};
  c.scan_on_start = false;
  c.per_game["example-game"] = config::Display{config::ScaleMode::Native, 0};
  c.per_game["example-game-2"] = config::Display{config::ScaleMode::Integer, 2};

  fs::path f = tmp / "golden" / "config.toml";
  config::save(f, c);
  std::ifstream in(f, std::ios::binary);
  const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string want =
      "# kretro settings. Edited here or in the Settings screen; both read the\n"
      "# same file, so a preference set in one is honoured by the other.\n"
      "\n"
      "[window]\n"
      "mode              = \"borderless\"   # windowed | borderless | fullscreen\n"
      "width             = 1920\n"
      "height            = 1080\n"
      "x                 = 40\n"
      "y                 = -1\n"
      "remember_geometry = false\n"
      "\n"
      "[display]\n"
      "# integer: whole-number scaling, every game pixel an exact square.\n"
      "# fit:     the largest whole number that fits, fullscreen.\n"
      "# native:  the game renders at the panel's own resolution, if it can.\n"
      "scale_mode = \"fit\"\n"
      "scale      = 3   # 0 = the largest that fits\n"
      "\n"
      "[library]\n"
      "paths         = [\"/home/me/iso\", \"/mnt/discs\"]\n"
      "scan_on_start = false\n"
      "\n"
      "[games]\n"
      "ids = [\"example-game\", \"example-game-2\"]\n"
      "\n"
      "[games.example-game]\n"
      "scale_mode = \"native\"\n"
      "scale      = 0\n"
      "\n"
      "[games.example-game-2]\n"
      "scale_mode = \"integer\"\n"
      "scale      = 2\n";
  CHECK_EQ(got, want);
}

static void test_text_helpers() {
  // The string helpers the rest of the code shares rather than copies.
  CHECK_EQ(to_lower("MiXeD Case-09.EXE"), std::string("mixed case-09.exe"));
  CHECK_EQ(to_upper("MiXeD Case-09.exe"), std::string("MIXED CASE-09.EXE"));
  CHECK_EQ(trim(" \t x \r\n"), std::string("x"));
  CHECK_EQ(trim(""), std::string(""));
  CHECK(iequals("Game.EXE", "game.exe"));
  CHECK(!iequals("game.exe", "game.ex"));
  CHECK(!iequals("game.exe", "gama.exe"));
}

static void test_formatters() {
  // Each variant exactly as the programs have always printed it: the byte
  // sizes differ on purpose in base and unit spelling, and the two durations
  // in whether minutes carry their seconds.
  CHECK_EQ(fmt::bytes_iec(0), std::string("0 B"));
  CHECK_EQ(fmt::bytes_iec(1023), std::string("1023 B"));
  CHECK_EQ(fmt::bytes_iec(1024), std::string("1.0 KiB"));
  CHECK_EQ(fmt::bytes_iec(999), std::string("999 B"));
  CHECK_EQ(fmt::bytes_iec(1000), std::string("1000 B"));
  CHECK_EQ(fmt::bytes_iec(1536), std::string("1.5 KiB"));
  CHECK_EQ(fmt::bytes_iec(5000000000ull), std::string("4.7 GiB"));

  CHECK_EQ(fmt::bytes_compact(0), std::string("0 B"));
  CHECK_EQ(fmt::bytes_compact(1023), std::string("1023 B"));
  CHECK_EQ(fmt::bytes_compact(1024), std::string("1 KB"));
  CHECK_EQ(fmt::bytes_compact(999), std::string("999 B"));
  CHECK_EQ(fmt::bytes_compact(1000), std::string("1000 B"));
  CHECK_EQ(fmt::bytes_compact(1536), std::string("2 KB"));
  CHECK_EQ(fmt::bytes_compact(5000000000ull), std::string("4.7 GB"));

  CHECK_EQ(fmt::bytes_si(0), std::string("0 B"));
  CHECK_EQ(fmt::bytes_si(1023), std::string("1.0 KB"));
  CHECK_EQ(fmt::bytes_si(1024), std::string("1.0 KB"));
  CHECK_EQ(fmt::bytes_si(999), std::string("999 B"));
  CHECK_EQ(fmt::bytes_si(1000), std::string("1.0 KB"));
  CHECK_EQ(fmt::bytes_si(1536), std::string("1.5 KB"));
  CHECK_EQ(fmt::bytes_si(5000000000ull), std::string("5.0 GB"));

  CHECK_EQ(fmt::gigabytes(0), std::string("0.0 GB"));
  CHECK_EQ(fmt::gigabytes(3500000000ull), std::string("3.5 GB"));

  CHECK_EQ(fmt::duration(0), std::string("0s"));
  CHECK_EQ(fmt::duration(59), std::string("59s"));
  CHECK_EQ(fmt::duration(60), std::string("1m 0s"));
  CHECK_EQ(fmt::duration(61), std::string("1m 1s"));
  CHECK_EQ(fmt::duration(3599), std::string("59m 59s"));
  CHECK_EQ(fmt::duration(3600), std::string("1h 0m"));
  CHECK_EQ(fmt::duration(3661), std::string("1h 1m"));

  CHECK_EQ(fmt::duration_short(0), std::string("0s"));
  CHECK_EQ(fmt::duration_short(59), std::string("59s"));
  CHECK_EQ(fmt::duration_short(60), std::string("1m"));
  CHECK_EQ(fmt::duration_short(61), std::string("1m"));
  CHECK_EQ(fmt::duration_short(3599), std::string("59m"));
  CHECK_EQ(fmt::duration_short(3600), std::string("1h 0m"));
  CHECK_EQ(fmt::duration_short(3661), std::string("1h 1m"));

  // kretro's journal says "never"; the shelf says "never played".
  CHECK_EQ(fmt::ago(0, "never"), std::string("never"));
  CHECK_EQ(fmt::ago(0, "never played"), std::string("never played"));

  const std::time_t t = 1700000000;
  std::tm tm{};
  localtime_r(&t, &tm);
  char want[32] = {};
  std::strftime(want, sizeof(want), "%Y-%m-%d %H:%M", &tm);
  CHECK_EQ(fmt::local_minute(t), std::string(want));
}

static void test_case_insensitive_paths(const fs::path& tmp) {
  // A manifest's spelling against a tree's: every component matched ignoring
  // case, and the path that comes back is the tree's own.
  fs::path root = tmp / "ci";
  fs::create_directories(root / "Data" / "SubDir");
  std::ofstream(root / "Data" / "SubDir" / "Game.EXE") << "x";
  CHECK_EQ(resolve_ci(root, "data/subdir/game.exe"), root / "Data" / "SubDir" / "Game.EXE");
  CHECK_EQ(resolve_ci(root, "DATA\\SUBDIR"), root / "Data" / "SubDir");
  CHECK(resolve_ci(root, "data/missing.exe").empty());
  CHECK(exists_ci(root, "DATA/subdir/GAME.exe"));
  CHECK(!exists_ci(root, "data/subdir/other.exe"));
}

int main() {
  fs::path tmp = fs::temp_directory_path() / "kretro-test-config";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  try {
    test_defaults_when_missing(tmp);
    test_round_trip(tmp);
    test_golden_save(tmp);
    test_per_game_overrides();
    test_names_round_trip();
    test_malformed_file_yields_defaults(tmp);
    test_largest_fitting_scale();
    test_geometry_integer();
    test_geometry_fit();
    test_geometry_native();
    test_geometry_unknown_panel();
    test_geometry_work_area();
    test_text_helpers();
    test_formatters();
    test_case_insensitive_paths(tmp);
  } catch (const std::exception& e) {
    kgtest::unexpected(e);
  }

  fs::remove_all(tmp);
  return kgtest::finish();
}
