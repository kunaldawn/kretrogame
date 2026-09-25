#include "prefix.h"

#include <fstream>
#include <stdexcept>
#include <vector>

#include "../util/proc.h"
#include "../wine/registry.h"
#include "../wine/system_files.h"

namespace kg::session {
namespace fs = std::filesystem;

std::string adopt_player_profile(const fs::path& prefix) {
  const fs::path users = prefix / "drive_c" / "users";
  const fs::path player = users / "player";
  std::error_code ec;
  if (!fs::is_directory(users, ec)) return "";
  // symlink_status: a "player" that is a dangling link is still a name taken.
  if (fs::exists(fs::symlink_status(player, ec))) return "";
  std::vector<fs::path> old;
  for (const fs::directory_entry& de : fs::directory_iterator(users, ec)) {
    const std::string n = de.path().filename().string();
    if (n == "Public" || n == "Default" || n == "All Users" || n == "player") continue;
    std::error_code le;
    if (de.is_symlink(le) || !de.is_directory(le)) continue;
    old.push_back(de.path());
  }
  if (old.size() != 1) return "";
  const fs::path was = old.front();
  fs::rename(was, player, ec);
  if (ec) return "";
  // Relative, so the prefix can move with its link still right.
  fs::create_directory_symlink("player", was, ec);
  return "the Wine profile C:\\users\\" + was.filename().string() + " is now C:\\users\\player" +
         (ec ? std::string() : ", and the old name still leads to it");
}

void prepare_prefix(const rt::Env& e, const fs::path& prefix, const fs::path& home,
                    const Meta& m, bool apply_registry,
                    const std::function<void(const std::string&)>& say,
                    const fs::path& system_tree) {
  std::error_code ec;
  fs::create_directories(prefix, ec);
  fs::create_directories(home / ".config", ec);
  if (std::string moved = adopt_player_profile(prefix); !moved.empty()) say(moved);

  // Every command here runs before the nested compositor exists, so none of
  // them is given a display; see rt::offscreen.
  rt::Env we = rt::offscreen(rt::wine_env(e, prefix, home));
  if (!m.runtime.dlloverrides.empty()) we.set("WINEDLLOVERRIDES", m.runtime.dlloverrides);

  // A prefix that has never been initialised takes about a minute, and done
  // lazily at launch that is indistinguishable from a hang.
  if (!fs::exists(prefix / ".kretro-ready", ec)) {
    fs::path wine = rt::find_wine(e.root);
    rt::Env be = we;
    // Without these, wineboot puts up a dialog offering to install Mono and
    // Gecko and waits for someone to click it - which, run from a launcher,
    // looks exactly like a hang. None of these games are .NET or HTML.
    be.set("WINEDLLOVERRIDES", "mscoree,mshtml=");
    // A prefix copied from the runtime's template was initialised when the
    // runtime was built, and carries the Wine version it was made by. Running
    // wineboot --init over it again is a minute spent redoing that.
    if (!fs::exists(prefix / ".kretro-wine-version", ec)) {
      say("preparing the Wine prefix (first run, about a minute)");
      ProcResult r = rt::run(be, wine, {"wineboot", "--init"}, ProcOptions{{}, "", true, 600});
      if (!r.ok()) throw std::runtime_error("could not build the Wine prefix:\n" + r.out);
    }
    // The stamp is written only on success, so an interrupted init is
    // rebuilt rather than used.
    // The graphics driver is pinned to x11 because the game talks to our own
    // Xwayland. Left to itself Wine may pick winewayland, connect to nothing,
    // and render into a void that looks exactly like a black screen.
    rt::run(be, wine, {"reg", "add", "HKCU\\Software\\Wine\\Drivers", "/v", "Graphics",
                       "/d", "x11", "/f"});
    std::ofstream(prefix / ".kretro-ready") << m.runtime.id << "\n";
  }

  // The Windows version a game expects. Left at Wine's default a 2002
  // installer sees Windows 10 and refuses outright - one says "OS not
  // supported for this product" and quits - so this is applied on
  // every prepare, not only at first init, because it is per-game.
  if (!m.run.windows_version.empty()) {
    fs::path wine = rt::find_wine(e.root);
    ProcResult r = rt::run(we, wine, {"winecfg", "/v", m.run.windows_version});
    if (r.ok()) say("windows version: " + m.run.windows_version);
    else say("warning: could not set the windows version to " + m.run.windows_version);
  }

  // The virtual desktop is set in the registry rather than passed as
  // `explorer /desktop=`. The command-line form returns as soon as the desktop
  // exists, so the launcher sees a one-second session and the game is orphaned;
  // the registry form applies to the process itself, which then blocks until
  // the game actually quits.
  {
    fs::path wine = rt::find_wine(e.root);
    std::string geom = std::to_string(m.run.width ? m.run.width : 800) + "x" +
                       std::to_string(m.run.height ? m.run.height : 600);
    rt::run(we, wine, {"reg", "add", "HKCU\\Software\\Wine\\Explorer\\Desktops", "/v",
                       "kretro", "/d", geom, "/f"});
    rt::run(we, wine, {"reg", "add", "HKCU\\Software\\Wine\\Explorer", "/v", "Desktop",
                       "/d", "kretro", "/f"});
  }

  // The files the installer wrote outside the game directory, put back before
  // the fragment that names them. Ordering is the whole of it: wineboot has
  // just finished laying down its own C:, and the fragment below registers COM
  // classes and DLL paths against files that have to be there when it does.
  if (!system_tree.empty() && fs::exists(system_tree, ec)) {
    size_t n = wine::restore_system_files(system_tree, prefix / "drive_c");
    if (n) say("restoring " + std::to_string(n) + " files the installer left outside the game");
  }

  // The keys the installer wrote, put back. wine::apply_fragment has existed
  // and had no caller since it was written; this is the call. It runs after
  // wineboot, because there is no registry to import into before that, and it
  // runs once per prefix - the marker holds the fragment's hash, so a game that
  // writes its own settings between launches keeps them.
  if (apply_registry && !m.registry.fragment.empty() &&
      !wine::registry_marker_matches(prefix, m.registry.fragment)) {
    say("restoring the registry keys the installer wrote");
    wine::apply_fragment(e, prefix, m.registry.fragment);
    wine::write_registry_marker(prefix, m.registry.fragment);
  }
}

}  // namespace kg::session
