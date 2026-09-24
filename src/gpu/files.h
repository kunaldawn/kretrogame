// Replacing files and links in directories a running game may be reading.
#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace kg::gpu {

// Writing what the loaders of a game already running read. Every start of a
// bundle rebuilds these directories - --doctor included - while a game
// started earlier from the same state reads them, and a Vulkan loader reads
// its manifests again at each instance. So nothing is removed and made
// again: each file or link is written beside itself and renamed over, and is
// the old one or the new one and never missing; then keep_only removes what
// `dir` holds that is not in `names`, other starts' temporaries aside.
void replace_file(const std::filesystem::path& dst, const std::string& bytes);
void replace_symlink(const std::filesystem::path& target, const std::filesystem::path& link);
void keep_only(const std::filesystem::path& dir, const std::set<std::string>& names);

}  // namespace kg::gpu
