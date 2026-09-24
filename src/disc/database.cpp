#include "database.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <fstream>

#include "../util/paths.h"

namespace kg::iso {
namespace fs = std::filesystem;

std::vector<Known> load_database(const rt::Env& e) {
  std::vector<Known> out;
  std::error_code ec;
  std::vector<fs::path> candidates;
  if (const char* d = std::getenv("KRETRO_DISC_DB")) candidates.push_back(d);
  if (e.valid()) candidates.push_back(e.root / "usr/share/kretro/discs.txt");
  candidates.push_back(state_dir() / "discs.txt");
  candidates.push_back("db/discs.txt");

  for (const fs::path& p : candidates) {
    if (!fs::exists(p, ec)) continue;
    return load_database_file(p);  // the first database found wins
  }
  return out;
}

std::vector<Known> load_database_file(const fs::path& p) {
  std::vector<Known> out;
  std::ifstream f(p);
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
    // v1: hash size volume name.  v2: hash size volume set disc name.
    if (col.size() != 4 && col.size() != 6) continue;
    Known k;
    try {
      // The stored prefix is the first 32 hex digits, so pad it back out to
      // a full hash before parsing.
      std::string hex = col[0];
      hex.resize(64, '0');
      k.prefix = from_hex(hex);
    } catch (const std::exception&) {
      continue;  // a corrupt row must not take the rest of the database with it
    }
    k.size = std::strtoull(col[1].c_str(), nullptr, 10);
    k.volume_id = col[2];
    if (col.size() == 4) {
      k.name = col[3];
    } else {
      k.set_id = col[3];
      k.disc_no = std::atoi(col[4].c_str());
      if (k.disc_no <= 0) k.disc_no = 1;
      k.name = col[5];
    }
    out.push_back(std::move(k));
  }
  return out;
}

Match identify(const std::vector<Known>& db, const Info& info) {
  Match m;
  for (const Known& k : db) {
    // Compare only the leading bytes, because that is all the database stores.
    bool prefix_same = std::equal(k.prefix.begin(), k.prefix.begin() + 16, info.prefix.begin());
    if (k.size == info.size && prefix_same) {
      m.entry = &k;
      m.exact = true;
      return m;
    }
    if (!info.volume_id.empty() && k.volume_id == info.volume_id && k.size == info.size) {
      // Right disc, wrong bytes: the classic signature of a bad dump or a
      // drive that is failing.
      m.entry = &k;
      m.suspect_bad_dump = true;
    }
  }
  return m;
}

}  // namespace kg::iso
