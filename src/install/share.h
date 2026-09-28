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
#include <vector>

#include "../rt/env.h"

namespace kg::install {

// Writes <id>.recipe.kgpack: everything except the game's bytes. Kilobytes.
std::filesystem::path export_recipe(const std::string& id, const std::filesystem::path& out);

// A game on the shelf: its index names a set, and that set holds it.
struct InstalledGame {
  std::string id;
  std::filesystem::path pack;
};
// Every installed game, by id. Each set is opened once however many games it
// holds; an index naming a pack that is missing, will not open or does not
// hold the game is left out, as a game that is not installed.
std::vector<InstalledGame> installed_games();

struct ImportResult {
  // The first game of the set, for the sentences that name one; `ids` is all
  // of them.
  std::string id, name;
  std::vector<std::string> ids;
  bool had_body = false;   // a capsule, so nothing had to be rebuilt
  bool rebuilt = false;    // a recipe, rebuilt from the importer's own disc
  bool root_matched = false;
  std::filesystem::path pack;
};

// A capsule is a whole set. `replace` overwrites the same set when it is
// already installed; the page that offers it says what it would replace first,
// including how much play is recorded against it, because the pack is the
// game and there is no second copy of it anywhere. A game of the set that is
// already installed in another set is refused either way: a game lives in one
// set.
ImportResult import_pack(const rt::Env& e, const std::filesystem::path& in, bool replace,
                         const std::function<void(const std::string&)>& say);

}  // namespace kg::install
