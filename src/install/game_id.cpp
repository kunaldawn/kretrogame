#include "game_id.h"

#include <filesystem>
#include <system_error>
#include <vector>

#include "../disc/disc.h"
#include "../util/paths.h"
#include "manifest.h"

namespace kg::install {
namespace fs = std::filesystem;

std::string slug(std::string_view name) {
  // The same rule disc::set_id_from applies to a disc set's name, and
  // deliberately so: a set assembled from "Example Game (USA)" and the game id
  // derived from the same title have to agree, or the two halves of the
  // wizard would disagree about what this game is called.
  return disc::set_id_from(name);
}

std::string IdClash::sentence() const {
  std::vector<std::string> parts;
  if (pack) parts.push_back("a pack");
  if (saves) parts.push_back("saves");
  if (prefix) parts.push_back("a Wine prefix");
  if (manifest) parts.push_back("a manifest");
  std::string s;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) s += (i + 1 == parts.size()) ? " and " : ", ";
    s += parts[i];
  }
  return s;
}

IdClash id_clash(const rt::Env& e, const std::string& id) {
  IdClash c;
  std::error_code ec;
  if (id.empty()) return c;
  // Taken by an index, whether or not the set it names is still there: the
  // index is what a second install under the id would overwrite.
  c.pack = fs::exists(game_index(id), ec);
  c.saves = fs::exists(saves_dir() / id, ec);
  c.prefix = fs::exists(prefixes_dir() / id, ec);
  for (const fs::path& d : manifest_dirs(e)) {
    if (fs::exists(d / (id + ".toml"), ec)) { c.manifest = true; break; }
  }
  return c;
}

std::string next_free_id(const rt::Env& e, const std::string& id) {
  for (int n = 2; n < 100; ++n) {
    std::string candidate = id + "-" + std::to_string(n);
    if (!id_clash(e, candidate).any()) return candidate;
  }
  return id + "-new";
}

}  // namespace kg::install
