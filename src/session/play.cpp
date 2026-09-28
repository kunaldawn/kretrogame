#include "play.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../config/config.h"
#include "../config/scaling.h"
#include "../disc/drive.h"
#include "../util/env.h"
#include "../util/format.h"
#include "../util/paths.h"
#include "../util/pe.h"
#include "compositor.h"
#include "internal.h"
#include "journal.h"
#include "lock.h"
#include "prefix.h"
#include "saves.h"
#include "saves_layout.h"

namespace kg::session {
namespace fs = std::filesystem;

using detail::apply_backend;
using detail::find_ci_in_dir;
using detail::MountInterruptGuard;

namespace {

// What play() knows about the session so far, filled in step by step. The
// things that own something - the lock, the pack, the layers and the guard
// over the mounts - are not in here: they are play()'s own locals, so the
// order they are released in is the order they are declared in.
struct PlayState {
  PlayState(const rt::Env& env, const PlayRequest& request, Outcome& out)
      : e(env), id(request.id), req(request), outcome(out) {}

  const rt::Env& e;
  const std::string& id;
  const PlayRequest& req;
  Outcome& outcome;

  fs::path pack_path;
  const Meta* m = nullptr;
  std::string exe;
  std::optional<backend::Plan> backend;
  fs::path prefix;
  fs::path home;
  rt::Env we;
  std::string dos_dir;
  config::Geometry geo;
  CompositorOptions co;
  fs::path wine;
  std::vector<std::string> gargs;
  std::time_t started = 0;
  std::time_t ended = 0;

  void plan(const std::string& k, const std::string& v) { outcome.plan.emplace_back(k, v); }

