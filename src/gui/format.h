// How the shelf, the wizard, the Bundles page and a player's launcher print
// sizes, play time and how long ago: the variants of kg::fmt the pages share,
// named once so every page says it the same way.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include "../util/format.h"

namespace kg::gui {

inline std::string human_size(uint64_t n) { return fmt::bytes_compact(n); }

inline std::string human_time(double seconds) { return fmt::duration_short(seconds); }

inline std::string ago(std::time_t then) { return fmt::ago(then, "never played"); }

}  // namespace kg::gui
