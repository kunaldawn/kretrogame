#include "body.h"

#include <fstream>
#include <stdexcept>
#include <system_error>

#include "../disc/drive.h"

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

}  // namespace

void lay_out_body(const fs::path& root, const fs::path& tree, const std::vector<BodyDisc>& discs,
                  const std::string& registry_fragment, const fs::path& system) {
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  move_or_link(tree, root / "game");

  // Beside the game, never inside it: `tree` is hashed into Meta.tree and the
  // Merkle root has to keep meaning the identity of the installed game. A DLL
  // the installer left in system32 is part of the pack, not part of the game
  // directory, and putting it under game/ would change the root of every
  // install that happened to touch C:.
  if (!system.empty() && fs::exists(system, ec) && !fs::is_empty(system, ec)) {
    move_or_link(system, root / "system");
  }

  // Numbered from 1, because that is what a person calls disc 1, and because
  // Meta.discs[i] is disc i+1 for every reader of this layout.
  for (size_t i = 0; i < discs.size(); ++i) {
    fs::path dst = root / "discs" / std::to_string(i + 1);
    move_or_link(discs[i].tree, dst);
    // The two files that make a directory-backed drive answer a CD check. They
    // are written now, once, while the tree is still writable; at play time it
    // is a read-only DwarFS mount and this would throw.
    disc::write_drive_metadata(dst, discs[i].label, discs[i].serial);
    if (discs[i].audio.empty()) continue;
    fs::path adir = dst / "audio";
    fs::create_directories(adir, ec);
    for (const fs::path& t : discs[i].audio) {
      if (!fs::exists(t, ec)) continue;
      move_or_link(t, adir / t.filename());
    }
  }

  // The same text Meta.registry.fragment carries. It is here as well so that a
  // person who mounts the image can read what the installer wrote without
  // decoding CBOR, and so a future reader of the body needs no metadata at all.
  if (!registry_fragment.empty()) {
    std::ofstream f(root / "registry.reg", std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write the registry fragment into the body");
    f << registry_fragment;
  }
}

}  // namespace kg::install
