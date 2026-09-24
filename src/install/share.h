// Moving a game between machines, and sharing one with a friend.
//
// A capsule carries the bytes; a recipe carries only the knowledge - the disc
// fingerprint, the extraction recipe, the per-file hashes and the Merkle root.
// Because the root is the root of the *tree* rather than of the packed body,
// a game rebuilt from your own disc can be proven identical to the one the
// recipe was made from.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "../rt/env.h"

namespace kg::install {

// Writes <id>.recipe.kgpack: everything except the game's bytes. Kilobytes.
std::filesystem::path export_recipe(const std::string& id, const std::filesystem::path& out);

struct ImportResult {
  std::string id, name;
  bool had_body = false;   // a capsule, so nothing had to be rebuilt
  bool rebuilt = false;    // a recipe, rebuilt from the importer's own disc
  bool root_matched = false;
  std::filesystem::path pack;
};

// `replace` overwrites a game of the same id that is already installed. The
// page that offers it says what it would replace first, including how much
// play is recorded against it, because the pack is the game and there is no
// second copy of it anywhere.
ImportResult import_pack(const rt::Env& e, const std::filesystem::path& in, bool replace,
                         const std::function<void(const std::string&)>& say);

}  // namespace kg::install
