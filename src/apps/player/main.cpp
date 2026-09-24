// The player - one file, its games, and nothing else.
//
// By the time main runs the bootstrap has checked the file's table of contents
// and mounted the runtime, and we are under the runtime's own loader. The
// first thing done here is deciding where this player keeps its state, because
// everything after it - the GPU probe's links, the settings, the saves - asks
// for a path, and paths are resolved once.
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "commands.h"
#include "../../gpu/probe.h"
#include "../../gui/launcher/launcher.h"
#include "../../player/cli.h"
#include "../../player/player.h"
#include "../../rt/env.h"
#include "../../util/env.h"
#include "../../util/paths.h"

namespace fs = std::filesystem;
using namespace kg;

namespace {

// The file the person ran. /proc/self/exe is the runtime's loader by now, so
// the bootstrap hands it down.
fs::path self_path() {
  if (const char* s = env_nonempty("KRETRO_SELF")) return s;
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  const fs::path self = self_path();
  const std::string name = self.filename().string();
  player::Command c = player::parse_command(args);
  if (c.kind == player::Command::Kind::Help) {
    std::fputs(player::usage(name).c_str(), stdout);
    return 0;
  }
  if (c.kind == player::Command::Kind::Error) {
    std::fprintf(stderr, "%s: %s\n\n%s", name.c_str(), c.error.c_str(), player::usage(name).c_str());
    return 2;
  }

  try {
    player::Bundle b = player::Bundle::open(self, env_nonempty("KRETRO_TOC") ? env_nonempty("KRETRO_TOC") : "");
    // Before anything else asks where anything is. The gamepad helper runs
    // with the game's HOME, so it takes the state its player chose.
    if (c.kind == player::Command::Kind::Input) {
      player::inherited_state(b, self);
      return player::app::input_helper(b, c.rest);
    }
    player::StateChoice st = player::settle_state(b, self);
    ensure_state_dirs();

    const bool needs_gpu = c.kind != player::Command::Kind::Licenses &&
                           c.kind != player::Command::Kind::SavesExport &&
                           c.kind != player::Command::Kind::SavesImport &&
                           c.kind != player::Command::Kind::ExtractTo;
    gpu::Report gl;
    if (needs_gpu) {
      gl = gpu::probe();
      gpu::materialize(gl);
    }
    rt::Env e = rt::make(needs_gpu ? &gl : nullptr);
    player::Player p(std::move(b), e, st, gl);

    // Said once on a terminal too; the launcher says it in its own window.
    if (!st.refused_portable.empty() && c.kind != player::Command::Kind::Launcher) {
      player::LauncherState ls = player::load_launcher(p.launcher_file());
      if (!ls.portable_fallback_said) {
        std::fprintf(stderr, "%s is there but %s, so saves go to %s instead.\n",
                     st.refused_portable.c_str(), st.refused_why.c_str(), st.dir.c_str());
        ls.portable_fallback_said = true;
        player::save_launcher(p.launcher_file(), ls);
      }
    }

    switch (c.kind) {
      case player::Command::Kind::Launcher: return gui::run_launcher(p);
      case player::Command::Kind::Play: return player::app::play(p, c);
      case player::Command::Kind::Doctor: return player::app::doctor(p, c);
      case player::Command::Kind::Licenses: return player::app::licenses(p);
      case player::Command::Kind::SavesExport: return player::app::saves_export(p, c);
      case player::Command::Kind::SavesImport: return player::app::saves_import(p, c);
      case player::Command::Kind::ExtractTo: return player::app::extract(p, c);
      default: return 2;
    }
  } catch (const player::Damaged& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s: %s\n", name.c_str(), ex.what());
    return 1;
  }
}
