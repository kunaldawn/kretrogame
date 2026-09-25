// What every page of a player's launcher shares: the player and its window,
// what launcher.toml remembers, the games and the one being looked at, the
// first-run check and what it found, the one worker a long job runs on, and
// the way from one screen to another.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "../../player/desktop.h"
#include "../../player/player.h"
#include "../job.h"
#include "../texture.h"
#include "../window.h"

namespace kg::gui::launcher {

// The launcher's screens.
enum class Screen { Checking, Blocked, Grid, Game, Saves, Display, Controls, Bundle, Licenses, About };

struct LauncherContext {
  // `w` is the Window local to run_launcher, which this must not outlive.
  LauncherContext(const player::Player& p, const Window& w);
  LauncherContext(const LauncherContext&) = delete;
  LauncherContext& operator=(const LauncherContext&) = delete;

  const player::Player& p;
  const Window& w;
  Textures textures;
  player::LauncherState state;
  std::vector<std::string> ids;
  // The game the Game, Saves, Display and Controls screens are about.
  std::string selected;
  bool quit = false;
  std::string status;
  // Puts a screen up. The host's router; a page calls it rather than knowing
  // which pages there are.
  std::function<void(Screen)> go;

  // The first-run check, and what came of it.
  player::doctor::Report report;
  bool checked = false;
  std::vector<std::string> blocking, notes;
  bool offer_desktop = false;
  bool auto_play = false;

  // Asking before unpacking a game on a machine with no FUSE.
  bool consent = false;
  std::string consent_game;
  player::UnpackPlan consent_plan;

  // The last game that would not play, and why.
  std::string failure_game, failure, failure_log;

  // ---- the worker ---------------------------------------------------------
  //
  // Checking the machine, hashing a pack and unpacking one take seconds to
  // minutes; they run here while the window keeps drawing. `then` runs on the
  // UI thread once the job is over, with what it threw, if anything.
  void run_job(const std::string& title, std::function<void()> fn,
               std::function<void(const std::string& error)> next);
  // The continuation is taken out of `then` before it runs: it may start the
  // next job, which sets `then` again, while it is still running.
  void finish_job();
  void set_progress(const std::string& s);
  // What the running job has got to, copied under the lock.
  std::string progress_text();

  Job job;
  // What the running job has got to, set from its thread under mu.
  std::mutex mu;
  std::string progress;
  std::function<void(const std::string&)> then;

  // ---- first run (first_run.cpp) ------------------------------------------
  void begin_check();
  void maybe_auto_play();

  // ---- playing ------------------------------------------------------------
  // Counts the sessions played, so a page keeping a copy of a game's journal
  // or snapshots reads it again when this changes.
  unsigned generation = 0;
  const Texture* cover(const std::string& id);
  // The same picture blurred, for the backdrop of the game's hero band.
  const Texture* cover_backdrop(const std::string& id);
  void play(const std::string& id);
  void play_checked(const std::string& id);
  void play_now(const std::string& id);
  // Called every frame by the host: hides the window once the session says
  // the game's screen is up. SDL's window calls are the UI thread's to make.
  void hide_for_game();
  void after_play(const std::string& id);
  void fail(const std::string& id, const std::string& what, bool with_log);
  void save_report();

  // How the session on the worker ended, set from its thread under mu. The
  // exception itself cannot cross to this thread with its type, and which
  // one it was decides what the launcher does next.
  struct PlayEnd {
    enum Kind { Played, Damaged, NeedsUnpack, Failed } kind = Failed;
    std::string what;
    session::Outcome out;
  };
  PlayEnd play_end;
  std::atomic<bool> game_screen_up{false};
  bool hidden = false;

  // ---- the bundle ---------------------------------------------------------
  player::DesktopPaths desktop_paths() const;
  void add_desktop_entry();
  void save_state();
};

}  // namespace kg::gui::launcher
