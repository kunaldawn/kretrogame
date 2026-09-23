#include "keys.h"

#include <fstream>

#include "../util/paths.h"

namespace fs = std::filesystem;

namespace kg::install {
namespace {

// Tabs separate the fields, so they cannot appear inside one.
std::string flatten(std::string s) {
  for (char& c : s) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return s;
}

}  // namespace

fs::path keys_file() { return state_dir() / "keys.txt"; }

std::vector<StoredKey> load_keys(const fs::path& file) {
  std::vector<StoredKey> out;
  std::ifstream f(file);
  if (!f) return out;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> col;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
      if (i == line.size() || line[i] == '\t') {
        col.push_back(line.substr(start, i - start));
        start = i + 1;
      }
    }
    if (col.size() < 2) continue;
    StoredKey k;
    k.game = col[0];
    k.value = col[1];
    if (col.size() > 2) k.note = col[2];
    out.push_back(std::move(k));
  }
  return out;
}

void save_keys(const fs::path& file, const std::vector<StoredKey>& keys) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path tmp = file;
  tmp += ".new";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << "# Serials you have entered. This file stays on this machine: a key is\n"
      << "# yours, not the game's, and it goes into no pack and over no network.\n"
      << "#\n"
      << "# game\tkey\tnote\n";
    for (const StoredKey& k : keys) {
      f << flatten(k.game) << "\t" << flatten(k.value) << "\t" << flatten(k.note) << "\n";
    }
  }
  fs::rename(tmp, file, ec);
}

std::string key_for(const std::vector<StoredKey>& keys, const std::string& game) {
  for (const StoredKey& k : keys) {
    if (k.game == game) return k.value;
  }
  return {};
}

void put_key(std::vector<StoredKey>& keys, const std::string& game, const std::string& value,
             const std::string& note) {
  for (StoredKey& k : keys) {
    if (k.game == game) {
      k.value = value;
      k.note = note;
      return;
    }
  }
  keys.push_back(StoredKey{game, value, note});
}

}  // namespace kg::install
