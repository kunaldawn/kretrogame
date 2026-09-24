#include "manifest.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <system_error>

#include "../util/env.h"
#include "../util/paths.h"
#include "../util/toml.h"

namespace kg::install {
namespace fs = std::filesystem;

std::vector<fs::path> manifest_dirs(const rt::Env& e) {
  std::vector<fs::path> dirs;
  if (const char* m = env_nonempty("KRETRO_MANIFESTS")) dirs.push_back(m);
  if (e.valid()) dirs.push_back(e.root / "usr/share/kretro/games");
  dirs.push_back(state_dir() / "manifests");
  dirs.push_back("games");  // running from a checkout
  return dirs;
}

fs::path find_manifest(const rt::Env& e, const std::string& id) {
  std::error_code ec;
  for (const fs::path& d : manifest_dirs(e)) {
    fs::path p = d / (id + ".toml");
    if (fs::exists(p, ec)) return p;
  }
  return {};
}

std::vector<std::string> known_games(const rt::Env& e) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const fs::path& d : manifest_dirs(e)) {
    if (!fs::exists(d, ec)) continue;
    for (const fs::directory_entry& de : fs::directory_iterator(d, ec)) {
      if (de.path().extension() == ".toml") {
        std::string id = de.path().stem().string();
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

Meta load_manifest(const fs::path& p) {
  Toml t = Toml::parse_file(p);
  Meta m;
  m.id = t.str("id", p.stem().string());
  m.name = t.str("name", m.id);
  m.year = static_cast<uint32_t>(t.integer("year"));
  m.developer = t.str("developer");
  m.publisher = t.str("publisher");

  m.recipe.method = t.str("source.method");
  m.recipe.member = t.str("source.member");
  m.recipe.subdir = t.str("source.subdir");
  m.recipe.setup = t.str("source.setup");
  m.recipe.setup_ref = t.str("source.setup_ref");
  m.recipe.verify = t.array("source.verify");
  m.recipe.discs = t.array("source.discs");
  if (m.recipe.discs.empty() && t.has("source.iso")) {
    m.recipe.discs.push_back(t.str("source.iso"));
  }

  m.run.exe = t.str("run.exe");
  {
    std::vector<std::string> args = t.array("run.args");
    for (size_t i = 0; i < args.size(); ++i) m.run.args += (i ? " " : "") + args[i];
    // Manifests usually write this as a plain string - args = "-game mod
    // -window" - and reading it only as an array would throw all of them
    // away, silently. Both forms are accepted.
    if (m.run.args.empty()) m.run.args = t.str("run.args");
  }
  m.run.width = static_cast<uint32_t>(t.integer("run.width", 640));
  m.run.height = static_cast<uint32_t>(t.integer("run.height", 480));
  m.run.windows_version = t.str("run.windows_version", "win98");

  m.present.dar = t.str("run.dar", "4:3");
  m.present.pause_on_blur = t.boolean("run.pause_on_blur", true);

  m.runtime.dgvoodoo = t.boolean("wine.dgvoodoo", false);

  // Per-game gamepad bindings. Toml has no table iteration, so a manifest lists
  // the buttons it rebinds, the way config.toml lists the games it overrides:
  //   [input]
  //   buttons = ["a", "start"]
  //   a       = "Return"
  //   start   = "F1"
  for (const std::string& b : t.array("input.buttons")) {
    std::string v = t.str("input." + b);
    if (!v.empty()) m.input[b] = v;
  }
  m.runtime.dlloverrides = t.str("wine.dlloverrides");
  m.runtime.winetricks = t.array("wine.winetricks");

  if (m.recipe.method.empty()) throw std::runtime_error(p.string() + ": source.method is required");
  if (m.run.exe.empty()) throw std::runtime_error(p.string() + ": run.exe is required");
  return m;
}

}  // namespace kg::install
