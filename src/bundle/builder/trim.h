// A set cut down to the games a player carries. A disc holding three games is
// one pack on the shelf; a player of one of them carries that game and the
// discs it uses, not the other two.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "../../pack/pack_meta.h"
#include "../progress.h"

namespace kg::bundle {

// The set's metadata with only the games in `keep` and the discs they use, in
// the set's order. The set id and the disc keys are kept: nothing is renamed.
// Throws naming a game the set does not hold.
SetMeta trim_meta(const SetMeta& s, const std::vector<std::string>& keep);

// A pack holding only `keep` of the set at `pack`: the pack itself when `keep`
// is all of its games, and otherwise a trimmed copy under <cache>/trim/, made
// once and handed back again while the set it was cut from is unchanged. It is
// made by unpacking the body with `tool`, leaving out what is not kept and
// packing the rest again. Throws std::runtime_error or Cancelled, and leaves no
// half-made pack or scratch behind.
std::filesystem::path trimmed_set(const std::filesystem::path& pack, const std::vector<std::string>& keep,
                                  const std::filesystem::path& tool, const std::filesystem::path& cache,
                                  const Callbacks& cb = {});

}  // namespace kg::bundle
