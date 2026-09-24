// kretro play, compare and input: running a game.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "cli.h"
#include "../config/config.h"
#include "../gui/gamepad_bridge.h"
#include "../gui/screen.h"
#include "handlers.h"
#include "../pack/kgpack.h"
#include "../rt/env.h"
#include "../session/input_helper.h"
#include "../session/play.h"
#include "../util/format.h"
#include "../util/paths.h"

namespace fs = std::filesystem;

namespace kg::cli {

int cmd_play(const rt::Env& e, std::vector<std::string>& a) {
  session::PlayRequest req;
  const gui::PanelSize ps = gui::desktop_size(false);
  req.display.panel_w = ps.w;
  req.display.panel_h = ps.h;
  // A partial override starts from the defaults, not from this game's
  // setting in config.toml.
  auto scaling_of = [](session::PlayRequest& r) -> config::Display& {
    if (!r.display.scaling) r.display.scaling.emplace();
    return *r.display.scaling;
  };
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] == "--fullscreen") req.display.fullscreen = true;
    else if (a[i] == "--windowed") req.display.fullscreen = false;
    else if (a[i] == "--dgvoodoo") req.backend.dgvoodoo = true;
    else if (a[i] == "--no-journal") req.record = false;
    // Everything up to the game - lock, mounts, prefix, backend - and then
    // the plan instead of the game: how a machine with no display, or a
    // test, finds out whether this game would get its writable layer.
    else if (a[i] == "--dry-run") req.dry_run = true;
    else if (a[i] == "--note" && i + 1 < a.size()) req.note = a[++i];
    // These override the setting for this game for one session, without
    // writing anything: useful for finding out what you like.
    else if (a[i] == "--integer") scaling_of(req).mode = config::ScaleMode::Integer;
    else if (a[i] == "--fit") scaling_of(req).mode = config::ScaleMode::Fit;
    else if (a[i] == "--native") scaling_of(req).mode = config::ScaleMode::Native;
    else if (a[i] == "--scale" && i + 1 < a.size()) {
      scaling_of(req).scale = static_cast<uint32_t>(std::atoi(a[++i].c_str()));
    }
    else if (!a[i].empty() && a[i][0] == '-') { std::fprintf(stderr, "kretro: unknown option %s\n", a[i].c_str()); return 2; }
    else req.id = a[i];
  }
  if (req.id.empty()) return usage();
  const std::string& id = req.id;
  session::Outcome o = session::play(e, req);
  if (req.dry_run) {
    std::printf("%s is ready to play. Not started (--dry-run):\n", id.c_str());
    size_t w = 0;
    for (const auto& [k, v] : o.plan) w = std::max(w, k.size());
    for (const auto& [k, v] : o.plan) std::printf("  %-*s  %s\n", static_cast<int>(w), k.c_str(), v.c_str());
    return 0;
  }
  std::printf("\nplayed for %s\n", fmt::duration(o.seconds).c_str());
  size_t wrote = o.diff.added.size() + o.diff.changed.size();
  if (wrote) std::printf("the game wrote %zu file%s\n", wrote, wrote == 1 ? "" : "s");
  if (!o.generation.empty()) std::printf("snapshot %s\n", o.generation.filename().c_str());
  return o.status;
}

int cmd_compare(const rt::Env& e, std::vector<std::string>& a) {
  // Which renderer looks correct is a question about pixels, and today it
  // is answered by trial and error over an afternoon. We own the
  // compositor, so we can just take the pictures.
  const std::string& id = a[0];
  int secs = 25;
  for (size_t i = 1; i + 1 < a.size(); ++i) {
    if (a[i] == "--seconds") secs = std::atoi(a[++i].c_str());
  }
  fs::path dir = state_dir() / "compare" / id;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  const char* backends[] = {"gl", "vulkan", "gdi"};
  std::printf("Running %s under each renderer for %ds and keeping a frame.\n\n", id.c_str(), secs);
  for (const char* b : backends) {
    session::PlayRequest req;
    req.id = id;
    req.backend.wined3d_renderer = b;
    req.stop_after = secs;
    req.capture_to = dir / (std::string(b) + ".png");
    std::fprintf(stderr, "-- %s --\n", b);
    try {
      session::play(e, req);
    } catch (const std::exception& ex) {
      std::fprintf(stderr, "   %s\n", ex.what());
    }
  }
  std::printf("\nLook at these side by side and keep the one that is right:\n");
  for (const char* b : backends) {
    fs::path f = dir / (std::string(b) + ".png");
    if (fs::exists(f, ec)) std::printf("  %-7s %s\n", b, f.c_str());
    else std::printf("  %-7s (no frame captured)\n", b);
  }
  return 0;
}

int cmd_input(const rt::Env& /*e*/, std::vector<std::string>& a) {
  // Started by a session beside a game; not something to run by hand.
  const session::InputHelperArgs ia = session::parse_input_helper(a);
  if (!ia.valid()) return usage();
  // The game's own bindings, when the session named one.
  std::map<std::string, std::string> binds;
  if (!ia.game.empty()) {
    std::error_code ec;
    fs::path pk = game_pack(ia.game);
    if (fs::exists(pk, ec)) {
      try { binds = Pack::open(pk).meta().input; } catch (const std::exception&) {}
    }
  }
  return gui::run_input(ia.display, ia.pid, ia.pause, binds);
}

}  // namespace kg::cli
