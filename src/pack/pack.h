// Reading a kgpack - opening one, alone or at an offset inside a player,
// verifying it and copying its body out - and writing one.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "header.h"
#include "pack_meta.h"

namespace kg {

class Pack {
 public:
  static Pack open(const std::filesystem::path& p);
  // The same, for a pack that is not a file of its own but `len` bytes at `off`
  // inside a larger one: a pack carried byte for byte inside a player. Every
  // offset a pack's header names is relative to its own first byte, so the pack
  // reads the same wherever it sits, and it may name nothing outside the range.
  static Pack open(const std::filesystem::path& p, uint64_t off, uint64_t len);

  const Header& header() const { return header_; }
  const Meta& meta() const { return meta_; }
  const std::filesystem::path& path() const { return path_; }
  // Where the pack's first byte is in path(): zero for a pack that is a file of
  // its own. The body is at base() + header().body_off, which is the number a
  // mount at an offset wants.
  uint64_t base() const { return base_; }

  bool has_body() const { return header_.has_body(); }

  // Copies the body out to a standalone file, for the DwarFS tools, which want
  // a path rather than an offset into a larger file.
  void extract_body(const std::filesystem::path& out) const;

  struct Verification {
    bool ok = false;
    bool root_matches = false;
    bool body_matches = false;
    std::string detail;
  };
  // Checks the header's root against the metadata's tree, and the body's hash
  // against what the metadata claims. Does not need the body extracted.
  Verification verify() const;

 private:
  std::filesystem::path path_;
  uint64_t base_ = 0;
  Header header_;
  Meta meta_;
};

struct WriteOptions {
  PackKind kind = PackKind::Game;
  // When set, the file's bytes become the pack body.
  std::optional<std::filesystem::path> body;
  bool body_is_squashfs = false;
};

// Writes a complete pack. The header's blake3_root is taken from meta.tree, and
// meta.body is filled in from the body file, so callers cannot forget either.
void write_pack(const std::filesystem::path& out, Meta meta, const WriteOptions& opt);

}  // namespace kg
