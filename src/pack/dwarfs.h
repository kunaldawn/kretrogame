// The DwarFS tool, and packing a pack's body with it.
#pragma once

#include <filesystem>

#include "../util/proc.h"

namespace kg {

// The DwarFS tool KRETRO_DWARFS names, or an empty path when it is unset or
// set to "".
std::filesystem::path dwarfs_tool();

// Packs the tree at `in` into a DwarFS image at `out`, the way a pack's body
// is packed: `--categorize -S 22`, which is what kBodyPacking (pack_meta.h)
// records. The flags here and that string are a pair, and change together.
ProcResult mkdwarfs_body(const std::filesystem::path& tool, const std::filesystem::path& in,
                         const std::filesystem::path& out);

}  // namespace kg
