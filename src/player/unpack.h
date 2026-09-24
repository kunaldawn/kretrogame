// Unpacking a game, for a machine that cannot mount it: how much room it
// takes and how much there is.
#pragma once

#include <cstdint>
#include <filesystem>

#include "../pack/kgpack.h"

namespace kg::player {

// What unpacking a game would take, on a machine that cannot mount it.
struct UnpackPlan {
  bool ready = false;              // already unpacked here, and it is this pack
  std::filesystem::path where;
  uint64_t need = 0;               // bytes, the peak: the copy and the tree
  uint64_t free = 0;               // bytes free where it would go
  bool fits() const { return free >= need; }
};

// An upper estimate of a pack's unpacked size: the game tree, what the
// installer put outside it, and each carried disc at its image's size.
uint64_t unpacked_estimate(const Meta& m);
// Free bytes on the filesystem that holds `p`, or its nearest ancestor that
// exists.
uint64_t free_bytes(const std::filesystem::path& p);

}  // namespace kg::player
