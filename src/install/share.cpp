#include "share.h"

#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>

#include "../pack/kgpack.h"
#include "../util/paths.h"
#include "install.h"
#include "set_merge.h"

namespace kg::install {
namespace fs = std::filesystem;

std::vector<InstalledGame> installed_games() {
  std::vector<InstalledGame> out;
  // One open per set: a DVD of three games is one pack read, not three.
  std::map<fs::path, std::optional<SetMeta>> sets;
  for (const std::string& id : indexed_games()) {
    const fs::path pack = game_pack(id);
    if (pack.empty()) continue;
    auto it = sets.find(pack);
    if (it == sets.end()) {
      std::optional<SetMeta> s;
      try {
        s = Pack::open(pack).set();
      } catch (const std::exception&) {
      }
      it = sets.emplace(pack, std::move(s)).first;
    }
    if (it->second && it->second->find(id)) out.push_back(InstalledGame{id, pack});
  }
  return out;
}

fs::path export_recipe(const std::string& id, const fs::path& out) {
  fs::path src = game_pack(id);
  std::error_code ec;
  if (src.empty() || !fs::exists(src, ec)) throw std::runtime_error(id + " is not installed");

  Pack p = Pack::open(src);
  const Meta& m = p.game(id);
  if (m.recipe.method.empty()) {
    throw std::runtime_error(id + " has no recipe recorded, so it cannot be shared this way");
  }
  if (m.recipe.fingerprints.empty()) {
    throw std::runtime_error(id + " has no disc fingerprint recorded; reinstall it to record one");
  }

  fs::path dst = out.empty() ? fs::path(id + ".recipe.kgpack") : out;
  // No body, and this one game: the tree, the recipe and the root travel; the
  // bytes, and the set's other games, do not. It is named after its own discs,
  // which is the set a rebuild from them makes.
  write_pack(dst, set_of(m), WriteOptions{PackKind::Game, std::nullopt, false});
  return dst;
}

ImportResult import_pack(const rt::Env& e, const fs::path& in, bool replace,
                         const std::function<void(const std::string&)>& say) {
  ImportResult res;
  Pack p = Pack::open(in);
  const SetMeta& set = p.set();
  const Meta& first = set.games.front();
  res.id = first.id;
  res.name = first.name.empty() ? first.id : first.name;
  for (const Meta& g : set.games) res.ids.push_back(g.id);
  ensure_state_dirs();

  if (p.header().kind == PackKind::SaveExport) {
    throw std::runtime_error(
        "that is a saves pack, not a game. Import it with: kretro import-saves <file>");
  }

  std::error_code ec;
  if (p.has_body()) {
    // A capsule: a whole set. Verify it, then it simply becomes the installed
    // set - the file already is what an install produces.
    res.had_body = true;
    // The shelf's one writer at a time, as an install is.
    session::GameLock shelf = lock_shelf();
    // A game lives in one set. Taking this one would leave the game's index
    // pointing at one set and its bytes in two.
    for (const Meta& g : set.games) {
      const std::string there = game_set(g.id);
      if (!there.empty() && there != set.set_id && fs::exists(set_pack(there), ec)) {
        throw std::runtime_error(g.id + " is already installed, in " + there + "; that set would have to go first");
      }
    }
    const fs::path dst = set_pack(set.set_id);
    if (fs::exists(dst, ec)) {
      // A set's name comes from its discs, so two people who installed
      // different games off one disc have sets of one name. Replacing this
      // one with that one would take the games only this one holds with it.
      std::string lost;
      try {
        const Pack here = Pack::open(dst);
        for (const Meta& g : here.games()) {
          if (!set.find(g.id)) lost += (lost.empty() ? "" : ", ") + g.id;
        }
      } catch (const std::exception&) {
      }
      if (!lost.empty()) {
        throw std::runtime_error("importing this set would replace set " + set.set_id + " on the shelf, and " + lost +
                                 " would be lost with it; install this pack's games from the disc instead");
      }
      if (!replace) throw std::runtime_error("set " + set.set_id + " is already installed");
      say("replacing the " + res.name + " already here");
    }
    say("verifying " + in.filename().string());
    Pack::Verification v = p.verify();
    if (!v.ok) throw std::runtime_error("this pack is damaged: " + v.detail);
    // Beside the destination and renamed over it, so a set a game is playing
    // from is never half a file.
    fs::path partial = dst;
    partial += ".partial";
    fs::copy_file(in, partial, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::rename(partial, dst, ec);
    if (ec) {
      std::error_code rm;
      fs::remove(partial, rm);
      throw std::runtime_error("cannot place the pack: " + ec.message());
    }
    for (const Meta& g : set.games) write_game_index(g.id, set.set_id);
    res.pack = dst;
    res.root_matched = true;
    return res;
  }

  // A recipe. Rebuild it from a disc the importer owns, and prove the result.
  if (set.games.size() != 1) throw std::runtime_error("a recipe carries one game, and this one carries several");
  res.rebuilt = true;
  say("this is a recipe - rebuilding " + res.name + " from your own disc");
  Options opt;
  opt.force = true;
  opt.expect_root_set = true;
  // The game's own root: the header's is the set's, over every game in it.
  opt.expect_root = first.tree.root();
  Result r = run(e, first, opt, say);
  res.pack = r.pack;
  res.root_matched = r.root_matched;
  return res;
}

}  // namespace kg::install
