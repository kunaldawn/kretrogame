// The player base a bundle is built on: where it comes from, how much of it
// goes into a player, and the licence notices its runtime carries.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../build.h"

namespace kg::bundle {

// ---- the player base ----------------------------------------------------------------

// Where the player base comes from: kretro's own file, the kind 6 entry. A
// development tree can point KRETRO_PLAYER_BASE at a linked player base
// instead. Throws with what to do when there is none.
BaseSource find_player_base(const std::filesystem::path& self);

// What of the base goes into a player: everything up to the end of its last
// payload, its own table and trailer being replaced. The runtime line of the
// size report.
uint64_t player_base_bytes(const BaseSource& b);

// The licence notices the player base's own runtime carries: the runtime a
// player ships, not the one kretro runs on, which carries tools no player has.
// Read with dwarfsck out of a copy of the base's runtime entry - DwarFS reads
// past an image's end for its index, so it cannot be pointed into the middle
// of a larger file - and remembered in `cache` by the entry's BLAKE3, so the
// copy is made once per base. With `compute` false only what is remembered is
// returned. Empty when the list cannot be read; it is a record in bundle.meta,
// and the player lists its own runtime's notices when asked.
std::vector<std::string> base_licenses(const BaseSource& base, const std::filesystem::path& tool,
                                       const std::filesystem::path& cache, bool compute = true);

}  // namespace kg::bundle
