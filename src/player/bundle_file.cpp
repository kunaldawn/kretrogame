#include "bundle_file.h"

#include <cstdint>
#include <cstdio>
#include <fstream>

#include "../util/hash.h"

namespace kg::player {
namespace fs = std::filesystem;

namespace {

std::string read_range(const fs::path& p, uint64_t off, uint64_t len) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("cannot read " + p.string());
  f.seekg(static_cast<std::streamoff>(off));
  std::string s(len, '\0');
  f.read(s.data(), static_cast<std::streamsize>(len));
  if (static_cast<uint64_t>(f.gcount()) != len) throw std::runtime_error(p.string() + " is shorter than its table says");
  return s;
}

}  // namespace

Bundle Bundle::open(const fs::path& self, const std::string& toc_env) {
  Bundle b;
  b.self = self;
  try {
    b.toc = bundle::read_toc(self);
  } catch (const bundle::FormatError& ex) {
    std::error_code ec;
    throw std::runtime_error("This file is damaged or incomplete (" + std::string(ex.what()) +
                             ", and it is " + std::to_string(fs::file_size(self, ec)) +
                             " bytes). Download it again.");
  }
  if (!toc_env.empty()) {
    unsigned long long off = 0, len = 0;
    if (std::sscanf(toc_env.c_str(), "%llu:%llu", &off, &len) != 2 || off != b.toc.toc_off ||
        len != b.toc.toc_len) {
      throw std::runtime_error("This file changed while it was starting. Run it again.");
    }
  }
  const bundle::Entry* m = b.toc.find(bundle::Kind::Meta);
  if (!m) {
    throw std::runtime_error("This is not a player: it carries no games. A player is built by "
                             "kretro, on its Bundles page.");
  }
  std::string raw = read_range(self, b.toc.at(*m), m->len);
  // The bootstrap checked the table; the table's hash of bundle.meta is how
  // the table vouches for these bytes.
  if (hash_string(raw) != m->blake3) {
    throw std::runtime_error("This file is damaged: its description of its games does not match "
                             "itself. Download it again.");
  }
  b.meta = bundle::BundleMeta::decode(raw);
  // Every game bundle.meta lists has a pack, and verify_bundle proved so when
  // the file was built; a file that has lost one since is a damaged file.
  for (const bundle::GameMeta& g : b.meta.games) {
    if (!b.toc.pack(g.set)) {
      throw std::runtime_error("This file is damaged: it names " + g.name + " and does not carry it. "
                               "Download it again.");
    }
  }
  return b;
}

const bundle::GameMeta* Bundle::game(const std::string& id) const {
  for (const bundle::GameMeta& g : meta.games) {
    if (g.id == id) return &g;
  }
  return nullptr;
}

const bundle::Entry* Bundle::pack(const std::string& id) const {
  const bundle::GameMeta* g = game(id);
  return g ? toc.pack(g->set) : nullptr;
}

std::string Bundle::game_list() const {
  std::string s;
  for (const bundle::GameMeta& g : meta.games) s += (s.empty() ? "" : ", ") + g.id;
  return s;
}

}  // namespace kg::player
