// Where a player keeps what it keeps, and the few small files it keeps there.
//
//   <state> = <exe-dir>/<exe-name>-data/   if it exists and is writable
//           = $XDG_DATA_HOME/<bundle-id>/  otherwise
//
// Keyed by the bundle id rather than the file's name, so a newer build of the
// same bundle - renamed, moved, downloaded again - finds the saves the old one
// made. The directory beside the executable is how a person carries a player
// and its saves together on a stick; making the directory is the whole of
// asking for it.
//
// A portable directory that cannot be written is the case a person does not
// expect - a stick mounted read-only, a directory copied from somebody else -
// and saves quietly landing somewhere else would be found months later. So it
// falls back to XDG and says so, once: launcher.toml remembers having said it.
// So does one that belongs to another user. A player run from a shared
// directory - /tmp, a download folder on a family machine - takes a -data
// beside it that anyone could have made first, and a state directory decides
// what runs: the game's prefix, its HOME, the unpacked tree it plays from.
//
// With no HOME and no XDG_DATA_HOME there is nowhere of the person's own, and
// the state goes under /tmp/kretro-<uid>. That directory is made private and
// must be ours, as the bootstrap's own /tmp directory is, for the same reason.
#pragma once

#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>

#include "../config/config.h"
#include "../util/hash.h"

namespace kg::player {

struct StateChoice {
  std::filesystem::path dir;
  bool portable = false;
  // The portable directory that was there and was not taken, when that is
  // why `dir` is the XDG one, and why not, to follow its name in a sentence:
  // "cannot be written", say. Empty otherwise.
  std::filesystem::path refused_portable;
  std::string refused_why;
  // Set when `dir` is under /tmp for want of a home: the directory under /tmp
  // that has to be made private, and found to be ours, before it is used.
  std::filesystem::path private_root;
};

// <exe-dir>/<exe-name>-data: "game.run" beside it has "game.run-data".
std::filesystem::path portable_dir(const std::filesystem::path& exe);

// Whether `dir` is a directory this process can create a file in. Asked by
// trying: permission bits say nothing about a read-only mount.
bool writable_dir(const std::filesystem::path& dir);
// Whether a symbolic link can be made in `dir`, by making one. A portable
// directory that cannot is on FAT or exFAT, and a Wine prefix cannot be made
// there: it falls back to XDG and says so, as a read-only one does.
bool holds_links(const std::filesystem::path& dir);

// The rule above. `xdg_data_home` and `home` are the environment's, empty
// when unset; with neither, the state is under /tmp/kretro-<me>. `me` is the
// user a portable directory has to belong to.
StateChoice choose_state(const std::filesystem::path& exe, const std::string& bundle_id,
                         const std::string& xdg_data_home, const std::string& home,
                         uid_t me = getuid());

// Makes `dir` if it is not there, and makes sure it is a real directory that
// belongs to `me` and to nobody else: mode 0700. Throws naming it otherwise.
void claim_private_dir(const std::filesystem::path& dir, uid_t me = getuid());

// Writes `bytes` to a temporary of this writer's own beside `file` and renames
// it over, so a crash leaves the old file and two writers at once each leave
// a whole one. Throws std::runtime_error naming the file.
void write_atomically(const std::filesystem::path& file, const std::string& bytes);

// launcher.toml: what the launcher remembers between runs.
struct LauncherState {
  uint32_t window_w = 1280, window_h = 800;
  bool window_fullscreen = false;
  bool desktop_offered = false;     // "Add to applications menu" asked once
  bool desktop_installed = false;
  bool portable_fallback_said = false;
  // warning_key of each warning already shown. A hash rather than the text,
  // because the text is a stranger's machine talking - paths, device names -
  // and this file is read back by a TOML reader that is not a general one.
  std::set<std::string> warnings_seen;
};

std::string warning_key(const std::string& line);

LauncherState load_launcher(const std::filesystem::path& file);
// Written to a temporary and renamed, so a crash cannot leave half of it.
void save_launcher(const std::filesystem::path& file, const LauncherState& s);

// <state>/<game>/settings.toml: how one game is shown and played. Absent
// fields come from the author's defaults in bundle.meta.
struct GameSettings {
  config::Display display;
  bool fullscreen = false;
  // "author" is the map the bundle's author set, over kretro's own; "kretro"
  // is kretro's own alone.
  std::string gamepad = "author";
};

GameSettings default_settings(const std::string& author_display, bool author_fullscreen);
GameSettings load_game_settings(const std::filesystem::path& file, const GameSettings& defaults);
void save_game_settings(const std::filesystem::path& file, const GameSettings& s);

// Which packs have already been checked against their BLAKE3, per bundle
// version. Hashing a pack reads every byte of it, gigabytes for a game that
// carries its disc; doing it on every launch would make the second launch as
// slow as the first. The entry's hash is part of the key, so a rebuilt pack
// under the same version string is still checked.
bool pack_verified(const std::filesystem::path& memo, const std::string& game,
                   const std::string& version, const Hash& h);
void remember_verified(const std::filesystem::path& memo, const std::string& game,
                       const std::string& version, const Hash& h);

// A TOML basic string: quoted, with its backslashes and quotes escaped.
std::string toml_string(const std::string& s);

}  // namespace kg::player
