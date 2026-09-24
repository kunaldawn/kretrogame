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
  std::filesystem::path path;
  Meta meta;
  uint64_t bytes = 0;           // the .kgpack as it sits on the shelf
  // Roughly what the pack would be without its discs: its bytes shared out
  // between the game and the discs by their unpacked sizes. The body is one
  // compressed image and does not say how much of it each directory is, so
  // this is an estimate and the page says "about".
  uint64_t without_discs = 0;
  size_t discs_carried = 0;
  size_t discs_named = 0;       // named by the pack but not inside it
};

// Reads the pack's header and metadata; the body is not touched.
PackFacts read_pack_facts(const std::filesystem::path& pack);

// A game as the page first shows it: name and year from the pack, the cover
// from `cover_png` when one is given (the shelf's title screenshot).
DraftGame game_from_pack(const PackFacts& f, const std::filesystem::path& cover_png = {});

// The shelf keeps a game as <id>.kgpack; a pack copied in by hand may be named
// anything, and is found by the id it says it is. Empty when there is none.
std::filesystem::path find_pack_on_shelf(const std::filesystem::path& games_dir, const std::string& id);

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
