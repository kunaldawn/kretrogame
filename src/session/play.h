// Playing a game, and remembering that you did.
//
// The game directory the game sees is a read-only DwarFS base with a writable
// layer on top, so every file the game touches is isolated by the filesystem
// rather than by a per-game list of save paths that somebody has to maintain.
// That single fact is what makes saves visible, snapshottable and exportable.
//
// Where FUSE is unavailable the base is extracted instead and the same answer
// is computed at exit by comparing the tree against the manifest. Slower,
// identical result, works everywhere.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../backend/policy.h"
#include "../config/scaling.h"
#include "../pack/tree.h"
#include "../rt/env.h"
#include "layers.h"

namespace kg::session {

class GameLock;

// Who sets what. The kretro CLI's play sets the id, the display flags, the
// panel it asked SDL for, dgvoodoo, record, note and dry_run; its compare
// sets the id and, per run, wined3d_renderer, stop_after and capture_to. The
// shelf sets only the id. The player sets the id, the source, held_lock, the
// game's own display settings, fullscreen, the panel, dry_run, the for_exe
// backend decision, game_drive and after_prefix. The tests set the id, the
// source, dry_run and a fixed backend.

// How this session should reach the panel.
struct DisplayOptions {
  // Unset, the setting for this game is read from config.toml, so the shelf
  // and the terminal agree.
  std::optional<config::Display> scaling;
  // The panel this session will cover, from whoever knows: the shelf has a
  // window, the terminal asks SDL once. Zero means unknown, and then nothing
  // is assumed.
  uint32_t panel_w = 0, panel_h = 0;
  bool fullscreen = false;  // ORed with the geometry's own fullscreen
};

// How this game is drawn and shown on this machine.
struct BackendSettings {
  // As backend::plan decided it: DXVK or WineD3D or cnc-ddraw, the Wine
  // settings each implies, and whether to skip the nested compositor under
  // gamescope. Unset, a session runs exactly as it always has. A refusal is
  // thrown before anything else.
  std::optional<backend::Plan> fixed;

  // The same decision, made once the game's executable can be read: a player
  // decides "auto" from the exe's imports, and the exe is only there once the
  // pack is mounted. Used when `fixed` is unset. A refusal it returns is
  // thrown before anything is started; a caller that can refuse without the
  // exe - no GPU, and the author said the game needs one - should do so
  // before calling play, so nothing is mounted to be told no.
  std::function<backend::Plan(const std::filesystem::path& exe)> for_exe;

  // Used by the renderer bake-off: run one backend for a fixed time and keep
  // the frame, so a person can look at the results side by side instead of
  // guessing which one is right. The wined3d backend (gl, vulkan, gdi),
  // applied last of the prefix writes.
  std::string wined3d_renderer;

  bool dgvoodoo = false;  // ORed with Meta.runtime.dgvoodoo
};

struct Hooks {
  // Called once the prefix is ready - registry imported, discs attached - and
  // before the backend and the game. A player applies its author's embedded
  // key and the overrides for its extra DLLs here (the files themselves are
  // Source::extra_layer). `wine_env` is the environment Wine will run under
  // and may be added to; `game` is the directory the game runs in.
  std::function<void(rt::Env& wine_env, const std::filesystem::path& prefix,
                     const std::filesystem::path& game)>
      after_prefix;
};

struct PlayRequest {
  std::string id;

  // The pack is not state/games/<id>.kgpack but this.
  std::optional<Source> source;

  // Set when the caller already holds lock_game(id), and holds it until play
  // returns: a player takes it before it touches the game's prefix and extra
  // layer, and letting it go for play to take again would let another copy
  // in between. play does not take it a second time - a second flock of the
  // same file from this process would be refused as the other copy's.
  // Non-owning; the caller keeps the lock alive until play returns.
  const GameLock* held_lock = nullptr;

  // Give the game directory a drive letter of its own and start the game
  // from it. Zero is kretro's way: the game is reached through Z:, which maps
  // /. A player's prefix has no Z: - a Windows program has no business seeing
  // the whole of somebody's machine - and a game whose working directory is
  // on no drive at all is started in C:\windows by Wine, which most of these
  // games take as their data being missing.
  char game_drive = 0;

  // Keep the frames, the snapshot and the journal entry of this session.
  bool record = true;
  std::string note;  // recorded against this session

  int stop_after = 0;                // seconds; 0 plays until you quit
  std::filesystem::path capture_to;  // copy the last frame here

  // Everything up to the game: lock, mount, prefix, registry, discs, backend.
  // Then the plan is put in Outcome::plan and nothing is started. The mounts
  // are released on the way out as they would be after a game.
  bool dry_run = false;

  DisplayOptions display;
  BackendSettings backend;
  Hooks hooks;
};

struct Outcome {
  int status = 0;
  double seconds = 0;
  Tree::Diff diff;
  std::filesystem::path generation;
  bool journal_written = false;
  // What was decided, in order, as label and value: filled on every run, and
  // the whole of the result of a dry run.
  std::vector<std::pair<std::string, std::string>> plan;
};

Outcome play(const rt::Env& e, const PlayRequest& req);

}  // namespace kg::session
