// The body of a set: the directory mkdwarfs is pointed at.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../pack/pack_meta.h"

namespace kg::install {

// One game, as it will appear in the body.
struct BodyGame {
  std::string id;
  std::filesystem::path tree;     // the installed game
  // What the installer wrote to C: outside the game directory, relative to
  // drive_c. Empty for every copy and unzip install: no installer ran, so
  // nothing was written outside the tree to carry.
  std::filesystem::path system;
  std::string registry;           // the fragment; empty for none
};

// One disc, as it will appear in the body.
struct BodyDisc {
  std::string key;                            // its directory, discs/<key>/
  std::filesystem::path tree;                 // the extracted disc
  std::string label;
  uint32_t serial = 0;
  std::vector<std::filesystem::path> audio;   // the ripped tracks
};

// Lays a set's body out under `root`, which mkdwarfs is then pointed at:
//
//   games/<id>/game/          the installed tree
//   games/<id>/system/        what the installer wrote to C: outside the game
//                             directory, relative to drive_c: windows/system32/
//                             msvcrt.dll and the rest of what a 1998 setup
//                             scatters around the machine
//   games/<id>/registry.reg   the fragment, as text, for a person who mounts
//                             the image
//   discs/<key>/              each disc once, with .windows-label and
//                             .windows-serial already written - at play time
//                             the tree is a read-only mount and nothing can put
//                             them there
//   discs/<key>/audio/        the FLAC tracks ripped from that disc
//
// Every tree named is moved where the filesystem allows and hardlinked
// otherwise: they are already gigabytes and already inside the staging
// directory, and a copy here would double the disk an install needs.
// Everything named is consumed - do not read a tree after.
//
// Returns each disc's size as laid out, tree and audio, in the order given.
std::vector<uint64_t> lay_out_set_body(const std::filesystem::path& root, const std::vector<BodyGame>& games,
                                       const std::vector<BodyDisc>& discs);

// What an unpacked set body at `from` gives a new body: every game of `set`
// but `skip_game` - the one being installed again - and each disc in
// `keep_discs` that the set carries and `discs` does not have yet. Appended
// to `games` and `discs` as trees to be moved; nothing is read or copied.
void collect_from_set(const std::filesystem::path& from, const SetMeta& set, const std::string& skip_game,
                      const std::vector<std::string>& keep_discs, std::vector<BodyGame>& games,
                      std::vector<BodyDisc>& discs);

}  // namespace kg::install
