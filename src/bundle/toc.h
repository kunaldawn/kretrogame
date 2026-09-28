// Where everything is inside a kretro file: the trailer and the table of
// contents.
//
// kretro and every player it builds are one ELF with payloads appended:
//
//   [bootstrap ELF][payload][payload]...[table of contents][trailer]
//
// The trailer is the last 64 bytes and says only where the table is and what
// its BLAKE3 is. The table is a 16-byte header and then one fixed 128-byte
// record per payload: its kind, where it is, how long, the BLAKE3 of its bytes,
// and for a game its id. Fixed records rather than CBOR because the bootstrap
// reads this table too, and the bootstrap is static musl C with no decoder in
// it - a table it can walk with pointer arithmetic is a table it cannot
// misparse.
//
// Binaries linked before v4 ended in a 176-byte trailer with four fixed slots.
// Those are still read, into the same Entry list, so every caller has one API
// and every binary already built stays a binary.
//
// Everything here is read from a file somebody downloaded. Every number in it is
// a stranger's arithmetic and is checked before it is believed: that it lies
// inside the file, that no two payloads claim the same bytes, and that the
// table is the table its trailer hashed.
//
// The format is specified in docs/file-format.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../util/hash.h"

namespace kg::bundle {

enum class Kind : uint32_t {
  Tools = 1,       // the static dwarfs-universal
  Runtime = 2,     // the runtime, a DwarFS image mounted in place
  App = 3,         // the program the runtime's ld-linux runs
  Meta = 4,        // bundle.meta; its presence is what makes a file a player
  Pack = 5,        // a .kgpack, byte for byte as it was on the author's shelf
  PlayerBase = 6,  // a whole player without games, carried inside kretro
};

// "tools", "runtime", ... for messages; "kind N" for one this build predates.
std::string kind_name(Kind k);

inline constexpr std::string_view kTrailerMagic{"KRETROv4", 8};
inline constexpr uint32_t kTrailerVersion = 4;
inline constexpr size_t kTrailerSize = 64;
inline constexpr size_t kLegacyTrailerSize = 176;  // v2 and v3

inline constexpr std::string_view kTocMagic{"KTOC", 4};
inline constexpr uint32_t kTocVersion = 1;
inline constexpr size_t kTocHeaderSize = 16;
inline constexpr size_t kRecordSize = 128;
inline constexpr size_t kNameSize = 64;

// Every payload starts on a page, so a DwarFS image can be mounted straight out
// of the file and a pack's body - itself page-aligned within the pack - stays
// page-aligned in the player.
inline constexpr uint64_t kAlign = 4096;

// A bundle of four thousand games is not a thing anybody will build; a table
// claiming more is damage, and is refused before a byte is allocated for it.
inline constexpr uint32_t kMaxEntries = 4096;

struct Entry {
  Kind kind = Kind::Tools;
  uint32_t flags = 0;
  uint64_t off = 0;  // from the start of the file the table belongs to
  uint64_t len = 0;
  Hash blake3{};
  std::string name;  // the set id for a pack; empty for everything else
  // False only for an entry read from a v2 or v3 trailer. Those slots were
  // hashed with SHA-256, to name cache directories, and the game slot not at
  // all; there is no BLAKE3 to check them against.
  bool hashed = true;
};

// What went wrong, for a caller that words it for a person: the player says
// "download it again" for most of these, and something else for a file that
// was never a kretro file at all.
enum class Failure {
  Unreadable,   // the file could not be opened or read
  Truncated,    // too short to hold what it must
  BadMagic,     // no trailer we know: not a kretro file, or cut short
  BadVersion,   // a trailer or table from a format this build does not read
  BadToc,       // the table's own shape is wrong
  TocHash,      // the table is not the one the trailer hashed
  TooMany,      // the table claims an absurd number of entries
  OutOfBounds,  // something runs past where it may
  Overlap,      // two payloads claim the same bytes
  BadEntry,     // an entry that is wrong on its own: empty, unaligned, badly named
};

class FormatError : public std::runtime_error {
 public:
  FormatError(Failure f, const std::string& what) : std::runtime_error(what), failure_(f) {}
  Failure failure() const { return failure_; }

 private:
  Failure failure_;
};

struct Toc {
  uint32_t version = kTrailerVersion;  // of the trailer it was read from: 2, 3 or 4
  // The file this table describes is `size` bytes at `base` in the path it was
  // read from. Both are zero-and-the-whole-file for an ordinary binary; they
  // are something else for the player base carried inside kretro, whose
  // offsets are relative to its own first byte.
  uint64_t base = 0;
  uint64_t size = 0;
  uint64_t toc_off = 0, toc_len = 0;  // v4 only
  Hash toc_hash{};                    // v4 only
  std::vector<Entry> entries;

  const Entry* find(Kind k) const;
  const Entry* pack(std::string_view id) const;
  std::vector<const Entry*> all(Kind k) const;
  // A file with a bundle.meta is a player; kretro has none.
  bool is_player() const { return find(Kind::Meta) != nullptr; }
  // Where an entry is in the path the table was read from.
  uint64_t at(const Entry& e) const { return base + e.off; }
  // Where the last payload ends, relative to `base`: the table and trailer
  // follow it. Zero for a table with no entries.
  uint64_t payload_end() const;
};

// The table's bytes: header then records, in the order given. Throws
// std::invalid_argument for a name that does not fit its 64 bytes or more
// entries than kMaxEntries; it does not otherwise judge what it is given,
// because the reader is where judgement belongs.
std::string encode_toc(const std::vector<Entry>& entries);

// The 64 bytes that end the file.
std::string encode_trailer(uint64_t toc_off, uint64_t toc_len, const Hash& toc_hash);

// Parses a table's bytes and checks every entry against `limit`, the offset
// where the table itself starts: payloads come before it, so nothing may reach
// past it. Throws FormatError.
std::vector<Entry> decode_toc(std::string_view raw, uint64_t limit);

// Reads the table of a whole file, or of `len` bytes at `off` inside one.
// A v4 trailer is looked for first; failing that a v2 or v3 one, whose slots
// come back as entries of kinds 1, 2 and 3, and a carried game as a kind 5
// entry with an empty name (the old trailer never said which game it was).
// Throws FormatError, with a message a person can act on.
Toc read_toc(const std::filesystem::path& p);
Toc read_toc(const std::filesystem::path& p, uint64_t off, uint64_t len);

}  // namespace kg::bundle
