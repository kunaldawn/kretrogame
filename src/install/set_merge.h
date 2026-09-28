// Where a newly installed game goes on the shelf: a pack is a media set - every
// disc once and every game installed from them - so a game from a disc that is
// already on the shelf joins the set that carries it, and a game whose discs
// span several sets folds them into one.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "../disc/disc.h"
#include "../pack/pack_meta.h"
#include "../session/lock.h"

namespace kg::install {

// A set on the shelf, as far as planning needs it: where it is and what it
// holds. Nothing of its body is read.
struct ShelfSet {
  std::filesystem::path path;
  SetMeta meta;
};

// Every set in packs_dir() that opens; one that does not is left out, as a
// set that is not there.
std::vector<ShelfSet> shelf_sets();

struct MergePlan {
  std::string set_id;        // the set the game ends up in
  std::vector<size_t> fold;  // the sets given that go into it, smallest id first
  // The set as it will be, body empty: its games sorted by id, each disc once,
  // and no disc that no game of it uses.
  SetMeta meta;
  // The game's discs that no folded set carries: these have to be read off
  // their images, and the rest come out of the folded bodies.
  std::vector<std::string> new_disc_keys;
};

// Where `game` goes among `sets`. The game carries its discs, keyed, in
// Meta::discs. A set is folded when it shares a disc with the game or already
// holds a game of that id; with none, the game gets a set of its own, named
// after its discs. Reads nothing.
MergePlan plan_merge(const std::vector<ShelfSet>& sets, const Meta& game);

// For each disc, whether a set on the shelf already carries it: the build page
// counts those as stored already, because the game will join that set.
std::vector<bool> discs_on_shelf(const std::vector<disc::Disc>& discs);

// The lock every writer of the shelf holds - an install, an import - from
// reading the sets to writing the last index. Throws, saying so, when another
// holds it.
session::GameLock lock_shelf();

}  // namespace kg::install
