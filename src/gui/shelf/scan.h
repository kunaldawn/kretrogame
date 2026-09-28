// What the shelf shows: the collection, and what is known about each game.
//
// In kg::gui rather than kg::gui::shelf, because the Bundles page lists the
// same games from the same scan.
#pragma once

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "../../pack/kgpack.h"
#include "../../rt/env.h"
#include "../../session/journal.h"

namespace kg::gui {

struct Entry {
  std::string id, name;
  uint32_t year = 0;
  // The set the game is in: a DVD's games share one pack, and pack_bytes is
  // that whole set's.
  std::filesystem::path pack;
  uint64_t pack_bytes = 0;
  std::string set_id;
  size_t set_games = 1;
  uint64_t tree_bytes = 0;
  size_t files = 0;

  // Art harvested from the game itself. There is no network here and never
  // will be, so the pictures come from the game's own screen.
  std::filesystem::path title_png, last_png;

  // The resume dossier.
  std::time_t last_played = 0;
  double total_seconds = 0;
  size_t sessions = 0;
  std::string last_note;

  bool installed = true;
  // A manifest we cannot fully resolve: its discs are not in the collection,
  // or the file itself does not parse. Not a refusal - the wizard takes files
  // from wherever they are - only what the shelf can see from here.
  bool blocked = false;
  std::string blocked_reason;
  // The archives the manifest names that are not in iso_dir(), so the game's
  // page can say which rather than "a disc".
  std::vector<std::string> missing_discs;
};

// Installed games first, then anything a manifest says could be installed.
std::vector<Entry> scan(const rt::Env& e);

}  // namespace kg::gui