  // A line for stderr, and for the caller's window when it has one.
  void say(const std::string& line) const {
    log_line(line);
    if (req.hooks.say) req.hooks.say(line);
  }
  std::function<void(const std::string&)> sayer() const {
    return [this](const std::string& line) { say(line); };
  }
};

// Where a caller's Stop is heard. Between steps rather than inside one: a
// step cut short is a prefix half made, and the next session would have to
// tell that apart from one that is whole.
void stop_if_asked(const PlayState& st) {
  if (st.req.hooks.stop_requested && st.req.hooks.stop_requested()) {
    throw std::runtime_error("stopped before " + st.id + " started");
  }
}

// A refusal is said before anything is locked, mounted or started: there
// is nothing for the person to wait through when the answer is already no.
void refuse_fixed_backend(const PlayRequest& req) {
  const std::optional<backend::Plan>& fixed = req.backend.fixed;
  if (fixed && fixed->refused()) throw std::runtime_error(fixed->decision.reason);
}

void check_installed(PlayState& st) {
  std::error_code ec;
  st.pack_path = st.req.source ? st.req.source->file : game_pack(st.id);
  if (!fs::exists(st.pack_path, ec)) {
    throw std::runtime_error(st.id + " is not installed. Try: kretro install " + st.id);
  }
}

// Before open_layers, because open_layers' first act is to unmount whatever
// it finds at this game's mountpoints - which, if another kretro is playing,
// is that game's live filesystem.
void take_lock(const PlayState& st, GameLock& lock) {
  if (!st.req.held_lock) {
    lock = lock_game(st.id);
    if (lock.busy()) {
      throw std::runtime_error(st.id + " is already being played by another kretro. Close that "
                                       "one first - two of them share one set of saves.");
    }
  }
}

// A player's table names each pack by the game it is; a pack that says it
// is another game would have its saves and prefix filed under the wrong one.
void check_pack(PlayState& st, const Pack& pack) {
  st.m = &pack.game(st.id);
  const Meta& m = *st.m;
  if (m.id != st.id) throw std::runtime_error("the pack for " + st.id + " says it is " + m.id);
  st.plan("game", m.name.empty() ? st.id : m.name);
  st.plan("pack", st.req.source ? st.pack_path.filename().string() + " at " + std::to_string(pack.base())
                                : st.pack_path.string());
}

void mount(PlayState& st, Layers& layers, const Pack& pack) {
  open_layers(layers, st.e, pack, st.id, st.req.source ? &*st.req.source : nullptr);

  st.say(std::string("game directory: ") +
      (layers.writes_isolated() ? "overlay (writes captured live)" : "extracted (writes found at exit)"));

  st.plan("game directory", layers.writes_isolated() ? "overlay, writes captured live"
                                                      : "unpacked, writes found at exit");
  st.plan("mounted at", layers.merged.string());
}

void resolve_exe(PlayState& st, const Layers& layers) {
  st.exe = find_ci_in_dir(layers.merged, st.m->run.exe);
  if (st.exe.empty()) throw std::runtime_error("no " + st.m->run.exe + " in the installed game");
  st.plan("exe", st.exe);
}

// How the game is drawn, decided from its own imports when the caller asked
// for that. Before the prefix is touched: a refusal is still said before
// anything is written.
void choose_backend(PlayState& st, const Layers& layers) {
  st.backend = st.req.backend.fixed;
  if (!st.backend && st.req.backend.for_exe) st.backend = st.req.backend.for_exe(layers.merged / st.exe);
  if (st.backend && st.backend->refused()) throw std::runtime_error(st.backend->decision.reason);
}

void make_prefix_dirs(PlayState& st) {
  std::error_code ec;
  st.prefix = game_prefix_dir(st.id);
  st.home = game_home_dir(st.id);
  st.plan("prefix", st.prefix.string());
  fs::create_directories(st.prefix, ec);
  fs::create_directories(st.home / ".config", ec);
}

// The fragment records where the game was installed to on the machine that
// built the pack: C:\Program Files (x86)\<Publisher>\<Game>, and some games
// read their install directory straight back out of HKLM. At play time the
// game is not there - it is on the overlay, which Wine reaches as Z:\ - so
// applying the fragment verbatim would point the game at a directory that
// does not exist.
//
// So the recorded path is made real, as a symlink to the game directory,
// before prepare_prefix imports anything. Then the fragment applies
// unaltered, run.exe is still resolved under merged and the cwd is still
// merged, and nothing else has to move. This is why Meta::Install::install_dir
// survived the cull of the other five install fields.
//
// Meta::decode refuses an install_dir that is not a relative path under
// drive_c, but this is the line that removes a file and puts a symlink where
// it was, and a Meta does not only ever come from a decoded pack. The check
// is made again where the damage would be done.
void link_install_dir(const PlayState& st, const Layers& layers) {
  const Meta& m = *st.m;
  std::error_code ec;
  if (!m.install.install_dir.empty() && install_dir_is_safe(m.install.install_dir)) {
    fs::path link = st.prefix / "drive_c" / fs::path(m.install.install_dir);
    fs::create_directories(link.parent_path(), ec);
    // remove, not remove_all: a real directory there is somebody's data and a
    // symlink is all we are entitled to replace.
    fs::remove(link, ec);
    ec.clear();
    fs::create_directory_symlink(layers.merged, link, ec);
    if (ec) st.say("warning: C:\\" + m.install.install_dir + " could not be pointed at the game");
  }
}

void build_wine_env_and_prefix(PlayState& st, const Layers& layers) {
  const Meta& m = *st.m;
  st.we = rt::wine_env(st.e, st.prefix, st.home);
  if (!m.runtime.dlloverrides.empty()) st.we.set("WINEDLLOVERRIDES", m.runtime.dlloverrides);

  // The game's own system/ in the set's image, beside its game/.
  session::prepare_prefix(st.e, st.prefix, st.home, m, /*apply_registry=*/true, st.sayer(),
                          layers.image / body_game_dir(m.id) / "system");
}

// Meta.discs is filled at install time, and this is what it is for: a game that checks for its disc at runtime - and the whole of this
// era does - gets the same drives back that it was installed with.
//
// The trees come out of the pack, at image/discs/<key>, each already carrying
// the .windows-label and .windows-serial written when the pack was built.
// Nothing is written into the saves layer, so nothing new enters a snapshot
// or a save export.
void attach_discs(PlayState& st, const Layers& layers) {
  const Meta& m = *st.m;
  std::error_code ec;
  if (!m.discs.empty()) {
    // The letter comes from the disc's position, not from how many have been
    // attached so far, because install assigns 'd' + i to every disc it mounts.
    // Advancing only on success would hand disc 2 the letter disc 1 had, and a
    // game that recorded D: at install time would look at the wrong drive.
    for (size_t i = 0; i < m.discs.size() && 'd' + i <= 'z'; ++i) {
      const char letter = static_cast<char>('d' + i);
      fs::path tree = layers.image / body_disc_dir(m.discs[i].key);
      if (!fs::exists(tree, ec)) {
        // The pack's metadata lists the disc and its body does not have it:
        // said, and the rest of the discs are still attached.
        st.say("disc " + std::to_string(i + 1) + " (" + m.discs[i].label + ") is missing from this pack");
        continue;
      }
      disc::attach_cdrom(st.e, st.prefix, letter, tree);
      st.say(std::string("  ") + static_cast<char>(std::toupper(letter)) + ": " + m.discs[i].label);
      st.plan(std::string(1, static_cast<char>(std::toupper(letter))) + ":", "CD-ROM " + m.discs[i].label);
    }
  }
}

// The game's own drive, for a prefix with no Z:. A plain symlink, as the
// discs are; Wine finds the drive a working directory is on by walking up
// it and comparing each directory with each drive's root, so a game started
// in merged is started on this drive, at its root.
void map_game_drive(PlayState& st, const Layers& layers) {
  if (st.req.game_drive) {
    const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(st.req.game_drive)));
    disc::repoint(st.prefix, letter, layers.merged);
    st.dos_dir = std::string(1, static_cast<char>(std::toupper(letter))) + ":\\";
    st.plan(st.dos_dir.substr(0, 2), "the game");
  }
}

