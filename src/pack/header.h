// The fixed 96-byte header every kgpack starts with: the magic, the container
// revision, the kind, and where the metadata, body and signature are.
// The format is specified in docs/file-format.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../util/hash.h"

namespace kg {

inline constexpr char kPackMagic[7] = {'K', 'G', 'P', 'A', 'C', 'K', '\0'};
// 3: a media set - discs/<key>/ once each, and games/<id>/ for every game
//    installed from them (see SetMeta). Revisions 1 (a flat body) and 2 (one
//    game with its own copy of its discs) are no longer read: those games are
//    installed again.
inline constexpr uint8_t kContainerRevision = 3;
inline constexpr uint8_t kOldestReadableRevision = 3;
inline constexpr uint16_t kPackFormatVersion = 1;
inline constexpr size_t kPackHeaderSize = 96;
inline constexpr uint64_t kBodyAlign = 4096;  // keeps the body mmap-friendly

// 64 MiB is far beyond any real manifest. It bounds the decompression of the
// metadata, and it bounds the header's declared meta_len too: a pack is a thing
// people are told to pass around, so the number a stranger wrote in the header
// gets checked before a byte is allocated on the strength of it.
inline constexpr uint64_t kMaxMetaLen = 64u << 20;

enum class PackKind : uint16_t { Game = 1, Runtime = 2, SaveExport = 3 };

// "game", "runtime" or "save-export": how kgpack info names a kind.
const char* pack_kind_name(PackKind k);
// The word kgpack create takes after --kind: "game", "runtime" or "save".
// Anything else is nullopt.
std::optional<PackKind> parse_pack_kind(std::string_view s);

enum Flags : uint16_t {
  kHasBody = 1u << 0,
  kSigned = 1u << 1,
  kBodyIsSquashfs = 1u << 2,  // clear means DwarFS
};

struct Header {
  uint8_t revision = kContainerRevision;
  uint16_t format_version = kPackFormatVersion;
  uint16_t flags = 0;
  PackKind kind = PackKind::Game;
  uint64_t meta_off = 0, meta_len = 0;
  uint64_t body_off = 0, body_len = 0;
  uint64_t sig_off = 0, sig_len = 0;
  Hash blake3_root{};

  std::string serialize() const;                      // exactly kPackHeaderSize bytes
  static Header parse(std::string_view raw);          // throws on anything wrong

  bool has_body() const { return (flags & kHasBody) != 0; }
  bool body_is_squashfs() const { return (flags & kBodyIsSquashfs) != 0; }
};

}  // namespace kg
