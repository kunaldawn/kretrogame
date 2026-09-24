#include "verify_memo.h"

#include <cstddef>
#include <fstream>
#include <vector>

#include "../util/file_io.h"
#include "../util/toml.h"

namespace kg::player {
namespace fs = std::filesystem;

// One line per pack checked: game, version, hash. The version is quoted so a
// version string with spaces in it is still one field.
namespace {
std::string memo_line(const std::string& game, const std::string& version, const Hash& h) {
  return game + " " + toml_string(version) + " " + to_hex(h);
}
}  // namespace

bool pack_verified(const fs::path& memo, const std::string& game, const std::string& version,
                   const Hash& h) {
  std::ifstream f(memo);
  const std::string want = memo_line(game, version, h);
  std::string line;
  while (std::getline(f, line)) {
    if (line == want) return true;
  }
  return false;
}

void remember_verified(const fs::path& memo, const std::string& game, const std::string& version,
                       const Hash& h) {
  if (pack_verified(memo, game, version, h)) return;
  // Older versions' lines are kept: two builds of one bundle share a state
  // directory on purpose, and a person going back and forth between them
  // should not pay for a full check each time. Bounded, so it cannot grow for
  // ever; what falls off the front is only checked again.
  std::vector<std::string> keep;
  {
    std::ifstream f(memo);
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty()) keep.push_back(line);
    }
  }
  keep.push_back(memo_line(game, version, h));
  constexpr size_t kMemoLines = 256;
  if (keep.size() > kMemoLines) keep.erase(keep.begin(), keep.end() - kMemoLines);
  std::string text;
  for (const std::string& l : keep) text += l + "\n";
  write_atomically(memo, text);
}

}  // namespace kg::player
