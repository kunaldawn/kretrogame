#include "pack_facts.h"

#include <exception>

#include "../../install/keys.h"
#include "../../util/env.h"
#include "../../util/file_io.h"
#include "../../util/paths.h"
#include "../gamepad.h"
#include "exe_probe.h"

namespace kg::bundle {
namespace fs = std::filesystem;

fs::path find_pack_on_shelf(const fs::path& games_dir, const std::string& id) {
  std::error_code ec;
  fs::path direct = games_dir / (id + ".kgpack");
  if (fs::exists(direct, ec)) return direct;
  for (const fs::directory_entry& de : fs::directory_iterator(games_dir, ec)) {
    if (de.path().extension() != ".kgpack") continue;
    try {
      if (Pack::open(de.path()).meta().id == id) return de.path();
    } catch (const std::exception&) {
    }
  }
  return {};
}

// ---- packs ------------------------------------------------------------------------

PackFacts read_pack_facts(const fs::path& pack) {
  Pack p = Pack::open(pack);
  PackFacts f;
  f.path = pack;
  f.meta = p.meta();
  std::error_code ec;
  f.bytes = fs::file_size(pack, ec);
  for (const Meta::Disc& d : f.meta.discs) (d.embedded ? f.discs_carried : f.discs_named)++;
  f.without_discs = f.bytes;
  if (f.discs_carried > 0) {
    // What the discs weigh unpacked is what their images weighed; the game's
    // share is its own tree and what its installer wrote to C:.
    uint64_t disc_bytes = 0;
    for (const DiscFingerprint& fp : f.meta.recipe.fingerprints) disc_bytes += fp.size;
    uint64_t game_bytes = f.meta.tree.total_bytes() + f.meta.system.bytes;
    if (disc_bytes > 0 && game_bytes + disc_bytes > 0) {
      long double share = static_cast<long double>(game_bytes) / static_cast<long double>(game_bytes + disc_bytes);
      f.without_discs = static_cast<uint64_t>(static_cast<long double>(f.bytes) * share);
    }
  }
  return f;
}

DraftGame game_from_pack(const PackFacts& f, const fs::path& cover_png) {
  DraftGame g;
  g.id = f.meta.id;
  g.name = f.meta.name.empty() ? f.meta.id : f.meta.name;
  g.year = f.meta.year;
  std::error_code ec;
  if (!cover_png.empty() && fs::exists(cover_png, ec)) {
    g.cover = read_file_or_empty(cover_png);
    g.cover_from = cover_png.string();
  }
  // The pack's own gamepad bindings, in the form the player reads back, so
  // the player's default for this game is what the author has been playing
  // with.
  g.gamepad = format_gamepad(f.meta.input);
  return g;
}

DraftFacts gather_draft_facts(const Draft& d, bool read_imports) {
  DraftFacts f;
  f.packs.resize(d.games.size());
  std::vector<install::StoredKey> keys = install::load_keys(install::keys_file());
  f.games.assign(d.games.size(), {});
  for (size_t i = 0; i < d.games.size(); ++i) {
    std::error_code ec;
    fs::path pk = game_pack(d.games[i].id);
    if (!fs::exists(pk, ec)) continue;
    try {
      f.packs[i] = read_pack_facts(pk);
    } catch (const std::exception&) {
      continue;
    }
    // packs is never resized after this, so the pointer stays good for as
    // long as f, or whatever f is moved into, lives.
    f.games[i].pack = &f.packs[i];
    f.games[i].vault_key = install::key_for(keys, d.games[i].id);
    if (read_imports) {
      f.games[i].imports = read_exe_imports(f.packs[i], env_or_empty("KRETRO_DWARFS"), cache_dir() / "bundle-exe");
    }
  }
  return f;
}

}  // namespace kg::bundle
