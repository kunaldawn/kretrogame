// "Repack for faster loading": a pack's body packed again the way install
// packs one now, with the game tree and so the saves left as they were.
#pragma once

#include <filesystem>

#include "../progress.h"
#include "pack_facts.h"

namespace kg::bundle {

// A pack with a body that was packed before install packed with --categorize
// and 4 MiB blocks (kBodyPacking): it plays, from a slower mount. The page
// offers "Repack for faster loading" for it.
bool packed_before_faster_loading(const PackFacts& f);

// "Repack for faster loading": the body unpacked under `scratch`, its game
// tree held to the pack's Merkle root, packed again the way install packs one
// now, and the pack rewritten beside itself and renamed over. The tree - and
// so the Merkle root, the saves, a recipe's proof - is the same; only the body
// and its hash change. Throws std::runtime_error, or Cancelled, and leaves the
// pack as it was; `scratch` is removed either way.
void repack_for_faster_loading(const std::filesystem::path& pack, const std::filesystem::path& tool,
                               const std::filesystem::path& scratch, const Callbacks& cb = {});

}  // namespace kg::bundle
