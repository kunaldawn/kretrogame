// Building a player from a draft: the file's name, bundle.meta, the inputs a
// build takes, and the build the page's Build button and `kretro bundle build`
// both run.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../../install/keys.h"
#include "../build.h"
#include "../meta.h"
#include "../progress.h"
#include "draft.h"

namespace kg::bundle {

// ---- building ----------------------------------------------------------------------

// <id>-<version>.run
std::string output_name(const Draft& d);

// bundle.meta, from the draft and the vault. `keys` is the vault's contents;
// a game's key is read from it only when the author asked for it embedded.
// `exe64` says, per game id, whether its executable is 64-bit, which decides
// the registry view an embedded key is written for; a game not in it is taken
// as 32-bit, which every game of this era is.
BundleMeta meta_from_draft(const Draft& d, const std::vector<install::StoredKey>& keys,
                           const std::string& built_at, const std::vector<std::string>& licenses,
                           const std::map<std::string, bool>& exe64 = {});

struct BuildInputs {
  std::filesystem::path self;        // kretro's own file
  std::filesystem::path games_dir;   // the shelf
  std::filesystem::path keys_file;   // the vault
  // dwarfs-universal: reads the base's licence list and a keyed game's
  // executable. May be empty; the licence list is then empty and a key is
  // written for the 32-bit view.
  std::filesystem::path tool;
  std::filesystem::path cache;       // where base_licenses remembers; scratch goes under it
  // A player base file to build from instead of the one kretro carries: the
  // command line's --base, for a base just linked by make player-base.
  std::filesystem::path base;
  // The file itself, in place of <out_dir>/<id>-<version>.run: the command
  // line's -o FILE. Written there directly, so build_bundle's refusal to
  // write over the base or a pack covers it, and no file of that other name
  // in the folder is replaced on the way.
  std::filesystem::path out;
};

// The inputs a build from kretro's own shelf takes: KRETRO_SELF, the shelf,
// the vault, KRETRO_DWARFS and the cache, with `base` as --base gave it.
// Either variable, when unset, is an empty path.
BuildInputs shelf_build_inputs(const std::filesystem::path& base = {});

// Everything the page's Build button does: the base out of kretro, the packs
// off the shelf in draft order, bundle.meta, build_bundle into
// <out_dir>/<id>-<version>.run.partial, verified and renamed. Throws as
// build_bundle does, and leaves no .partial behind.
Built build_from_draft(const Draft& d, const BuildInputs& in, const Callbacks& cb = {});

}  // namespace kg::bundle
