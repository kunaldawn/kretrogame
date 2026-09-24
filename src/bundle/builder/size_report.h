// How big the player file will be, and the warnings a size earns.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pack_facts.h"

namespace kg::bundle {

// ---- size -----------------------------------------------------------------------

inline constexpr uint64_t kWarn2G = 2ull << 30;  // GitHub Releases, itch.io web upload
inline constexpr uint64_t kWarn4G = 4ull << 30;  // FAT32

struct SizePart {
  std::string label;
  uint64_t bytes = 0;
  uint64_t without_discs = 0;
};

struct SizeReport {
  SizePart runtime;             // the player base: bootstrap, tools, runtime, app
  uint64_t meta = 0;            // bundle.meta: pictures and extra files
  std::vector<SizePart> games;
  uint64_t total = 0;
  uint64_t total_without_discs = 0;
  std::vector<std::string> warnings;
};

// The file build_bundle would write, byte for byte when the sizes given are
// the real ones: what the author uploads and what a FAT32 stick has to hold.
// `base_bytes` is player_base_bytes of the base.
SizeReport size_report(uint64_t base_bytes, uint64_t meta_bytes,
                       const std::vector<const PackFacts*>& packs);

}  // namespace kg::bundle
