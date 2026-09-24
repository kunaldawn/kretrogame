// A player's launcher: the only window a person who was sent a player sees.
//
// The bundle's title and banner, then one tile per game with its cover, name
// and year. Each game has Play and a menu - Saves, Display, Controls - and the
// bundle has its own settings: the applications-menu entry, the data folder,
// the licences, and About, which is the doctor with a Save button.
//
// A one-game bundle skips the grid: the game starts as soon as the machine has
// been checked, and the window only stays up when there is something to say -
// a first-run warning, the offer of a menu entry, a failure. Everything the
// grid's menu reaches is also on the command line.
//
// The first run is quiet unless it cannot be: the doctor runs without a word,
// a problem that stops every game stops here with its message, a warning is
// shown once and remembered in launcher.toml, and "Add to applications menu"
// is offered once.
#pragma once

#include "../../player/player.h"

namespace kg::gui {

// Returns a process exit status. Says on the terminal, and returns 1, when
// there is no display to draw on.
int run_launcher(const player::Player& p);

}  // namespace kg::gui
