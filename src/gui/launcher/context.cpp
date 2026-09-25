#include "context.h"

#include <SDL.h>

#include <cctype>
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
#include "pages.h"

namespace kg::gui::launcher {
namespace fs = std::filesystem;

LauncherContext::LauncherContext(const player::Player& pl, const Window& win)
    : p(pl), w(win), textures(win.ren), state(player::load_launcher(pl.launcher_file())) {
  for (const bundle::GameMeta& g : p.bundle().meta.games) ids.push_back(g.id);
  selected = ids.front();
}

std::string path_part(const std::string& name) {
  std::string out;
  bool gap = false;
  for (char ch : name) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || c >= 0x80) {
      if (gap && !out.empty()) out += '-';
      gap = false;
      out += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
    } else {
      gap = true;
    }
  }
  return out;
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

const Texture* LauncherContext::cover_backdrop(const std::string& id) {
  const bundle::GameMeta& g = p.game(id);
  if (textures.png("cover:" + id, g.cover)) return textures.backdrop_png("cover:" + id, g.cover);
  return textures.backdrop_file(session::title_art_file(session::journal_dir(id)));
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

// Step three, on the worker, with what the session says as the working
// modal's progress: seeding a prefix and getting it ready take up to a
// minute, and a window that vanished at the click said nothing about it.
// The window goes when the game's screen is up, and comes back after.
void LauncherContext::play_now(const std::string& id) {
  player::PlayOverrides req;
  const PanelSize ps = desktop_size(false);
  req.panel_w = ps.w;
  req.panel_h = ps.h;
  req.usable_w = ps.usable_w;
  req.usable_h = ps.usable_h;
  req.say = [this](const std::string& line) { set_progress(line); };
  req.screen_up = [this] { game_screen_up = true; };
  game_screen_up = false;
  {
    std::lock_guard<std::mutex> lk(mu);
    play_end = PlayEnd{};
  }
  run_job("Starting " + p.game(id).name,
          [this, id, req] {
            PlayEnd end;
            try {
              end.out = p.play(id, req);
              end.kind = PlayEnd::Played;
            } catch (const player::Damaged& ex) {
              end.kind = PlayEnd::Damaged;
              end.what = ex.what();
            } catch (const session::NeedsUnpack&) {
              end.kind = PlayEnd::NeedsUnpack;
            } catch (const std::exception& ex) {
              end.kind = PlayEnd::Failed;
              end.what = ex.what();
            }
            std::lock_guard<std::mutex> lk(mu);
            play_end = std::move(end);
          },
          [this, id](const std::string&) { after_play(id); });
}

void LauncherContext::hide_for_game() {
  if (game_screen_up.exchange(false) && job.running() && !hidden) {
    SDL_HideWindow(w.win);
    hidden = true;
  }
}

// On the UI thread, once the worker is done. A one-game player comes back to
// its game's page afterwards, as a grid's game does: it started the game
// without being asked, and this page is the only way to its saves, display
// and controls. Esc there quits.
void LauncherContext::after_play(const std::string& id) {
  if (hidden) {
    SDL_ShowWindow(w.win);
    SDL_RaiseWindow(w.win);
    hidden = false;
  }
  game_screen_up = false;
  PlayEnd end;
  {
    std::lock_guard<std::mutex> lk(mu);
    end = std::move(play_end);
  }
  switch (end.kind) {
    case PlayEnd::Played: {
      const session::Outcome& o = end.out;
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
      break;
    }
    case PlayEnd::Damaged: fail(id, end.what, false); break;
    case PlayEnd::NeedsUnpack:
      // The mount failed on a machine the bootstrap thought could mount.
      consent = true;
      consent_game = id;
      consent_plan = p.unpack_plan(id);
      break;
    case PlayEnd::Failed: fail(id, end.what, true); break;
  }
  textures.forget_matching("/" + id + "/");
  ++generation;
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
