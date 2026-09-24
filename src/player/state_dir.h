// Where a player keeps what it keeps.
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

#include <filesystem>
#include <string>

#include "bundle_file.h"

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

// Decides the state directory and puts it where every later path lookup will
// find it: KRETRO_STATE, and the bundle layout. Called before anything else
// asks where anything is; paths resolve state_dir() once.
StateChoice settle_state(const Bundle& b, const std::filesystem::path& exe);

// The same, for the helper a session starts beside a game - the gamepad's.
// It is this program again, exec'd straight from the runtime rather than
// through the bootstrap, with the game's environment: HOME is the game's own
// home by then, and choosing afresh would find a state under it. The player
// that started the game handed its choice down as KRETRO_STATE, and that is
// the one taken; without it, this is settle_state.
StateChoice inherited_state(const Bundle& b, const std::filesystem::path& exe);

}  // namespace kg::player
