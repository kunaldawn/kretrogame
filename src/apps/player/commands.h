// What the player does for each command on its line, once main has settled
// where its state lives and built the Player. Internal to src/apps/player.
#pragma once

#include <string>
#include <vector>

#include "../../player/cli.h"
#include "../../player/player.h"

namespace kg::player::app {

// The gamepad helper a session starts beside a game. Runs before the state is
// settled: main has already taken the state the game's player chose. Arguments
// it does not understand return 2, silently; no person typed them.
int input_helper(const Bundle& b, const std::vector<std::string>& args);

// play <game>, with its checks, its warnings and the offer to unpack.
int play(const Player& p, const Command& c);

// --doctor, and --save to keep a copy of the report.
int doctor(const Player& p, const Command& c);

// --extract-to DIR [<game>].
int extract(const Player& p, const Command& c);

// --licenses.
int licenses(const Player& p);

// saves <game> export <file>.
int saves_export(const Player& p, const Command& c);

// saves <game> import <file>.
int saves_import(const Player& p, const Command& c);

}  // namespace kg::player::app
