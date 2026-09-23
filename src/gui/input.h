#pragma once
#include <sys/types.h>

#include <map>
#include <string>

namespace kg::gui {

// Runs beside a game, on its private X display, until that game exits.
//
// Two jobs, both of which exist because we own the screen the game is drawing
// into and it does not know we are here:
//
//   - a gamepad is turned into the keyboard and mouse the game was written
//     for, so a 1997 keyboard-only game is playable from a sofa;
//   - when the window loses focus the game is stopped outright, so alt-tabbing
//     away costs nothing and coming back is where you left off.
// `overrides` maps a button name to an X keysym name, from the game's manifest:
// a 1997 game whose keys are wrong can be fixed without changing the default
// for every other game.
int run_input(const std::string& display, pid_t game_pid, bool pause_on_blur,
              const std::map<std::string, std::string>& overrides = {});

}  // namespace kg::gui
