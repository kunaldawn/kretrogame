// The checks on the two strings a pack carries that become paths on the
// machine that opens it: its id and its install directory.
#pragma once

#include <string_view>

namespace kg {

// Two strings in a pack become paths on the machine that opens it, and a pack
// is the one thing users are told to send each other. Both are checked where
// they arrive rather than where they are used, because they are used in four
// places each and it takes only one of those to be missed.
//
// The id names state/games/<id>.kgpack, state/saves/<id> and
// state/prefixes/<id>: one ordinary component, or the pack chooses the
// directory. The wizard already slugs the ids it makes; this is about the ids
// that arrive from outside.
bool id_is_safe(std::string_view id);
// install_dir is joined onto the prefix's drive_c and the result is removed and
// replaced with a symlink the first time the game is played, so it has to be a
// relative path that is still under drive_c once normalised. Empty is safe: it
// means the pack records no install directory and nothing is linked.
bool install_dir_is_safe(std::string_view dir);

}  // namespace kg
