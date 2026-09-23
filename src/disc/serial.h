// Serials and the files that came with the download.
//
// A disc from an archive almost always arrives with its key beside it, in a
// serial.txt or an .nfo or the readme, and finding it by hand means opening
// twenty archives. This reads what is already on the user's disk. It fetches
// nothing - there is no network path in this program - and it applies nothing
// on its own.
//
// Two halves at two stages of life. `scan_companions` is wired up: `kretro
// scan` lists the files that came down beside a disc. `extract_keys` is not -
// it is covered by tests and called by nothing, so no part of the program yet
// says what it found, only where to look.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kg::disc {

struct KeyCandidate {
  std::string value;
  std::string source;   // which file it came out of
};

// Pulls serial-shaped strings out of text. Two shapes count on their own -
// groups of five and groups of four - and a long alphanumeric run counts only
// when a key word sits within a few lines of it, because otherwise every
// checksum in every log becomes a candidate.
std::vector<KeyCandidate> extract_keys(std::string_view text, std::string_view source);

struct Companion {
  std::filesystem::path path;
  std::string kind;   // serial | nfo | readme | crack | nocd
};

// What else is in the directory that holds this image.
std::vector<Companion> scan_companions(const std::filesystem::path& image);

}  // namespace kg::disc
