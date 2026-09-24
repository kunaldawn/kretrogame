#include "settings.h"

#include <sstream>

#include "../bundle/gamepad.h"
#include "../util/file_io.h"
#include "../util/hash.h"
#include "../util/toml.h"

namespace kg::player {
namespace fs = std::filesystem;

namespace {

// A settings file that does not parse is a new machine rather than an error:
// the player must start whatever state a crash or a hand edit left it in.
Toml read_or_empty(const fs::path& file) {
  std::error_code ec;
  if (!fs::exists(file, ec)) return Toml::parse("");
  try {
    return Toml::parse_file(file);
  } catch (const std::exception&) {
    return Toml::parse("");
  }
}

uint32_t clamp_u32(int64_t v, uint32_t lo, uint32_t hi, uint32_t def) {
  if (v < lo || v > hi) return def;
  return static_cast<uint32_t>(v);
}

}  // namespace

std::string warning_key(const std::string& line) { return to_hex(hash_string(line)).substr(0, 16); }

LauncherState load_launcher(const fs::path& file) {
  LauncherState s;
  Toml t = read_or_empty(file);
  s.window_w = clamp_u32(t.integer("window.width", s.window_w), 320, 16384, s.window_w);
  s.window_h = clamp_u32(t.integer("window.height", s.window_h), 240, 16384, s.window_h);
  s.window_fullscreen = t.boolean("window.fullscreen", false);
  s.desktop_offered = t.boolean("desktop.offered", false);
  s.desktop_installed = t.boolean("desktop.installed", false);
  s.portable_fallback_said = t.boolean("notes.portable_fallback_said", false);
  for (const std::string& w : t.array("notes.warnings_seen")) s.warnings_seen.insert(w);
  return s;
}

void save_launcher(const fs::path& file, const LauncherState& s) {
  std::ostringstream o;
  o << "# The launcher's memory. Safe to delete: it only means being asked again.\n\n"
    << "[window]\n"
    << "width = " << s.window_w << "\n"
    << "height = " << s.window_h << "\n"
    << "fullscreen = " << (s.window_fullscreen ? "true" : "false") << "\n\n"
    << "[desktop]\n"
    << "offered = " << (s.desktop_offered ? "true" : "false") << "\n"
    << "installed = " << (s.desktop_installed ? "true" : "false") << "\n\n"
    << "[notes]\n"
    << "portable_fallback_said = " << (s.portable_fallback_said ? "true" : "false") << "\n"
    << "warnings_seen = [";
  bool first = true;
  for (const std::string& w : s.warnings_seen) {
    o << (first ? "" : ", ") << toml_string(w);
    first = false;
  }
  o << "]\n";
  write_atomically(file, o.str());
}

GameSettings default_settings(const std::string& author_display, bool author_fullscreen) {
  GameSettings g;
  g.display.mode = config::parse_scale_mode(author_display);
  g.fullscreen = author_fullscreen;
  return g;
}

GameSettings load_game_settings(const fs::path& file, const GameSettings& defaults) {
  GameSettings g = defaults;
  Toml t = read_or_empty(file);
  if (t.has("display.mode")) g.display.mode = config::parse_scale_mode(t.str("display.mode"));
  if (t.has("display.scale")) g.display.scale = clamp_u32(t.integer("display.scale"), 0, 16, 0);
  if (t.has("display.fullscreen")) g.fullscreen = t.boolean("display.fullscreen");
  if (t.has("controls.gamepad")) {
    std::string p = t.str("controls.gamepad");
    if (p == "author" || p == "kretro") g.gamepad = p;
  }
  return g;
}

void save_game_settings(const fs::path& file, const GameSettings& s) {
  std::ostringstream o;
  o << "[display]\n"
    << "mode = " << toml_string(config::name_of(s.display.mode)) << "\n"
    << "scale = " << s.display.scale << "\n"
    << "fullscreen = " << (s.fullscreen ? "true" : "false") << "\n\n"
    << "[controls]\n"
    << "gamepad = " << toml_string(s.gamepad) << "\n";
  write_atomically(file, o.str());
}

std::map<std::string, std::string> gamepad_bindings(const std::map<std::string, std::string>& pack_input,
                                                    const bundle::GameMeta& g, const GameSettings& s) {
  std::map<std::string, std::string> binds = pack_input;
  if (s.gamepad == "author") {
    for (const auto& [k, v] : bundle::parse_gamepad(g.gamepad)) binds[k] = v;
  }
  return binds;
}

std::vector<std::string> unseen_warnings(const doctor::Report& rep, LauncherState& seen) {
  std::vector<std::string> out;
  for (const gpu::Problem& pr : rep.problems) {
    if (pr.blocking()) continue;
    if (seen.warnings_seen.insert(warning_key(pr.line())).second) out.push_back(pr.line());
  }
  return out;
}

}  // namespace kg::player
