// kretro list, verify, display and show: the games on the shelf.
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
#include "../install/share.h"
#include "../pack/kgpack.h"
#include "../rt/env.h"
#include "../util/env.h"
#include "../util/format.h"
#include "../util/hash.h"
#include "../util/paths.h"

namespace fs = std::filesystem;

namespace kg::cli {

int cmd_list(const rt::Env& /*e*/, std::vector<std::string>& /*a*/) {
  ensure_state_dirs();
  std::error_code ec;
  int n = 0;
  const std::vector<install::InstalledGame> games = install::installed_games();
  if (games.empty()) {
    std::printf("No games yet.\n\n  kretro games            what you can install\n"
                "  kretro install <id>     install one\n");
    return 0;
  }
  // SIZE is the set's: games from one disc share one pack.
  std::printf("%-18s %-32s %-6s %-20s %s\n", "ID", "NAME", "YEAR", "SET", "SIZE");
  for (const install::InstalledGame& g : games) {
    try {
      Pack pk = Pack::open(g.pack);
      const Meta& m = pk.game(g.id);
      std::printf("%-18s %-32s %-6u %-20s %s\n", m.id.c_str(), m.name.c_str(), m.year,
                  pk.set().set_id.c_str(), fmt::bytes_iec(fs::file_size(g.pack, ec)).c_str());
      ++n;
    } catch (const std::exception& ex) {
      std::printf("%-18s %s\n", g.id.c_str(), ex.what());
    }
  }
  return n ? 0 : 1;
}

int cmd_verify(const rt::Env& /*e*/, std::vector<std::string>& a) {
  const std::string& id = a[0];
  fs::path p = game_pack(id);
  std::error_code ec;
  if (p.empty() || !fs::exists(p, ec)) {
    std::fprintf(stderr, "kretro: %s is not installed\n", id.c_str());
    return 1;
  }
  Pack pk = Pack::open(p);
  const Meta& m = pk.game(id);
  // The whole set is checked: its body is one image, whichever game asked.
  Pack::Verification v = pk.verify();
  std::printf("%-14s %s\n", "set", pk.set().set_id.c_str());
  std::printf("%-14s %s\n", "tree root", v.root_matches ? "ok" : "MISMATCH");
  std::printf("%-14s %s\n", "body", v.body_matches ? "ok" : "CORRUPT");
  std::printf("%-14s %zu files, %s\n", "contents", m.tree.size(), fmt::bytes_iec(m.tree.total_bytes()).c_str());
  if (!v.ok) std::fprintf(stderr, "\n%s\n", v.detail.c_str());
  return v.ok ? 0 : 1;
}

int cmd_display(const rt::Env& /*e*/, std::vector<std::string>& a) {
  const std::string& id = a[0];
  const gui::PanelSize ps = gui::desktop_size(false);
  const uint32_t pw = ps.w, ph = ps.h;
  fs::path pack = game_pack(id);
  std::error_code ec;
  if (pack.empty() || !fs::exists(pack, ec)) {
    std::fprintf(stderr, "kretro: %s is not installed\n", id.c_str());
    return 1;
  }
  Meta m = Pack::open(pack).game(id);
  config::Config cfg = config::load(config::config_file());
  config::Display d = config::for_game(cfg, id);
  config::Geometry g =
      config::compute_geometry(m.run.width, m.run.height,
                               {pw, ph, ps.usable_w, ps.usable_h,
                                config::weston_window_frame(env_nonempty("WAYLAND_DISPLAY"))},
                               d);

  std::printf("%s\n", m.name.c_str());
  std::printf("  the game renders at   %ux%u\n", g.logical_w, g.logical_h);
  if (pw && ph) std::printf("  this screen is        %ux%u\n", pw, ph);
  else std::printf("  this screen is        unknown (no display)\n");
  // A window fits in what the desktop leaves free, so say when that is less.
  if (pw && ph && (ps.usable_w != pw || ps.usable_h != ph))
    std::printf("  its work area is      %ux%u\n", ps.usable_w, ps.usable_h);
  std::printf("  scaling               %s", config::name_of(d.mode));
  if (d.mode == config::ScaleMode::Integer && d.scale) std::printf(", asked for %ux", d.scale);
  std::printf("\n");
  std::printf("  you will see          %ux%u at %ux%s\n", g.logical_w * g.scale,
              g.logical_h * g.scale, g.scale, g.fullscreen ? ", fullscreen" : "");
  return 0;
}

// Hidden: the shelf runs it to measure the panel while its own window holds
// SDL's video under a driver that answers in the wrong units.
int cmd_panel(const rt::Env& /*e*/, std::vector<std::string>& /*a*/) { return gui::panel_main(); }

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
