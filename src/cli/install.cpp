// kretro create, install and swap: getting a game onto the shelf.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli.h"
#include "../disc/drive.h"
#include "../gui/shelf/shelf.h"
#include "handlers.h"
#include "../install/collection.h"
#include "../install/install.h"
#include "../install/manifest.h"
#include "../install/staging.h"
#include "../pack/kgpack.h"
#include "../rt/env.h"
#include "../util/format.h"
#include "../util/hash.h"

namespace fs = std::filesystem;

namespace kg::cli {

int cmd_create(const rt::Env& e, std::vector<std::string>& /*a*/) {
  gui::Startup entry;
  entry.create = true;
  return gui::run(e, entry);
}

// `kretro install <id>` is a way into the wizard now rather than a way around
// it. The manifest is looked up here, on the terminal where the id was typed,
// so a typo is answered by the line under it rather than by a window that opens
// and then complains.
//
// Except where there is nobody to be in the room. A copy or unzip install asks
// no questions: the manifest holds every answer an installer would have wanted,
// and install::run - the same engine import_pack rebuilds a recipe with - can
// produce the pack with no display at all. So the wizard is what a person gets,
// and --headless is what a script gets; a machine with no Wayland or X11
// session takes the headless road without being asked, because the alternative
// there is a window that cannot open.
//
// wine_setup and installer_exe refuse. Clicking through somebody's 1998 setup -
// its directory, its questions, its serial - is the whole point of the wizard,
// and a script claiming to do it would be lying about what happened.
int cmd_install(const rt::Env& e, std::vector<std::string>& a) {
  std::string id;
  bool headless = false;
  install::Options opt;
  for (const std::string& s : a) {
    if (s == "--headless") {
      headless = true;
    } else if (s == "--force") {
      opt.force = true;
    } else if (s == "--keep-tree") {
      opt.keep_tree = true;
    } else if (!s.empty() && s[0] == '-') {
      std::fprintf(stderr, "kretro: unknown option %s\n", s.c_str());
      return 2;
    } else {
      id = s;
    }
  }
  if (id.empty()) return usage();
  fs::path toml = install::find_manifest(e, id);
  if (toml.empty()) {
    std::fprintf(stderr, "kretro: no manifest for '%s'. Try: kretro games\n", id.c_str());
    return 1;
  }

  const bool screen = std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY");
  if (!headless && screen) {
    gui::Startup entry;
    entry.create = true;
    entry.preset = id;
    return gui::run(e, entry);
  }

  Meta m = install::load_manifest(toml);
  const std::string& method = m.recipe.method;
  if (method != "copy" && method != "unzip") {
    std::fprintf(stderr,
                 "kretro: %s installs by running its own installer, and that needs a person.\n"
                 "  Its method is %s: somebody has to answer %s under Wine - where to put\n"
                 "  the game, which pieces to install, the serial off the box. Nothing here\n"
                 "  can do that for you, so this refuses rather than pretend.\n"
                 "  Run it on a desktop and the wizard will walk you through: kretro install %s\n",
                 m.name.c_str(), method.c_str(),
                 m.recipe.setup.empty() ? "the installer" : m.recipe.setup.c_str(), id.c_str());
    return 1;
  }
  if (!headless) {
    log_line("no display, and " + m.name + " needs nobody at an installer - installing here");
  }

  std::fprintf(stderr, "%s\n", m.name.c_str());
  install::Result r = install::run(e, m, opt, log_line);
  std::printf("\n%s\n", r.pack.c_str());
  std::printf("  %s in %zu files, packed to %s\n", fmt::bytes_iec(r.tree_bytes).c_str(), r.entries,
              fmt::bytes_iec(r.pack_bytes).c_str());
  std::printf("  root %s\n", to_hex(r.root).c_str());
  std::printf("  kretro play %s\n", m.id.c_str());
  return 0;
}

// Short form: `kretro swap <id> <n>` while an install is running. An installer
// that insists on one drive gets the disc it asked for, and because a Wine
// drive is a symlink resolved on every open, it sees the change immediately.
int cmd_swap(const rt::Env& /*e*/, std::vector<std::string>& a) {
  const std::string& id = a[0];
  int n = std::atoi(a[1].c_str());
  fs::path work = install::staging_dir(id);
  fs::path prefix = install::staging_prefix(work);
  std::error_code ec;
  if (!fs::exists(prefix, ec)) {
    std::fprintf(stderr, "kretro: no install of %s is running\n", id.c_str());
    return 1;
  }
  if (n < 1 || n > 8) { std::fprintf(stderr, "kretro: disc %d?\n", n); return 1; }
  fs::path tree = install::staging_drive(work, static_cast<size_t>(n - 1));
  if (!fs::exists(tree, ec)) {
    std::fprintf(stderr, "kretro: %s has no disc %d mounted\n", id.c_str(), n);
    return 1;
  }
  std::string label;
  { std::ifstream f(tree / ".windows-label"); std::getline(f, label); }
  disc::repoint(prefix, 'd', tree);
  std::printf("D: is now disc %d (%s). The installer should carry on.\n", n, label.c_str());
  return 0;
}

}  // namespace kg::cli
