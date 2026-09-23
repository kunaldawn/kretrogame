// Cue sheets.
//
// Many disc downloads are .bin/.cue rather than .iso, and 7z cannot read a cue
// sheet. A cue sheet is the disc's table of contents: which tracks exist, what
// sector format each uses, and where each begins. Everything else in this layer
// depends on getting that right.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace kg::disc {

// The sector formats that appear on the discs people actually have. Mode1/2048
// is a plain ISO image; the /2352 forms carry the full raw sector, sync bytes
// and error correction included, which is what a real ripper produces.
enum class TrackMode { Audio, Mode1_2048, Mode1_2352, Mode2_2352, Mode2_2336 };

struct CueTrack {
  int number = 0;
  TrackMode mode = TrackMode::Mode1_2048;
  std::string file;            // as spelled in the cue sheet
  uint64_t start_sector = 0;   // INDEX 01, in sectors, relative to `file`
  uint64_t sector_count = 0;   // zero until resolve_lengths() has run
};

struct CueSheet {
  std::vector<CueTrack> tracks;
};

// Throws std::runtime_error on anything it cannot make sense of: a TRACK with
// no FILE above it, an unknown mode, an empty sheet.
CueSheet parse_cue(std::string_view text);

// "mm:ss:ff" at 75 frames per second. Throws on malformed input.
uint64_t msf_to_sectors(std::string_view msf);

uint32_t sector_bytes(TrackMode m);    // what a sector occupies on disc
uint32_t payload_bytes(TrackMode m);   // how much of it is user data
uint32_t payload_offset(TrackMode m);  // where that data starts in the sector

// Fills in sector_count. A track that shares its file with the next track runs
// until that track's start; a track that owns its file runs to the end of it.
void resolve_lengths(CueSheet& c, const std::function<uint64_t(const std::string&)>& file_size);

}  // namespace kg::disc
