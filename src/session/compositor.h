// Running one Windows program inside our own Weston and Xwayland: what
// playing a game and running an installer have in common.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "../rt/env.h"

namespace kg::session {

// Puts `frame` in as the game's tile art, at frames/title.png, if it is this
// run's to take. Returns whether it did.
//
// Write-once: the first stable frame of a session becomes the tile and every
// frame after it leaves the tile alone, because a tile that changed every
// session would not be a tile. `provisional` is the one exception. An install
// photographs the installer rather than the game, and write-once meant an
// InstallShield dialog was a game's tile for as long as the game was
// installed; so the install marks what it leaves as a stand-in - still better
// than a coloured rectangle for a game not yet played - and the first frame of
// the first real play takes the tile back and removes the mark.
//
// The mark is a file beside the tile rather than a field in the journal: the
// journal is one JSON file per session and this outlives all of them.
bool set_title_art(const std::filesystem::path& frames, const std::filesystem::path& frame,
                   bool provisional);

struct CompositorOptions {
  uint32_t width = 800;
  uint32_t height = 600;
  uint32_t scale = 1;
  bool fullscreen = false;
  std::string socket_suffix;              // distinguishes concurrent sessions
  std::filesystem::path home;             // where weston.ini and the logs go
  std::filesystem::path capture_dir;      // where harvested frames go
  bool capture = true;
  int first_capture_after = 15;           // seconds before the first frame
  bool keep_every_frame = false;          // an install keeps them all, for evidence
  // The first stable frame becomes title.png, the game's tile on the shelf,
  // and then stays: a tile that changed every session would not be a tile.
  //
  // An install is the one run where that frame is not of the game. It is of
  // InstallShield, and write-once meant an InstallShield dialog was the tile
  // for as long as the game was installed. So the install marks what it leaves
  // as a stand-in - better than a coloured rectangle for a game not yet played
  // - and the first frame of the first real play takes the tile back.
  bool title_is_provisional = false;
  bool pause_on_blur = true;
  int stop_after = 0;                     // seconds; 0 waits for the program
  // Some Windows programs are stubs: InstallShield's setup.exe extracts its
  // engine, launches it and exits within seconds. Waiting only for the process
  // we launched would tear the compositor down on top of the real installer.
  bool wait_for_processes = false;         // wait until the prefix is idle

  // Weston with no output of its own. The nested screen still exists, is still
  // drawn into and can still be read - it is simply never presented to the
  // host. This is what lets an installer be a panel inside our own window
  // instead of a second window somebody has to go and find.
  bool headless = false;

  // Fired once, from inside run_in_compositor, the moment Xwayland answers on
  // its socket. That is the earliest instant anything else may open this
  // display, and it is the whole of the coupling between the engine and the
  // stage: the caller's UI thread opens its own connection to this string. An
  // Xlib display is not shared across threads here; each side has one.
  std::function<void(const std::string& display)> on_display_ready;

  // `kretro input` translates a gamepad into XTest events on this display.
  // During an install that is one writer too many: the GUI process is already
  // holding the pad, and the stage is already injecting into that same
  // display, so the two would fight over one pointer. The install path passes
  // false; playing a game leaves it alone.
  bool input_helper = true;

  // The process group the launched program was put in, reported as soon as it
  // exists. An installer waits for a person, so "abandon this install" cannot
  // wait for the installer: it kills this group, the waitpid below returns,
  // and the guards tear the compositor down on the way out.
  //
  // Setting this is also what asks for that group. Unset - which is every
  // caller but the install - the program stays in kretro's own process group,
  // and that is not a detail: a game in a group of its own is not in the
  // terminal's foreground group, so Ctrl-C during `kretro play` would reach
  // kretro and never the game. The two cannot disagree because there is one
  // field, not two.
  std::function<void(pid_t)> on_pgid;
};

struct CompositorResult {
  int status = -1;
};

// Runs one Windows program inside our own Weston and Xwayland, and waits for
// it. This is what playing a game and running an installer have in common, and
// it is why an installer run gets screenshots and pause-on-blur for free.
CompositorResult run_in_compositor(const rt::Env& e, const rt::Env& wine_env,
                                   const std::filesystem::path& prog,
                                   const std::vector<std::string>& args,
                                   const std::filesystem::path& cwd,
                                   const CompositorOptions& opt,
                                   const std::function<void(const std::string&)>& say);

}  // namespace kg::session
