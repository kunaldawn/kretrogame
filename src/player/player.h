// The player: one file, its games, and what playing one of them takes.
//
// This is everything the player's command line and its launcher have in
// common, so each is only a way of asking. It reads the file it was started
// from - KRETRO_SELF, whose table of contents the bootstrap has already
// checked - decides where its state goes before anything else can ask, and
// then, per game:
//
//   1. checks the pack against its entry's BLAKE3 the first time this version
//      of the bundle plays it, and remembers that it did;
//   2. mounts it in place, at the player's offset of the pack plus the pack's
//      own offset of its body, with a capped DwarFS cache - or, on a machine
//      the bootstrap found cannot mount, plays the copy unpacked with consent;
//   3. makes the game's prefix from the runtime's template, or upgrades it;
//   4. hands the rest to session::play (session/play.cpp), with the backend
//      chosen from the author's setting, the game's imports and this machine.
//
// What it never does: use the network, install anything, need root, or write
// anywhere but its state, its cache, and - asked first - the two desktop
// entry files.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../bundle/meta.h"
#include "../bundle/toc.h"
#include "../gpu/caps.h"
#include "../gpu/probe.h"
#include "../pack/kgpack.h"
#include "../rt/env.h"
#include "../session/lock.h"
#include "../session/play.h"
#include "../session/unpack.h"
#include "bundle_file.h"
#include "doctor.h"
#include "prefix.h"
#include "settings.h"
#include "state_dir.h"
#include "unpack.h"
#include "verify_memo.h"

namespace kg::player {

// The DwarFS block cache a player gives each game's mount. DwarFS's own
// default is half a gigabyte; a player runs on machines nobody measured, and
// the page cache does the same job for a game that rereads a file.
inline constexpr const char* kPackCacheSize = "128m";

// Where the source of everything in the runtime is. A kretro release mirrors
// every tarball SOURCES.txt names beside itself.
inline constexpr const char* kSourceNote =
    "The source of every LGPL and GPL part of this runtime is published beside the kretro "
    "release that built this player, as the exact tarballs SOURCES.txt names. SOURCES.txt, in "
    "the list below, also says where each part's upstream source lives.";

struct PlayOverrides {
  bool dry_run = false;
  uint32_t panel_w = 0, panel_h = 0;
  bool fullscreen_set = false, fullscreen = false;
};

class Player {
 public:
  Player(Bundle b, rt::Env env, StateChoice st, gpu::Report gpu);

  const Bundle& bundle() const { return b_; }
  const StateChoice& state() const { return st_; }
  const rt::Env& env() const { return env_; }
  bool no_fuse() const;

  std::filesystem::path launcher_file() const;
  std::filesystem::path settings_file(const std::string& id) const;
  GameSettings settings(const std::string& id) const;
  void save_settings(const std::string& id, const GameSettings& s) const;

  // The table's entry for a game, and the game's meta; throws when the file
  // carries no such game, naming the ones it does.
  const bundle::Entry& entry(const std::string& id) const;
  const bundle::GameMeta& game(const std::string& id) const;
  Pack open_pack(const std::string& id) const;

  // Step 1. Throws Damaged. `progress` gets bytes done and total, may be empty.
  void verify(const std::string& id,
              const std::function<void(uint64_t, uint64_t)>& progress = {}) const;
  bool verified(const std::string& id) const;

  UnpackPlan unpack_plan(const std::string& id) const;
  // Unpacks the game into `dir`, or where unpack_plan says when empty, and
  // remembers where. Checks the free space first.
  std::filesystem::path unpack(const std::string& id, const std::filesystem::path& dir = {}) const;

  // The machine, probed once: the doctor's inputs and what the backend policy
  // decides on. Takes a second or two - vulkaninfo and glxinfo run.
  const doctor::Inputs& machine() const;
  doctor::Report doctor_report() const;

  // Steps 2 to 4. Throws session::NeedsUnpack when the game must be unpacked
  // first and has not been; Damaged; or std::runtime_error with the reason,
  // refusals included.
  session::Outcome play(const std::string& id, const PlayOverrides& ov) const;

  std::filesystem::path export_saves(const std::string& id, const std::filesystem::path& out) const;
  void import_saves(const std::string& id, const std::filesystem::path& in) const;

  // The last session's log, cut to its last lines.
  std::string last_log(size_t lines) const;

  // The notices the runtime carries, and where the source is.
  std::string licenses_text() const;
  std::vector<std::filesystem::path> license_files() const;

 private:
  std::filesystem::path unpack_pointer(const std::string& id) const;
  std::filesystem::path memo_file() const;

  Bundle b_;
  rt::Env env_;
  StateChoice st_;
  gpu::Report gpu_;
  mutable std::optional<doctor::Inputs> machine_;
};

}  // namespace kg::player
