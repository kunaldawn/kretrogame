// The DwarFS tool, and packing a pack's body with it.
#pragma once

#include <filesystem>

#include "../util/proc.h"

namespace kg {

class Pack;

// The DwarFS tool KRETRO_DWARFS names, or an empty path when it is unset or
// set to "".
std::filesystem::path dwarfs_tool();

// Packs the tree at `in` into a DwarFS image at `out`, the way a pack's body
// is packed: `--categorize -S 22`, which is what kBodyPacking (pack_meta.h)
// records. The flags here and that string are a pair, and change together.
ProcResult mkdwarfs_body(const std::filesystem::path& tool, const std::filesystem::path& in,
                         const std::filesystem::path& out);

// Unpacks a pack's whole body into `dir`, which must exist. A pack that is a
// file of its own ends with its body, so the tool is pointed at the offset; one
// inside a larger file is followed by the next pack or the table of contents,
// which dwarfsextract would read as more image - it has no counterpart to the
// mount's imagesize - so its body is copied out to `scratch_image` first and
// removed after. Throws std::runtime_error with the tool's words.
void extract_body_tree(const std::filesystem::path& tool, const Pack& pack, const std::filesystem::path& dir,
                       const std::filesystem::path& scratch_image);

}  // namespace kg
