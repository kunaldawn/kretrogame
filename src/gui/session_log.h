// A game's sessions as its page lists them, the way `git log --oneline` reads:
// a bullet, when, and for how long, with what the player wrote under it. Shared
// by kretro's game page and a player's, which read the same journal.
#pragma once

#include <vector>

#include "../session/journal.h"

namespace kg::gui {

// The sessions in `journal`, as the journal has them (newest first), in a text
// panel `id` as tall as they are up to `max_lines` lines, past which it
// scrolls. It is one stop for the keys, which scroll it (begin_text_panel).
void session_log(const char* id, const std::vector<session::Record>& journal, float max_lines);

}  // namespace kg::gui