void run_after_prefix(PlayState& st, const Layers& layers) {
  if (st.req.hooks.after_prefix) st.req.hooks.after_prefix(st.we, st.prefix, layers.merged);
}

// dgVoodoo2 wraps DirectX 1 through 7 and Glide, which is the era every game
// here belongs to - DXVK cannot stand in, it starts at Direct3D 9. It is also
// not redistributable on the terms this project ships under, so it is not in
// the runtime. Saying so is better than accepting the flag and doing nothing.
void apply_dgvoodoo(PlayState& st, const Layers& layers) {
  const Meta& m = *st.m;
  std::error_code ec;
  if (st.req.backend.dgvoodoo || m.runtime.dgvoodoo) {
    fs::path dg = st.e.root / "opt" / "dgvoodoo";
    if (fs::exists(dg / "MS" / "x86" / "D3D8.dll", ec)) {
      for (const char* dll : {"D3D8.dll", "DDraw.dll", "D3DImm.dll"}) {
        fs::copy_file(dg / "MS" / "x86" / dll, fs::path(layers.merged) / dll,
                      fs::copy_options::overwrite_existing, ec);
      }
      fs::copy_file(dg / "dgVoodoo.conf", fs::path(layers.merged) / "dgVoodoo.conf",
                    fs::copy_options::overwrite_existing, ec);
      // This sets WINEDLLOVERRIDES afresh from the pack's own overrides, and
      // so drops whatever after_prefix added to it. It is not Env::append, on
      // purpose: that would change what Wine is given.
      std::string ov = "d3d8,ddraw,d3dim=n";
      if (!m.runtime.dlloverrides.empty()) ov = m.runtime.dlloverrides + ";" + ov;
      st.we.set("WINEDLLOVERRIDES", ov);
      st.say("dgVoodoo2: wrapping DirectX with the bundled copy");
    } else {
      st.say("dgVoodoo2 was asked for but is not in this runtime.");
      st.say("  It is not redistributable on the terms kretro ships under. Put its");
      st.say("  MS/x86 DLLs and dgVoodoo.conf under opt/dgvoodoo in the runtime and");
      st.say("  this will pick them up. Playing without it.");
    }
  }
}

// Before the renderer below, so the bake-off's explicit choice still wins.
void apply_graphics(PlayState& st) {
  if (st.backend) {
    const backend::Plan& b = *st.backend;
    apply_backend(st.e, st.we, st.prefix, b);
    st.plan("graphics", std::string(backend::backend_name(b.decision.backend)) + " - " + b.decision.reason);
    st.plan("display path", backend::display_path_name(b.display));
  }
}

