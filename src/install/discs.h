// Finding the discs a manifest asks for.
//
// Before the disc layer a manifest named a file in iso/. Now a disc may be the
// third member of a zip inside a zip, and position is not identity: an archive
// of a three-disc game can list its discs as Install, Cinematics, Play, which
// is not the order printed on the discs. So a manifest names a disc by its volume
// label, and falls back to position only where the labels are unhelpful.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../disc/disc.h"
#include "../rt/env.h"

namespace kg::install {

struct DiscRef {
  std::string archive;   // the file in the collection
  std::string label;     // volume label, when named that way
  int index = 0;         // 1-based position, when named that way; 0 means unset
};

// "Example.zip#DEMO_DISC", "Example.zip#2", or a bare filename.
DiscRef parse_disc_ref(std::string_view s);

// Which disc in an opened set the reference means. -1 when nothing matches.
int pick_disc(const std::vector<disc::Disc>& discs, const DiscRef& ref);

// Resolves every reference, opening only the archives that are actually
// needed and reusing an opened set across references into the same archive.
// Throws naming the missing disc, because "which disc is missing" is the only
// useful thing to say when a set is incomplete.
std::vector<disc::Disc> resolve_discs(const rt::Env& e, const std::vector<std::string>& refs,
                                      const std::filesystem::path& work,
                                      const std::function<void(const std::string&)>& say);

}  // namespace kg::install
