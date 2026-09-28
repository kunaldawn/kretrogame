#include "pack_facts.h"

#include <exception>
#include <fstream>

#include "../../install/keys.h"
#include "../../util/env.h"
#include "../../util/file_io.h"
#include "../../util/paths.h"
#include "../../util/safe_names.h"
#include "../gamepad.h"
#include "exe_probe.h"

namespace kg::bundle {
namespace fs = std::filesystem;

fs::path find_pack_on_shelf(const fs::path& state, const std::string& id) {
  // The same rule as game_set(), for a state that need not be this process's.
  std::ifstream f(state / "games" / (id + ".set"));
  std::string set;
  std::getline(f, set);
  if (!id_is_safe(set)) return {};
  std::error_code ec;
  fs::path p = state / "packs" / (set + ".kgpack");
  return fs::exists(p, ec) ? p : fs::path();
}

// ---- packs ------------------------------------------------------------------------

PackFacts read_pack_facts(const fs::path& pack, const std::string& id) {
  Pack p = Pack::open(pack);
  PackFacts f;
  f.path = pack;
  f.meta = p.game(id);
  f.set_id = p.set().set_id;
  f.set_games = p.games().size();
  std::error_code ec;
  f.bytes = fs::file_size(pack, ec);
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
    if (pk.empty() || !fs::exists(pk, ec)) continue;
    try {
      f.packs[i] = read_pack_facts(pk, d.games[i].id);
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
