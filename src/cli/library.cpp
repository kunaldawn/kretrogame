// kretro list, verify, uninstall, display and show: the games on the shelf.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "../config/config.h"
#include "../config/scaling.h"
#include "../gpu/probe.h"
#include "../gui/shelf/shelf.h"
#include "../gui/screen.h"
#include "handlers.h"
#include "../pack/kgpack.h"
#include "../rt/env.h"
#include "../util/format.h"
#include "../util/hash.h"
#include "../util/paths.h"

namespace fs = std::filesystem;

namespace kg::cli {

int cmd_list(const rt::Env& /*e*/, std::vector<std::string>& /*a*/) {
  ensure_state_dirs();
  std::error_code ec;
  int n = 0;
  std::vector<fs::path> packs;
  for (const fs::directory_entry& de : fs::directory_iterator(games_dir(), ec)) {
    if (de.path().extension() == ".kgpack") packs.push_back(de.path());
  }
  std::sort(packs.begin(), packs.end());
  if (packs.empty()) {
    std::printf("No games yet.\n\n  kretro games            what you can install\n"
                "  kretro install <id>     install one\n");
    return 0;
  }
  std::printf("%-18s %-32s %-6s %-10s %s\n", "ID", "NAME", "YEAR", "SIZE", "ROOT");
  for (const fs::path& p : packs) {
    try {
      Pack pk = Pack::open(p);
      std::printf("%-18s %-32s %-6u %-10s %s\n", pk.meta().id.c_str(), pk.meta().name.c_str(),
                  pk.meta().year, fmt::bytes_iec(fs::file_size(p, ec)).c_str(),
                  to_hex(pk.header().blake3_root).substr(0, 12).c_str());
      ++n;
    } catch (const std::exception& ex) {
      std::printf("%-18s %s\n", p.filename().c_str(), ex.what());
    }
  }
  return n ? 0 : 1;
}

int cmd_verify(const rt::Env& /*e*/, std::vector<std::string>& a) {
  const std::string& id = a[0];
  fs::path p = game_pack(id);
  std::error_code ec;
  if (!fs::exists(p, ec)) {
    std::fprintf(stderr, "kretro: %s is not installed\n", id.c_str());
    return 1;
  }
  Pack pk = Pack::open(p);
  Pack::Verification v = pk.verify();
  std::printf("%-14s %s\n", "tree root", v.root_matches ? "ok" : "MISMATCH");
  std::printf("%-14s %s\n", "body", v.body_matches ? "ok" : "CORRUPT");
  std::printf("%-14s %zu files, %s\n", "contents", pk.meta().tree.size(),
              fmt::bytes_iec(pk.meta().tree.total_bytes()).c_str());
  if (!v.ok) std::fprintf(stderr, "\n%s\n", v.detail.c_str());
  return v.ok ? 0 : 1;
}

int cmd_uninstall(const rt::Env& /*e*/, std::vector<std::string>& a) {
  const std::string& id = a[0];
  fs::path p = game_pack(id);
  std::error_code ec;
  if (!fs::exists(p, ec)) {
    std::fprintf(stderr, "kretro: %s is not installed\n", id.c_str());
    return 1;
  }
  // The game is one file, so this really is all of it. Saves are deliberately
  // somewhere else and are left alone.
  fs::remove(p, ec);
  std::printf("removed %s\n", p.c_str());
  fs::path saves = saves_dir() / id;
  if (fs::exists(saves, ec)) std::printf("saves kept at %s\n", saves.c_str());
  return 0;
}

int cmd_display(const rt::Env& /*e*/, std::vector<std::string>& a) {
  const std::string& id = a[0];
  const gui::PanelSize ps = gui::desktop_size(false);
  uint32_t pw = ps.w, ph = ps.h;
  fs::path pack = game_pack(id);
  std::error_code ec;
  if (!fs::exists(pack, ec)) {
    std::fprintf(stderr, "kretro: %s is not installed\n", id.c_str());
    return 1;
  }
  Meta m = Pack::open(pack).meta();
  config::Config cfg = config::load(config::config_file());
  config::Display d = config::for_game(cfg, id);
  config::Geometry g = config::compute_geometry(m.run.width, m.run.height, pw, ph, d);

  std::printf("%s\n", m.name.c_str());
  std::printf("  the game renders at   %ux%u\n", g.logical_w, g.logical_h);
  if (pw && ph) std::printf("  this screen is        %ux%u\n", pw, ph);
  else std::printf("  this screen is        unknown (no display)\n");
  std::printf("  scaling               %s", config::name_of(d.mode));
  if (d.mode == config::ScaleMode::Integer && d.scale) std::printf(", asked for %ux", d.scale);
  std::printf("\n");
  std::printf("  you will see          %ux%u at %ux%s\n", g.logical_w * g.scale,
              g.logical_h * g.scale, g.scale, g.fullscreen ? ", fullscreen" : "");
  return 0;
}

// Probes the GPU itself rather than asking the table to: the table's probe
// would run before the argument count is checked.
int cmd_show(const rt::Env& /*e*/, std::vector<std::string>& a) {
  gpu::Report gl2 = gpu::probe();
  gpu::materialize(gl2);
  gui::Startup entry;
  entry.game = a[0];
  return gui::run(rt::make(&gl2), entry);
}

}  // namespace kg::cli
