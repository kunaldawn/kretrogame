// The tree manifest: what a game install actually consists of, and the Merkle
// root that makes it provable.
//
// The root is deliberately a hash of the *tree*, not of the packed body. That
// is what lets a recipe pack and a capsule pack of the same install carry the
// same root, so a game rebuilt from your own disc can be proven identical to
// the one someone else packed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../util/hash.h"

namespace kg {

struct TreeEntry {
  std::string path;   // relative, '/'-separated, never with a leading "./"
  uint32_t mode = 0;  // st_mode, so the file type bits distinguish the kinds
  uint64_t size = 0;  // regular files: byte count. symlinks: target length.
                      // directories: 0.
  Hash hash{};        // regular files: content. symlinks: hash of the target
                      // string. directories: hash of the empty string.

  bool is_dir() const;
  bool is_symlink() const;
  bool is_regular() const;
};

class Tree {
 public:
  // Walks `root` without following symlinks, hashing every regular file.
  // Throws if it meets anything that is not a file, directory or symlink,
  // because a game install containing a device node or socket means the
  // extraction went wrong and we would rather say so than pack it.
  static Tree from_directory(const std::filesystem::path& root);

  // Parses the canonical form back. Round-trips exactly.
  static Tree from_canonical(std::string_view text);

  // One line per entry, sorted by path, byte for byte reproducible:
  //   <mode:08x> <size:016x> <hash:64hex> <path>\n
  // It is both the hash input and a readable manifest, which matters when a
  // verification fails and someone has to work out why.
  std::string canonical() const;

  Hash root() const;

  const std::vector<TreeEntry>& entries() const { return entries_; }
  size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }

  uint64_t total_bytes() const;

  // Entries present in `other` but not here, changed, or removed. Used both to
  // report what a session wrote and to explain a failed verification.
  struct Diff {
    std::vector<TreeEntry> added;
    std::vector<TreeEntry> changed;
    std::vector<TreeEntry> removed;
    bool empty() const { return added.empty() && changed.empty() && removed.empty(); }
  };
  Diff diff_to(const Tree& newer) const;

  void add(TreeEntry e);
  void sort();

 private:
  std::vector<TreeEntry> entries_;  // kept sorted by path
};

}  // namespace kg
