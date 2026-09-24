// Writing and verifying a player file: building one, and proving the one just
// built is the one that was meant.
//
// A player is the player base - bootstrap, tools, runtime, app, exactly as
// kretro carries it - with bundle.meta and the chosen packs appended after it,
// then a new table of contents and trailer. Nothing is recompiled or repacked:
// the base is copied up to the end of its last payload, and each pack is copied
// byte for byte off the author's shelf, so a pack inside a player has the same
// Merkle root it had on the shelf.
//
// Nothing half-built is ever left looking finished. The player is written to
// <out>.partial, re-read from disk and checked - every entry's BLAKE3, every
// pack's Merkle root and body hash - and only then renamed to <out>. A cancel,
// a full disk, a pack that changed while it was being copied: the .partial is
// removed and the error says which.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../util/hash.h"
#include "meta.h"
#include "progress.h"
#include "toc.h"

namespace kg::bundle {

// A player base: `len` bytes at `off` in `path`, or the whole file when `len`
// is empty. `blake3`, when set, is what those bytes must hash to - the
// kind 6 entry's hash when the base is carried inside kretro - and is checked
// during the copy, which reads every byte anyway.
struct BaseSource {
  std::filesystem::path path;
  uint64_t off = 0;
  std::optional<uint64_t> len;
  std::optional<Hash> blake3;

  // A player base on its own, as `make player-base` links one: the whole file.
  static BaseSource whole_file(const std::filesystem::path& p);
};

// The table of the base's own bytes: `len` of them at `off`, or the rest of the
// file from `off` when `len` is empty. Throws std::runtime_error ("cannot open
// <path>: <reason>") when the file's size cannot be had, and FormatError.
Toc read_base_toc(const BaseSource& b);

// Finds the player base inside kretro's own file (the kind 6 entry), and checks
// that its table is a player base's table. Nothing is copied: the result is a
// range to hand to build_bundle. Throws FormatError or std::runtime_error.
BaseSource extract_player_base(const std::filesystem::path& self);

struct Built {
  std::filesystem::path path;
  uint64_t size = 0;
  Toc toc;
};

// Writes the player. `packs` are .kgpack files with bodies, one for each game
// in meta.games and no others; they go into the file in the order given. Throws
// Cancelled, FormatError or std::runtime_error, and in every case leaves
// neither <out> nor <out>.partial behind that was not there before.
Built build_bundle(const BaseSource& base, const BundleMeta& meta,
                   const std::vector<std::filesystem::path>& packs,
                   const std::filesystem::path& out, const Callbacks& cb = {});

struct Verified {
  Toc toc;
  BundleMeta meta;
};

// Re-reads a player from disk and checks all of it: the table, each entry's
// BLAKE3, bundle.meta, and for each pack its Merkle root, its body hash and
// that it is the game its entry names; and that the games bundle.meta lists
// are the games the file carries. Throws on the first thing that is wrong.
Verified verify_bundle(const std::filesystem::path& p, const Callbacks& cb = {});

// One piece of a file being linked: a payload read from `path`.
struct Part {
  Kind kind;
  std::filesystem::path path;
  std::string name;
};

// Writes bootstrap + parts + table + trailer - what scripts/kretro-link.py does,
// byte for byte. Used by the tests to make player bases, and to hold the two
// writers to one layout.
void link_file(const std::filesystem::path& out, const std::filesystem::path& bootstrap,
               const std::vector<Part>& parts);

// BLAKE3 of `len` bytes at `off`, streamed; `cb` as above.
Hash hash_range(const std::filesystem::path& p, uint64_t off, uint64_t len, const Callbacks& cb = {});

}  // namespace kg::bundle
