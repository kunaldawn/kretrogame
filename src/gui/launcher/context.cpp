#include "context.h"

#include <SDL.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <utility>

#include "../../session/saves_layout.h"
#include "../../util/env.h"
#include "../../util/paths.h"
#include "../format.h"
#include "../screen.h"

namespace kg::gui::launcher {
namespace fs = std::filesystem;

LauncherContext::LauncherContext(const player::Player& pl, const Window& win)
    : p(pl), w(win), textures(win.ren), state(player::load_launcher(pl.launcher_file())) {
  for (const bundle::GameMeta& g : p.bundle().meta.games) ids.push_back(g.id);
  selected = ids.front();
}

// ---- the worker -------------------------------------------------------------

void LauncherContext::run_job(const std::string& title, std::function<void()> fn,
                              std::function<void(const std::string& error)> next) {
  if (job.running()) return;
  set_progress("");
  then = std::move(next);
  job.start(title, std::move(fn));
}

void LauncherContext::finish_job() {
  if (!job.running() || !job.finished()) return;
  std::string err = job.join();
  auto after = std::move(then);
  then = nullptr;
  if (after) after(err);
}

void LauncherContext::set_progress(const std::string& s) {
  std::lock_guard<std::mutex> lk(mu);
  progress = s;
}

std::string LauncherContext::progress_text() {
  std::lock_guard<std::mutex> lk(mu);
  return progress;
}

// ---- playing ----------------------------------------------------------------

const Texture* LauncherContext::cover(const std::string& id) {
  const bundle::GameMeta& g = p.game(id);
  if (const Texture* t = textures.png("cover:" + id, g.cover)) return t;
  // No cover from the author: the game's own screen, once it has been played.
  return textures.file(session::title_art_file(session::journal_dir(id)));
}

void LauncherContext::fail(const std::string& id, const std::string& what, bool with_log) {
  failure_game = id;
  failure = what;
  failure_log = with_log ? p.last_log(40) : "";
  selected = id;
  go(Screen::Game);
}

// Step one of playing: the pack is checked the first time this version of
// the bundle plays it. Hashing a pack that carries its disc is gigabytes, so
// it is a job with its own progress, and it is only ever done once.
void LauncherContext::play(const std::string& id) {
  status.clear();
  failure.clear();
  if (!p.verified(id)) {
    const std::string name = p.game(id).name;
    run_job("Checking " + name + " (the first time only)",
            [this, id] {
              p.verify(id, [this](uint64_t done, uint64_t total) {
                set_progress(std::to_string(total ? done * 100 / total : 100) + "%");
              });
            },
            [this, id](const std::string& err) {
              if (!err.empty()) fail(id, err, false);
              else play_checked(id);
            });
    return;
  }
  play_checked(id);
}

// Step two: a machine with no FUSE plays an unpacked copy, which is asked
// for with its size and its place.
void LauncherContext::play_checked(const std::string& id) {
  if (p.no_fuse()) {
    player::UnpackPlan u = p.unpack_plan(id);
    if (!u.ready) {
      consent = true;
      consent_game = id;
      consent_plan = u;
      return;
    }
  }
  play_now(id);
}

// Step three. The game gets its own screen; ours would only be in the way.
void LauncherContext::play_now(const std::string& id) {
  player::PlayOverrides req;
  const PanelSize ps = desktop_size(false);
  req.panel_w = ps.w;
  req.panel_h = ps.h;
  SDL_HideWindow(w.win);
  // A one-game player comes back to its game's page afterwards, as a grid's
  // game does: it started the game without being asked, and this page is the
  // only way to its saves, display and controls. Esc there quits.
  try {
    session::Outcome o = p.play(id, req);
    // A game that died at once is a game that did not start, and the only
    // record of why is the compositor's log.
    if (o.status != 0 && o.seconds < 15) {
      fail(id, p.game(id).name + " stopped as soon as it started (exit status " +
                   std::to_string(o.status) + "). The end of its log:",
           true);
    } else {
      status = "played for " + human_time(o.seconds);
      if (!o.generation.empty()) status += ", snapshot " + o.generation.filename().string();
    }
  } catch (const player::Damaged& ex) {
    fail(id, ex.what(), false);
  } catch (const session::NeedsUnpack&) {
    // The mount failed on a machine the bootstrap thought could mount.
    consent = true;
    consent_game = id;
    consent_plan = p.unpack_plan(id);
  } catch (const std::exception& ex) {
    fail(id, ex.what(), true);
  }
  SDL_ShowWindow(w.win);
  SDL_RaiseWindow(w.win);
  textures.forget_matching("/" + id + "/");
}

void LauncherContext::save_report() {
  std::string text = player::doctor::redact(player::doctor::render(p.doctor_report()));
  fs::path out = state_dir() / "report.txt";
  std::ofstream f(out, std::ios::trunc);
  f << text;
  status = f ? "saved to " + out.string() + ", without your home directory or user name"
             : "could not write " + out.string();
}

// ---- the bundle -------------------------------------------------------------

player::DesktopPaths LauncherContext::desktop_paths() const {
  return player::desktop_paths(p.bundle().meta.id, env_nonempty("XDG_DATA_HOME") ? env_nonempty("XDG_DATA_HOME") : "",
                               env_nonempty("HOME") ? env_nonempty("HOME") : "");
}

void LauncherContext::add_desktop_entry() {
  try {
    player::install_desktop_entry(desktop_paths(), p.bundle().meta.title, p.bundle().meta.id,
                                  p.bundle().self, p.bundle().meta.icon);
    state.desktop_installed = true;
    status = "added to your applications menu";
  } catch (const std::exception& ex) {
    status = ex.what();
  }
  save_state();
}

void LauncherContext::save_state() {
  try {
    player::save_launcher(p.launcher_file(), state);
  } catch (const std::exception& ex) {
    status = ex.what();
  }
}

}  // namespace kg::gui::launcher
