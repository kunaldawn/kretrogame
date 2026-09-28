#include "body.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include "../disc/drive.h"
#include "../pack/pack_meta.h"

namespace kg::install {
namespace fs = std::filesystem;

namespace {

// Moves a tree, and falls back to a hardlink farm across a filesystem boundary.
// Neither costs the bytes; a copy would, twice, for a disc set.
void move_or_link(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::rename(from, to, ec);
  if (!ec) return;

  // A single file - an audio track is one - has no tree to walk, and
  // recursive_directory_iterator over a file yields nothing, which would drop
  // the track silently. Link or copy it directly instead.
  if (!fs::is_directory(from, ec)) {
    std::error_code one;
    fs::create_hard_link(from, to, one);
    if (one) {
      one.clear();
      fs::copy_file(from, to, fs::copy_options::overwrite_existing, one);
      if (one) throw std::runtime_error("cannot place " + to.string() + ": " + one.message());
    }
    fs::remove(from, ec);
    return;
  }

  ec.clear();
  fs::create_directories(to, ec);
  std::error_code walk;
  for (const fs::directory_entry& de : fs::recursive_directory_iterator(from, walk)) {
    std::error_code one;
    fs::path dst = to / fs::relative(de.path(), from, one);
    if (de.is_symlink(one)) {
      fs::create_directories(dst.parent_path(), one);
      fs::copy_symlink(de.path(), dst, one);
      continue;
    }
    if (de.is_directory(one)) {
      fs::create_directories(dst, one);
      continue;
    }
    fs::create_directories(dst.parent_path(), one);
    fs::create_hard_link(de.path(), dst, one);
    if (one) {
      one.clear();
      fs::copy_file(de.path(), dst, fs::copy_options::overwrite_existing, one);
      if (one) throw std::runtime_error("cannot place " + dst.string() + ": " + one.message());
    }
  }
  if (walk) throw std::runtime_error("cannot read " + from.string() + ": " + walk.message());
  fs::remove_all(from, ec);
}

// What a laid-out tree weighs, for the pack to say how much unpacking it takes.
uint64_t tree_bytes(const fs::path& dir) {
  uint64_t n = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    std::error_code one;
    if (it->is_regular_file(one)) n += it->file_size(one);
  }
  return n;
}

}  // namespace

std::vector<uint64_t> lay_out_set_body(const fs::path& root, const std::vector<BodyGame>& games,
                                       const std::vector<BodyDisc>& discs) {
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  for (const BodyGame& g : games) {
    const fs::path dir = root / body_game_dir(g.id);
    move_or_link(g.tree, dir / "game");

    // Beside the game, never inside it: game/ is hashed into Meta.tree and the
    // Merkle root has to keep meaning the identity of the installed game. A DLL
    // the installer left in system32 is part of the pack, not part of the game
    // directory, and putting it under game/ would change the root of every
    // install that happened to touch C:.
    if (!g.system.empty() && fs::exists(g.system, ec) && !fs::is_empty(g.system, ec)) {
      move_or_link(g.system, dir / "system");
    }

    // The same text Meta.registry.fragment carries. It is here as well so that
    // a person who mounts the image can read what the installer wrote without
    // decoding CBOR.
    if (!g.registry.empty()) {
      std::ofstream f(dir / "registry.reg", std::ios::trunc);
      if (!f) throw std::runtime_error("cannot write the registry fragment into the body");
      f << g.registry;
    }
  }

  // Named by key rather than numbered: a disc keeps its directory whichever
  // games of the set use it and in whatever order they came, so adding a game
  // or trimming a set never renames one.
  std::vector<uint64_t> sizes;
  sizes.reserve(discs.size());
  for (const BodyDisc& d : discs) {
    fs::path dst = root / body_disc_dir(d.key);
    move_or_link(d.tree, dst);
    // The two files that make a directory-backed drive answer a CD check. They
    // are written now, once, while the tree is still writable; at play time it
    // is a read-only DwarFS mount and this would throw.
    disc::write_drive_metadata(dst, d.label, d.serial);
    if (!d.audio.empty()) {
      fs::path adir = dst / "audio";
      fs::create_directories(adir, ec);
      for (const fs::path& t : d.audio) {
        if (!fs::exists(t, ec)) continue;
        move_or_link(t, adir / t.filename());
      }
    }
    sizes.push_back(tree_bytes(dst));
  }
  return sizes;
}

void collect_from_set(const fs::path& from, const SetMeta& set, const std::string& skip_game,
                      const std::vector<std::string>& keep_discs, std::vector<BodyGame>& games,
                      std::vector<BodyDisc>& discs) {
  std::error_code ec;
  for (const Meta& g : set.games) {
    if (g.id == skip_game) continue;
    // A game already on its way into the body - from an earlier folded set -
    // is not taken twice.
    if (std::any_of(games.begin(), games.end(), [&](const BodyGame& b) { return b.id == g.id; })) continue;
    const fs::path dir = from / body_game_dir(g.id);
    const fs::path system = dir / "system";
    games.push_back(BodyGame{g.id, dir / "game", fs::exists(system, ec) ? system : fs::path(), g.registry.fragment});
  }
  for (const std::string& key : keep_discs) {
    if (std::any_of(discs.begin(), discs.end(), [&](const BodyDisc& d) { return d.key == key; })) continue;
    auto it = std::find_if(set.discs.begin(), set.discs.end(), [&](const Meta::Disc& d) { return d.key == key; });
    if (it == set.discs.end()) continue;
    // The audio is already inside the tree, where the set's own layout put it.
    discs.push_back(BodyDisc{key, from / body_disc_dir(key), it->label, it->serial, {}});
  }
}

}  // namespace kg::install
