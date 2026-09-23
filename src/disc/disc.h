// A disc, and a set of them.
//
// This is where the pieces meet: a candidate found inside some archive becomes
// a normalised ISO with an identity, a label, a serial and its audio. Above
// this line nothing knows what a sector is.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../install/iso.h"
#include "../rt/env.h"
#include "container.h"

namespace kg::disc {

struct AudioTrack {
  int number = 0;
  uint64_t sector_count = 0;
  std::filesystem::path file;   // the ripped .flac, or .wav if flac was missing
};

struct Disc {
  std::filesystem::path source;      // the file in the collection
  std::vector<std::string> members;  // where inside it, if nested
  std::filesystem::path iso;         // the normalised data track
  iso::Info info;                    // volume id, publisher, prefix hash
  std::vector<AudioTrack> audio;
  std::string label;                 // what Windows will show
  uint32_t serial = 0;               // what a CD check will read
};

struct DiscSet {
  std::string set_id;
  std::string name;
  std::vector<Disc> discs;
  std::vector<Disc> alternates;   // duplicate dumps of a disc already in the set
};

// A stable volume serial derived from the disc's recorded creation time. The
// same pressing always yields the same number, and it is never zero, because
// Windows reads zero as "no volume".
uint32_t volume_serial(const iso::Info& info);

// "Example Game (USA) (Rev 1)" -> "example-game-usa-rev-1".
std::string set_id_from(std::string_view name);

// Materialises a candidate, normalises its data track and reads its identity.
// With `rip_audio`, any audio tracks are ripped beside the ISO.
Disc open(const rt::Env& e, const Candidate& c, const std::filesystem::path& work, bool rip_audio);

// Groups opened discs into a set, moving duplicate dumps aside. An archive
// holding three rips of one disc is one disc and two alternates, not a
// three-disc game.
DiscSet assemble(std::string set_id, std::string name, std::vector<Disc> discs);

}  // namespace kg::disc