// A game that draws with OpenGL itself is given an extension list of the
// length its era expects; see backend::gl_extension_cap for why. It is
// decided here, for every caller, rather than only in the player's backend
// plan: kretro's own play asks for no plan, and this is the difference between
// a game that starts and one that dies a second after it does.
void cap_gl_extensions(PlayState& st, const Layers& layers) {
  if (!backend::draws_with_opengl(pe::parse_file(layers.merged / st.exe))) return;
  bool capped = false;
  for (const auto& [k, v] : backend::gl_extension_cap()) {
    if (std::getenv(k.c_str())) continue;
    st.we.set(k, v);
    capped = true;
  }
  if (capped) st.say("OpenGL: the extension list is cut to the length a game of this age expects");
  st.plan("opengl", capped ? "extension list cut short" : "extension list as the person set it");
}

void apply_renderer(const PlayState& st) {
  const std::string& renderer = st.req.backend.wined3d_renderer;
  if (!renderer.empty()) {
    fs::path wine = rt::find_wine(st.e.root);
    rt::run(rt::offscreen(st.we), wine, {"reg", "add", "HKCU\\Software\\Wine\\Direct3D", "/v", "renderer", "/d", renderer, "/f"});
    st.say("renderer backend: " + renderer);
  }
}

// Where this game goes on this panel: the manifest says what the game renders
// at, the settings say what to do with it, and the panel says what will fit.
// The panel size comes from the caller. This layer draws nothing and knows
// about no display server, and linking SDL into it would drag a window
// toolkit into the packing tool and the unit tests.
void geometry(PlayState& st) {
  const Meta& m = *st.m;
  // On a Wayland host the window is the game plus the frame Weston draws
  // around it, and the whole window has to fit the work area.
  const config::Panel panel{st.req.display.panel_w, st.req.display.panel_h,
                            st.req.display.usable_w, st.req.display.usable_h,
                            config::weston_window_frame(env_nonempty("WAYLAND_DISPLAY"))};
  config::Display want = st.req.display.scaling
                             ? *st.req.display.scaling
                             : config::for_game(config::load(config::config_file()), st.id);
  st.geo = config::compute_geometry(m.run.width, m.run.height, panel, want);

  // Native gives the game an X screen the size of the panel, which is what it
  // finds as the desktop's resolution.
  if (st.geo.desktop_is_panel) {
    st.say("native: the game is asked to render at " + std::to_string(st.geo.logical_w) + "x" +
           std::to_string(st.geo.logical_h));
  }
}

// A game plays on the X screen itself, not in Wine's virtual desktop.
//
// prepare_prefix names the desktop, which an installer wants: its windows
// stay in one screen the stage can show. A game changing its display mode
// in there is a different matter. Wine 11 answers ChangeDisplaySettings in
// a virtual desktop by resizing the desktop, never the X screen, so a
// 640x480 movie or an 800x600 menu was drawn in the top-left corner of a
// 1024x768 screen, with black round it. Without the desktop the mode change
// is a RandR one, the nested Xwayland emulates it (CompositorOptions::
// emulate_modes), and the game fills its screen at every resolution it
// uses. The screen is still ours, not the host's: no mode ever reaches the
// person's desktop.
void leave_virtual_desktop(const PlayState& st) {
  fs::path wine = rt::find_wine(st.e.root);
  rt::run(rt::offscreen(st.we), wine, {"reg", "delete", "HKCU\\Software\\Wine\\Explorer", "/v", "Desktop", "/f"});
}

void compositor_options(PlayState& st) {
  CompositorOptions& co = st.co;
  co.width = st.geo.logical_w;
  co.height = st.geo.logical_h;
  co.scale = st.geo.scale;
  co.window_scale = st.geo.window_scale;
  co.fullscreen = st.geo.fullscreen || st.req.display.fullscreen;
  co.socket_suffix = st.id;
  co.home = st.home;
  co.capture_dir = journal_dir(st.id);
  co.capture = st.req.record;
  co.pause_on_blur = st.m->present.pause_on_blur;
  co.pointer_capture = true;  // a game, unlike an installer, keeps the pointer
  co.emulate_modes = true;    // see leave_virtual_desktop
  // Closing the window asks the game to quit, so that it saves what it keeps
  // until it exits; a second close ends it.
  co.close_image = fs::path(st.exe).filename().string();
  co.stop_after = st.req.stop_after;
  // Xwayland answering is the game's screen being up: Weston's window is
  // mapped by then, and the game is started a moment later.
  if (st.req.hooks.screen_up) {
    co.on_display_ready = [&st](const std::string&) { st.req.hooks.screen_up(); };
  }
}

