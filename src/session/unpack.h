// Playing a game out of an unpacked copy of its pack's body, where it cannot
// be mounted: unpacking it, and the stamp that says which pack a copy is of.
#pragma once

#include <filesystem>
#include <string>

#include "../pack/kgpack.h"

namespace kg::session {

// Unpacks the pack's whole body into `dir` and stamps it, as the no-FUSE path
// does on its own. The body is first copied out to a file of its own beside
// `dir` when the pack sits inside a larger file: dwarfsextract has no image
// size option and reads whatever follows the image as more of it.
//
// `dir` is replaced whole, so one that holds anything no unpack put there is
// refused: it may be a directory a person named.
void unpack_body(const Pack& pack, const std::filesystem::path& dir);
// Whether an unpack of game `id` may replace `dir`: nothing is there, or an
// empty directory, or a tree with an extraction stamp beside it, or the
// game's own cache directory.
bool may_unpack_into(const std::filesystem::path& dir, const std::string& id);
// Where the stamp for an unpacked tree lives: beside it, never inside it.
std::filesystem::path extraction_stamp_file(const std::filesystem::path& dir);

// The no-FUSE path unpacks the body once and plays out of the unpacked tree.
// The stamp is what tells one unpacked tree from another: the body's layout,
// the body's hash and the tree's Merkle root, on one line.
//
// Without it the cache is keyed on the game's id alone, and an id is the one
// thing a rebuild does not change. Reinstalling a game - or installing it under
// a build that lays the body out differently - would then be played from the
// bytes of the install before it, and a flat tree reached through a rooted
// path, or the reverse, is not reached at all: the game directory silently does
// not exist and the exe is reported missing from a pack that is perfectly good.
//
// The stamp lives beside the unpacked tree, at extracted/<id>.stamp, and never
// inside it. Inside, it is a file the game did not write sitting in the very
// tree that is diffed against meta.tree at exit, and every session would report
// it as written and cut a snapshot generation containing nothing else.
std::string extraction_stamp(const Meta& m, const Header& h);
bool extraction_stamp_matches(const std::filesystem::path& stamp_file, const std::string& stamp);
void write_extraction_stamp(const std::filesystem::path& stamp_file, const std::string& stamp);

}  // namespace kg::session
