#include "draft_build.h"

#include <ctime>
#include <exception>
#include <stdexcept>
#include <utility>

#include "../../util/env.h"
#include "../../util/paths.h"
#include "../../util/pe.h"
#include "../../util/safe_names.h"
#include "exe_probe.h"
#include "pack_facts.h"
#include "player_base.h"

namespace kg::bundle {
namespace fs = std::filesystem;

namespace {

// Now, in UTC, the way bundle.meta records when a player was built.
std::string utc_timestamp() {
  char when[32] = {};
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return when;
}

}  // namespace

// ---- building -----------------------------------------------------------------------------

std::string output_name(const Draft& d) { return d.id + "-" + d.version + ".run"; }

BundleMeta meta_from_draft(const Draft& d, const std::vector<install::StoredKey>& keys,
                           const std::string& built_at, const std::vector<std::string>& licenses,
                           const std::map<std::string, bool>& exe64) {
  BundleMeta m;
  m.id = d.id;
  m.title = d.title;
  m.version = d.version;
  m.built_at = built_at;
#ifdef KRETRO_VERSION
  m.kretro_version = KRETRO_VERSION;
#else
  m.kretro_version = "dev";
#endif
  m.banner = d.banner;
  m.icon = d.icon;
  m.rights_acknowledged = d.rights;
  m.licenses = licenses;
  for (const DraftGame& g : d.games) {
    GameMeta gm;
    gm.id = g.id;
    gm.name = g.name;
    gm.year = g.year;
    gm.cover = g.cover;
    gm.backend = g.backend;
    gm.needs_gpu = g.needs_gpu;
    gm.display = g.display;
    gm.fullscreen = g.fullscreen;
    gm.gamepad = g.gamepad;
    gm.extra_dlls = g.extra_dlls;
    if (g.embed_key) {
      std::string v = install::key_for(keys, g.id);
      if (v.empty()) throw std::runtime_error("the keys vault has no key for " + g.id + " to embed");
      auto bits = exe64.find(g.id);
      const bool is64 = bits != exe64.end() && bits->second;
      gm.key = GameMeta::Key{v, g.key_path, g.key_value, is64 ? "64" : "32"};
    }
    m.games.push_back(std::move(gm));
  }
  return m;
}

BuildInputs shelf_build_inputs(const fs::path& base) {
  return BuildInputs{.self = env_or_empty("KRETRO_SELF"),
                     .games_dir = games_dir(),
                     .keys_file = install::keys_file(),
                     .tool = env_or_empty("KRETRO_DWARFS"),
                     .cache = cache_dir(),
                     .base = base,
                     .out = {}};
}

Built build_from_draft(const Draft& d, const BuildInputs& in, const Callbacks& cb) {
  if (!d.rights) throw std::runtime_error("tick \"I have the right to distribute these games\" first");
  // The id is half of the output's name, joined onto the chosen folder: one
  // with a '/' or a '..' in it would write the player somewhere else.
  if (!kg::id_is_safe(d.id)) throw std::runtime_error("the bundle id '" + d.id + "' cannot be part of a file name");
  if (!version_is_safe(d.version)) throw std::runtime_error("the version '" + d.version + "' cannot be part of a file name");
  if (d.out_dir.empty()) throw std::runtime_error("choose a folder to write the bundle to");
  std::error_code ec;
  if (!fs::is_directory(d.out_dir, ec)) throw std::runtime_error(d.out_dir + " is not a folder");

  std::vector<fs::path> packs;
  for (const DraftGame& g : d.games) {
    fs::path p = find_pack_on_shelf(in.games_dir, g.id);
    if (p.empty()) throw std::runtime_error(g.display_name() + " is no longer on the shelf");
    packs.push_back(p);
  }
  BaseSource base = in.base.empty() ? find_player_base(in.self) : BaseSource::whole_file(in.base);

  const std::string when = utc_timestamp();

  // Which half of the registry each embedded key is for: the game's own
  // executable says, in its PE header.
  std::map<std::string, bool> exe64;
  for (size_t i = 0; i < d.games.size(); ++i) {
    if (!d.games[i].embed_key || in.tool.empty()) continue;
    try {
      pe::Imports im = read_exe_imports(read_pack_facts(packs[i]), in.tool,
                                        (in.cache.empty() ? fs::path(d.out_dir) : in.cache) / ".exe-scratch");
      if (im.ok) exe64[d.games[i].id] = im.is64;
    } catch (const std::exception&) {
    }
  }

  BundleMeta meta = meta_from_draft(d, install::load_keys(in.keys_file), when,
                                    base_licenses(base, in.tool, in.cache), exe64);
  return build_bundle(base, meta, packs, in.out.empty() ? fs::path(d.out_dir) / output_name(d) : in.out, cb);
}

}  // namespace kg::bundle
