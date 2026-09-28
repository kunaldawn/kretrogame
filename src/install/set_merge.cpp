#include "set_merge.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <system_error>

#include "../pack/pack.h"
#include "../util/hash.h"
#include "../util/paths.h"

namespace kg::install {
namespace fs = std::filesystem;

namespace {

bool has_disc(const std::vector<Meta::Disc>& discs, const std::string& key) {
  return std::any_of(discs.begin(), discs.end(), [&](const Meta::Disc& d) { return d.key == key; });
}

}  // namespace

std::vector<ShelfSet> shelf_sets() {
  std::vector<ShelfSet> out;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(packs_dir(), ec)) {
    if (de.path().extension() != ".kgpack") continue;
    try {
      out.push_back(ShelfSet{de.path(), Pack::open(de.path()).set()});
    } catch (const std::exception&) {
    }
  }
  return out;
}

session::GameLock lock_shelf() {
  session::GameLock l = session::lock_path(packs_dir() / ".lock");
  if (l.busy()) {
    throw std::runtime_error("another install or import is writing the shelf; try again when it has finished");
  }
  return l;
}

std::vector<bool> discs_on_shelf(const std::vector<disc::Disc>& discs) {
  const std::vector<ShelfSet> sets = shelf_sets();
  std::vector<bool> out;
  out.reserve(discs.size());
  for (const disc::Disc& d : discs) {
    const std::string key = disc_key(d.info.size, d.info.prefix);
    out.push_back(std::any_of(sets.begin(), sets.end(), [&](const ShelfSet& s) { return has_disc(s.meta.discs, key); }));
  }
  return out;
}

MergePlan plan_merge(const std::vector<ShelfSet>& sets, const Meta& game) {
  MergePlan p;
  for (size_t i = 0; i < sets.size(); ++i) {
    const SetMeta& s = sets[i].meta;
    bool shares = s.find(game.id) != nullptr;
    for (const Meta::Disc& d : game.discs) shares = shares || has_disc(s.discs, d.key);
    if (shares) p.fold.push_back(i);
  }
  // Smallest id first, and that id kept: the same fold comes out whichever
  // game of it happens to be installed last.
  std::sort(p.fold.begin(), p.fold.end(),
            [&](size_t a, size_t b) { return sets[a].meta.set_id < sets[b].meta.set_id; });

  if (p.fold.empty()) {
    p.meta = set_of(game);
    // A set keeps the id it was first given even when a reinstall has moved
    // it off the disc it was named after, so the name this game's discs make
    // may already be taken - and writing it would replace that set. Another,
    // derived from it, is taken instead.
    const auto taken = [&](const std::string& id) {
      return std::any_of(sets.begin(), sets.end(), [&](const ShelfSet& s) { return s.meta.set_id == id; });
    };
    for (int n = 1; taken(p.meta.set_id); ++n) {
      p.meta.set_id = "s-" + to_hex(hash_string(p.meta.set_id + "#" + std::to_string(n))).substr(0, 16);
    }
    p.set_id = p.meta.set_id;
    for (const Meta::Disc& d : game.discs) p.new_disc_keys.push_back(d.key);
    return p;
  }

  p.set_id = sets[p.fold.front()].meta.set_id;
  p.meta.set_id = p.set_id;
  for (size_t i : p.fold) {
    for (const Meta& g : sets[i].meta.games) {
      // Once each: a crash between a new set and the removal of the one it
      // folded leaves a game in both, and a pack holding it twice would not
      // open. The first copy, from the smallest set id, is kept.
      if (g.id != game.id && !p.meta.find(g.id)) p.meta.games.push_back(g);
    }
    for (const Meta::Disc& d : sets[i].meta.discs) {
      if (!has_disc(p.meta.discs, d.key)) p.meta.discs.push_back(d);
    }
  }
  p.meta.games.push_back(game);
  for (const Meta::Disc& d : game.discs) {
    if (has_disc(p.meta.discs, d.key)) continue;
    p.meta.discs.push_back(d);
    p.new_disc_keys.push_back(d.key);
  }

  // A disc no game of the set uses any more - the one a reinstalled game has
  // just moved off - would be carried unplayed for ever.
  std::vector<Meta::Disc> used;
  for (const Meta::Disc& d : p.meta.discs) {
    const bool wanted = std::any_of(p.meta.games.begin(), p.meta.games.end(),
                                    [&](const Meta& g) { return has_disc(g.discs, d.key); });
    if (wanted) used.push_back(d);
  }
  p.meta.discs = std::move(used);
  std::sort(p.meta.games.begin(), p.meta.games.end(), [](const Meta& a, const Meta& b) { return a.id < b.id; });
  return p;
}

}  // namespace kg::install
