#include "config.h"

#include <fstream>
#include <sstream>

#include "../util/paths.h"
#include "../util/toml.h"

namespace fs = std::filesystem;

namespace kg::config {

const char* name_of(WindowMode m) {
  switch (m) {
    case WindowMode::Borderless: return "borderless";
    case WindowMode::Fullscreen: return "fullscreen";
    case WindowMode::Windowed: return "windowed";
  }
  return "windowed";
}

const char* name_of(ScaleMode m) {
  switch (m) {
    case ScaleMode::Fit: return "fit";
    case ScaleMode::Native: return "native";
    case ScaleMode::Integer: return "integer";
  }
  return "integer";
}

WindowMode parse_window_mode(std::string_view s) {
  if (s == "borderless") return WindowMode::Borderless;
  if (s == "fullscreen") return WindowMode::Fullscreen;
  return WindowMode::Windowed;
}

ScaleMode parse_scale_mode(std::string_view s) {
  if (s == "fit") return ScaleMode::Fit;
  if (s == "native") return ScaleMode::Native;
  return ScaleMode::Integer;
}

fs::path config_file() { return state_dir() / "config.toml"; }

Config load(const fs::path& p) {
  Config c;
  std::error_code ec;
  if (!fs::exists(p, ec)) return c;
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  Toml t;
  try {
    t = Toml::parse(ss.str());
  } catch (const std::exception&) {
    return c;  // a typo in a settings file must not stop the program starting
  }

  c.window.mode = parse_window_mode(t.str("window.mode", "windowed"));
  c.window.width = static_cast<uint32_t>(t.integer("window.width", 1280));
  c.window.height = static_cast<uint32_t>(t.integer("window.height", 800));
  c.window.x = static_cast<int>(t.integer("window.x", -1));
  c.window.y = static_cast<int>(t.integer("window.y", -1));
  c.window.remember_geometry = t.boolean("window.remember_geometry", true);

  c.display.mode = parse_scale_mode(t.str("display.scale_mode", "integer"));
  c.display.scale = static_cast<uint32_t>(t.integer("display.scale", 0));

  c.library_paths = t.array("library.paths");
  c.scan_on_start = t.boolean("library.scan_on_start", true);

  // Per-game overrides live under [games.<id>]. Toml has no table iteration, so
  // the ids are listed in games.ids - written by save(), and harmless to a
  // person editing the file as long as they keep the list in step.
  for (const std::string& id : t.array("games.ids")) {
    Display d;
    d.mode = parse_scale_mode(t.str("games." + id + ".scale_mode", name_of(c.display.mode)));
    d.scale = static_cast<uint32_t>(t.integer("games." + id + ".scale", c.display.scale));
    c.per_game[id] = d;
  }
  return c;
}

void save(const fs::path& p, const Config& c) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += ".new";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << "# kretro settings. Edited here or in the Settings screen; both read the\n"
      << "# same file, so a preference set in one is honoured by the other.\n\n";
    f << "[window]\n"
      << "mode              = \"" << name_of(c.window.mode)
      << "\"   # windowed | borderless | fullscreen\n"
      << "width             = " << c.window.width << "\n"
      << "height            = " << c.window.height << "\n"
      << "x                 = " << c.window.x << "\n"
      << "y                 = " << c.window.y << "\n"
      << "remember_geometry = " << (c.window.remember_geometry ? "true" : "false") << "\n\n";
    f << "[display]\n"
      << "# integer: whole-number scaling, every game pixel an exact square.\n"
      << "# fit:     the largest whole number that fits, fullscreen.\n"
      << "# native:  the game renders at the panel's own resolution, if it can.\n"
      << "scale_mode = \"" << name_of(c.display.mode) << "\"\n"
      << "scale      = " << c.display.scale << "   # 0 = the largest that fits\n\n";
    f << "[library]\npaths         = [";
    for (size_t i = 0; i < c.library_paths.size(); ++i) {
      f << (i ? ", " : "") << "\"" << c.library_paths[i] << "\"";
    }
    f << "]\nscan_on_start = " << (c.scan_on_start ? "true" : "false") << "\n\n";

    f << "[games]\nids = [";
    bool first = true;
    for (const auto& kv : c.per_game) {
      f << (first ? "" : ", ") << "\"" << kv.first << "\"";
      first = false;
    }
    f << "]\n";
    for (const auto& kv : c.per_game) {
      f << "\n[games." << kv.first << "]\n"
        << "scale_mode = \"" << name_of(kv.second.mode) << "\"\n"
        << "scale      = " << kv.second.scale << "\n";
    }
  }
  fs::rename(tmp, p, ec);
}

Display for_game(const Config& c, const std::string& id) {
  auto it = c.per_game.find(id);
  return it == c.per_game.end() ? c.display : it->second;
}

}  // namespace kg::config
