// What the shelf shows: the collection, and what is known about each game.
#pragma once

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "../pack/kgpack.h"
#include "../session/session.h"

namespace kg::gui {

struct Entry {
  std::string id, name;
  uint32_t year = 0;
  std::filesystem::path pack;
  uint64_t pack_bytes = 0;
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

  // What the pack says about the discs it was built from, for the one sentence
  // the game's page owes a person before they press Play.
  //
  // A revision 1 pack has a flat body and carries no discs at all - there was
  // nowhere in the body to put them - so it is not a pack that lost something,
  // it is a pack made before packs carried discs. A rooted pack that names a
  // disc it does not carry is the other case, and there are two ways to get
  // one: it came from somewhere else, or the person who built it cleared
  // "include the discs" on the wizard's build page, which is offered because a
  // three-disc game is three discs of pack. Either way the disc is still named
  // here, per disc. Both cases come out as labels; the page tells them apart by
  // `flat_body`.
  bool flat_body = false;
  std::vector<std::string> absent_discs;
};

// Installed games first, then anything a manifest says could be installed.
std::vector<Entry> scan(const rt::Env& e);

std::string human_size(uint64_t n);
std::string human_time(double seconds);
std::string ago(std::time_t then);

}  // namespace kg::gui