void command_line(PlayState& st) {
  const Meta& m = *st.m;
  st.wine = rt::find_wine(st.e.root);
  st.gargs = {st.dos_dir.empty() ? st.exe : st.dos_dir + st.exe};
  if (!m.run.args.empty()) {
    std::istringstream as(m.run.args);
    std::string a;
    while (as >> a) st.gargs.push_back(a);
  }
  {
    const CompositorOptions& co = st.co;
    std::string cmd;
    for (const std::string& a : st.gargs) cmd += (cmd.empty() ? "" : " ") + a;
    st.plan("command", cmd);
    st.plan("screen", std::to_string(co.width) + "x" + std::to_string(co.height) + " at " +
                          std::to_string(co.scale) + "x" + (co.fullscreen ? ", fullscreen" : ""));
  }
}

void launch(PlayState& st, const Layers& layers) {
  st.started = std::time(nullptr);
  // The prefix was got ready with no display, and the wineserver that did it
  // lingers for a few seconds. Stopping it means the game starts a server of
  // its own, with every process in it, explorer's desktop included, on the
  // game's display. The game's lock is held, so nothing else of this prefix
  // is running. -k rather than -w: waiting for the server to leave by itself
  // adds about three seconds to every launch.
  if (fs::path ws = rt::which(st.e, "wineserver"); !ws.empty()) rt::run(st.we, ws, {"-k"});
  CompositorResult cres;
  if (st.backend && st.backend->display == backend::DisplayPath::GamescopeDirect) {
    // gamescope is already the compositor, already fullscreen and already
    // scaling; Wine goes straight onto its display. No frames are harvested
    // on this road, because there is no compositor of ours to read them from.
    st.say("display: gamescope, without a nested compositor");
    if (st.req.hooks.screen_up) st.req.hooks.screen_up();
    ProcOptions po;
    po.cwd = layers.merged.string();
    po.capture = false;
    po.timeout_sec = st.req.stop_after;
    ProcResult pr = rt::run(st.we, st.wine, st.gargs, po);
    cres.status = pr.status;
    // As run_in_compositor does: Wine's server and whatever the game left
    // running outlive the game, with their working directory on the overlay,
    // and the overlay cannot be unmounted or snapshotted cleanly under them.
    fs::path ws = rt::which(st.e, "wineserver");
    if (!ws.empty()) rt::run(st.we, ws, {"-k"});
  } else {
    cres = run_in_compositor(st.e, st.we, st.wine, st.gargs, layers.merged, st.co, st.sayer());
  }
  st.outcome.status = cres.status;
}

void keep_frame(const PlayState& st) {
  std::error_code ec;
  fs::path frames = journal_dir(st.id);
  if (!st.req.capture_to.empty()) {
    fs::create_directories(st.req.capture_to.parent_path(), ec);
    fs::copy_file(last_frame_file(frames), st.req.capture_to, fs::copy_options::overwrite_existing, ec);
  }
}

