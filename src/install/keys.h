// Where a serial lives once you have typed it.
//
// The scan of a real collection found no serial files at all: these downloads
// simply do not carry them. So the vault's job is not discovery, it is memory -
// `kretro key <id> <serial>` writes one down and `kretro key <id>` reads it
// back, which is a place to keep a serial rather than a scrap of paper.
//
// Nothing types it into a box nobody is watching. What reads it is the wizard,
// which never types anything: its identity page offers whatever is stored under
// the id, draft.cpp's prefill_serial puts it into a draft that arrived without
// one - a recipe carries no serial, so a rebuild depends on this - and the
// install step puts it on screen beside the installer that is asking, for a
// person to read while they type. Keys stay on this machine and go into no
// pack, because a serial is the user's, not the game's.
//
// That holds for the registry too, which is where an installer writes the key
// the moment you have typed it: an unfiltered diff would carry the serial
// inside every capsule and every exported recipe. It is held back -
// registry.h's is_serial_value is the test, Build::diff_after is where it is
// applied and says so - and a game that asks again on first run is asking for
// something this vault is holding.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kg::install {

struct StoredKey {
  std::string game;
  std::string value;
  std::string note;   // where it came from, in the user's words
};

std::filesystem::path keys_file();

std::vector<StoredKey> load_keys(const std::filesystem::path& file);
void save_keys(const std::filesystem::path& file, const std::vector<StoredKey>& keys);

std::string key_for(const std::vector<StoredKey>& keys, const std::string& game);
void put_key(std::vector<StoredKey>& keys, const std::string& game, const std::string& value,
             const std::string& note);

}  // namespace kg::install
