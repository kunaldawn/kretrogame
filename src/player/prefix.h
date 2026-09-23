// A game's Wine prefix, in a player.
//
// The runtime carries a prefix already made: wineboot ran when the runtime
// was built, with mono and gecko kept out, and the result was stamped with the
// Wine version that made it (.kretro-wine-version) and shipped read-only at
// /opt/kretro/prefix-template. A game's first launch copies it to
// <state>/<game>/prefix - a minute of wineboot becomes a second of copying -
// and makes the copy the game's own:
//
//   - dosdevices is rebuilt: c: and nothing else. The template's z: to / is
//     the one thing a player's prefix must not have; a Windows program has no
//     business seeing the whole of somebody's machine. The discs and the
//     game's own drive are added by the session, each time, as today.
//   - the stamp comes along, and is what says which Wine made this prefix.
//
// A newer player of the same bundle can carry a newer Wine. Its template's
// stamp then differs from the prefix's, and the prefix is upgraded in place
// with wineboot -u - never re-seeded, because games of this era keep saves and
// settings under drive_c/users and in the registry, and re-seeding would take
// both. What wineboot -u may touch is snapshotted first, as a restore
// snapshots the saves before it replaces them.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../bundle/meta.h"
#include "../pack/kgpack.h"
#include "../rt/env.h"

namespace kg::player {

enum class PrefixAction {
  Seed,     // no prefix yet: copy the template
  Upgrade,  // made by another Wine: snapshot, then wineboot -u in place
  Ready,    // made by this Wine, or nothing to compare it with
};
const char* prefix_action_name(PrefixAction a);

// The decision, from the two stamps. A prefix that exists but has no stamp
// was made some other way (kretro's own wineboot --init) and is left alone,
// as is any prefix when the runtime carries no template to compare with.
PrefixAction prefix_action(const std::optional<std::string>& template_stamp,
                           const std::optional<std::string>& prefix_stamp, bool prefix_exists);

// The first line of <dir>/.kretro-wine-version, or nothing.
std::optional<std::string> read_stamp(const std::filesystem::path& dir);

// <runtime>/opt/kretro/prefix-template, or empty when the runtime has none.
std::filesystem::path template_dir(const rt::Env& e);

// Copies the template to `prefix`, cloning each file where the filesystem can
// (reflink) and copying it where it cannot, then rebuilds dosdevices with c:
// alone. The copy is assembled beside `prefix` and renamed into place, so a
// seed interrupted halfway is not mistaken for a prefix.
void seed_prefix(const std::filesystem::path& tmpl, const std::filesystem::path& prefix);

// Copies what a game may have written in its prefix - the registry hives and
// drive_c/users - to a new directory under `into`, named for the Wine that
// made them and when. Returns that directory. Throws when the copy did not
// complete, having removed what it got.
std::filesystem::path snapshot_prefix(const std::filesystem::path& prefix,
                                      const std::filesystem::path& into);

// What ensure_prefix did, for the plan and the page.
struct PrefixResult {
  PrefixAction action = PrefixAction::Ready;
  std::filesystem::path snapshot;  // set when an upgrade snapshotted first
};

// Seeds, upgrades or leaves the prefix, as prefix_action says. `snapshots` is
// where an upgrade's snapshot goes.
PrefixResult ensure_prefix(const rt::Env& e, const std::filesystem::path& prefix,
                           const std::filesystem::path& snapshots,
                           const std::function<void(const std::string&)>& say);

// The REGEDIT4 text that puts an author's embedded key where the game reads
// it. `registry_path` may name its hive in full or as HKLM/HKCU/HKU/HKCR;
// no hive at all means HKEY_LOCAL_MACHINE, where the installers of this era
// kept their keys. A key whose view is "32" and whose path is under
// HKLM\Software is written under HKLM\Software\Wow6432Node instead, which is
// where a 32-bit game in a 64-bit prefix reads it. Throws std::runtime_error
// when there is no path or no value name to put it at.
std::string key_registry(const bundle::GameMeta::Key& k);

// Writes the author's extra files into `dir`, each only when it is not already
// there with the same bytes. Returns the DLL overrides that make Wine load the
// DLLs among them before its own ("ddraw=n,b;d3d8=n,b"), empty when there are
// none.
std::string place_extra_files(const std::vector<bundle::GameMeta::Dll>& files,
                              const std::filesystem::path& dir);

// The directory the game's executable is in, relative to the game's root and
// spelled as the pack's tree spells it: "" when it is at the root, "Bin" for
// Bin\game.exe. run.exe was typed by a person or an installer and Wine does
// not care about case; the directory an extra file lands in has to be the one
// the tree really has, or it is a second directory beside it.
std::string exe_dir_in_tree(const Meta& m);

// The author's extra files, laid out as a layer over the game: at
// <layer>/<exe_dir>/<name>, beside the executable, whatever else the layer
// held removed. The session puts the layer over the game read-only - a lower
// layer above the pack's in the overlay - so the files are where the game
// looks and are never in its writable layer: not in the saves, a snapshot or
// a saves export, and not something the game "wrote" every session. Returns
// place_extra_files' overrides.
std::string stage_extra_layer(const std::vector<bundle::GameMeta::Dll>& files, const std::string& exe_dir,
                              const std::filesystem::path& layer);

}  // namespace kg::player