void record(PlayState& st, const Layers& layers) {
  const std::string& id = st.id;
  Outcome& outcome = st.outcome;
  std::error_code ec;
  Record rec;
  rec.started = st.started;
  rec.ended = st.ended;
  rec.runtime_id = st.m->runtime.id;
  rec.note = st.req.note;
  rec.status = outcome.status;

  // A snapshot that could not be completed is now said out loud rather than
  // silently filed as a generation, and it must not take the journal entry
  // with it: the session happened, it lasted this long, and that is worth
  // recording whether or not the copy of what it wrote succeeded. Nothing
  // has been deleted here, so there is nothing to undo.
  auto keep = [&](const fs::path& from) {
    try {
      fs::path g = snapshot(id, from);
      if (!g.empty()) { outcome.generation = g; rec.generation = g.filename().string(); }
    } catch (const std::exception& ex) {
      log_line(std::string("warning: ") + ex.what() + " - no snapshot of this session");
    }
  };

  if (layers.writes_isolated()) {
    // The filesystem already told us exactly what changed.
    Tree after = Tree::from_directory(layers.upper);
    outcome.diff.added = after.entries();
    rec.files_written = after.size();
    keep(layers.upper);
  } else {
    // No overlay, so compare the tree against what the pack says it was.
    Tree after = Tree::from_directory(layers.merged);
    outcome.diff = st.m->tree.diff_to(after);
    // The extra layer's files were put there for the game, not by it.
    if (!layers.layered.empty()) {
      auto ours = [&](const TreeEntry& te) {
        return std::find(layers.layered.begin(), layers.layered.end(), te.path) != layers.layered.end();
      };
      for (auto* list : {&outcome.diff.added, &outcome.diff.changed}) {
        list->erase(std::remove_if(list->begin(), list->end(), ours), list->end());
      }
    }
    rec.files_written = outcome.diff.added.size() + outcome.diff.changed.size();
    if (rec.files_written) {
      fs::path staging = game_saves_dir(id) / "staging";
      fs::remove_all(staging, ec);
      for (const auto& list : {outcome.diff.added, outcome.diff.changed}) {
        for (const TreeEntry& te : list) {
          if (te.is_dir()) continue;
          fs::path dst = staging / te.path;
          fs::create_directories(dst.parent_path(), ec);
          fs::copy_file(layers.merged / te.path, dst, fs::copy_options::overwrite_existing, ec);
        }
      }
      keep(staging);
      fs::remove_all(staging, ec);
    }
  }
  write_record(id, rec);
  outcome.journal_written = true;
}

}  // namespace

// play() is a list of steps, and their order is the whole of its behaviour:
//
//  1. A backend the caller already refused is said before the lock is taken,
//     and before anything is mounted or started.
//  2. The lock is taken before the stale-mount sweep in open_layers: that
//     sweep unmounts whatever is at this game's mountpoints, which is another
//     kretro's live game if one is playing it.
//  3. The signal handlers (MountInterruptGuard) are in place before the first
//     mount, so a signal while mounting still releases what went up.
//  4. for_exe is asked, and may refuse, before anything is written into
//     the prefix.
//  5. The install_dir symlink is made before prepare_prefix, which imports
//     the registry fragment that names it.
//  6. after_prefix runs before dgVoodoo and the backend, which add to or
//     replace the environment it was handed.
//  7. The renderer is set last of the prefix writes, so the bake-off's
//     explicit choice wins over the backend's.
//  8. A dry run returns after every prefix write and before anything is
//     launched.
//  9. A caller's stop is heard before the prefix is touched, after it is
//     ready, and before the game starts; never inside a step.
//
// The lock, the pack, the layers and the guard are declared here, in that
// order, so the mounts are closed before the lock is let go.
Outcome play(const rt::Env& e, const PlayRequest& req) {
  refuse_fixed_backend(req);
  Outcome outcome;
  PlayState st{e, req, outcome};
  check_installed(st);
  GameLock lock;
  take_lock(st, lock);
  Pack pack = req.source ? Pack::open(st.pack_path, req.source->off, req.source->len)
                         : Pack::open(st.pack_path);
  check_pack(st, pack);

  // The handler is in place before the first mount, not after the last:
  // mounting the image waits up to two seconds, and the overlay after it as
  // long again, and a Ctrl-C or a Stop in that time took the default action
  // and left both mounted - under a scratch HOME's state, for a preview,
  // where no later session would find them to sweep.
  Layers layers;
  MountInterruptGuard guard(layers);
  mount(st, layers, pack);

  resolve_exe(st, layers);
  choose_backend(st, layers);
  stop_if_asked(st);
  make_prefix_dirs(st);
  link_install_dir(st, layers);
  build_wine_env_and_prefix(st, layers);
  stop_if_asked(st);
  attach_discs(st, layers);
  map_game_drive(st, layers);
  run_after_prefix(st, layers);
  apply_dgvoodoo(st, layers);
  apply_graphics(st);
  cap_gl_extensions(st, layers);
  apply_renderer(st);
  geometry(st);
  leave_virtual_desktop(st);
  compositor_options(st);
  command_line(st);
  if (req.dry_run) {
    st.say("dry run: everything is ready, and the game is not started");
    return outcome;
  }
  stop_if_asked(st);

  launch(st, layers);
  keep_frame(st);
  st.ended = std::time(nullptr);
  outcome.seconds = static_cast<double>(st.ended - st.started);
  if (req.record) record(st, layers);
  return outcome;
}

}  // namespace kg::session
