// The shelf: kretro's big-picture interface, as the command line opens it.
#pragma once
#include <string>

#include "../../rt/env.h"
// begin_page and the rest of what the shelf shares with a player's launcher.
#include "../widgets.h"

namespace kg::gui {

// What the shelf should be showing when it comes up. Empty is the shelf
// itself, which is what running the binary with no arguments means.
//
// Named Startup rather than Entry because shelf/scan.h already has an Entry,
// and that one is a game on the shelf.
struct Startup {
  std::string game;     // deep-link straight to one game's page
  bool create = false;  // open the wizard instead
  std::string preset;   // a manifest id the wizard starts already knowing
};

// The shelf. Returns a process exit status, and says plainly on the terminal
// when there is no display to draw on.
int run(const rt::Env& e, const Startup& entry = {});
}  // namespace kg::gui
