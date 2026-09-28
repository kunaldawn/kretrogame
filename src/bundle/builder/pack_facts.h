// What a pack on the shelf says, as the Bundles page needs it: its metadata
// and sizes, the game it starts a draft with, and, for a whole draft, each
// game's pack and the vault's key for it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../../pack/kgpack.h"
#include "checks.h"
#include "draft.h"

namespace kg::bundle {

// ---- what a pack on the shelf says ----------------------------------------------

struct PackFacts {
  std::filesystem::path path;   // the set the game is in
  Meta meta;                    // the game
  uint64_t bytes = 0;           // the .kgpack as it sits on the shelf: the whole set
  std::string set_id;
  size_t set_games = 1;         // how many games share the set
};

// Reads the pack's header and game `id`'s metadata; the body is not touched.
// Throws when the pack does not hold the game.
PackFacts read_pack_facts(const std::filesystem::path& pack, const std::string& id);

// A game as the page first shows it: name and year from the pack, the cover
// from `cover_png` when one is given (the shelf's title screenshot).
DraftGame game_from_pack(const PackFacts& f, const std::filesystem::path& cover_png = {});

// The set a game is in, on the shelf in state directory `state`: the one
// games/<id>.set names, under packs/. Empty when there is no index, the index
// names something that cannot be a set, or the set is not there.
std::filesystem::path find_pack_on_shelf(const std::filesystem::path& state, const std::string& id);

// What is known about each game of a draft, off the shelf: its pack, and the
// vault's key for it, in draft order. games[i].pack points into packs, or is
// null for a game with no readable pack, so the two travel together: copying
// is deleted because a copy's games would point into the original's packs,
// and a move keeps the vector's buffer and so every pointer into it.
struct DraftFacts {
  std::vector<PackFacts> packs;
  std::vector<GameFacts> games;

  DraftFacts() = default;
  DraftFacts(const DraftFacts&) = delete;
  DraftFacts& operator=(const DraftFacts&) = delete;
  DraftFacts(DraftFacts&&) = default;
  DraftFacts& operator=(DraftFacts&&) = default;
};

// Reads each draft game's pack from the shelf and its key from the vault. The
// executables are read too when `read_imports` is set, which is what the
// Glide checks need; it takes a dwarfsextract per game.
DraftFacts gather_draft_facts(const Draft& d, bool read_imports);

}  // namespace kg::bundle
