#include "header.h"

#include <cstring>
#include <stdexcept>

#include "../util/bytes.h"

namespace kg {

namespace {

// And the same for the body. A capsule carrying a three-disc game's discs is
// a few gigabytes and a large one is not much more; a terabyte is past anything
// that will ever be a game. The number matters less than the bound existing:
// body_off and body_len are a stranger's arithmetic, and Pack::open compares
// them against the file's size. Unbounded, 2^64-4096 is a body length that
// passes every check by wrapping.
constexpr uint64_t kMaxBodyLen = 1ull << 40;

}  // namespace

const char* pack_kind_name(PackKind k) {
  switch (k) {
    case PackKind::Game: return "game";
    case PackKind::Runtime: return "runtime";
    case PackKind::SaveExport: return "save-export";
  }
  return "?";
}

std::optional<PackKind> parse_pack_kind(std::string_view s) {
  if (s == "game") return PackKind::Game;
  if (s == "runtime") return PackKind::Runtime;
  if (s == "save") return PackKind::SaveExport;
  return std::nullopt;
}

std::string Header::serialize() const {
  std::string s;
  s.reserve(kPackHeaderSize);
  s.append(kPackMagic, sizeof(kPackMagic));
  s.push_back(static_cast<char>(revision));
  le::put_u16(s, format_version);
  le::put_u16(s, flags);
  le::put_u16(s, static_cast<uint16_t>(kind));
  le::put_u16(s, 0);  // reserved
  le::put_u64(s, meta_off);
  le::put_u64(s, meta_len);
  le::put_u64(s, body_off);
  le::put_u64(s, body_len);
  le::put_u64(s, sig_off);
  le::put_u64(s, sig_len);
  s.append(reinterpret_cast<const char*>(blake3_root.data()), blake3_root.size());
  if (s.size() != kPackHeaderSize) throw std::runtime_error("header size drifted from 96 bytes");
  return s;
}

Header Header::parse(std::string_view raw) {
  if (raw.size() < kPackHeaderSize) throw std::runtime_error("not a kgpack: file is too short");
  if (std::memcmp(raw.data(), kPackMagic, sizeof(kPackMagic)) != 0) {
    throw std::runtime_error("not a kgpack: bad magic");
  }
  Header h;
  h.revision = static_cast<uint8_t>(raw[7]);
  // A pack from before media sets is one game with its own copy of its discs.
  // Reading it would mean keeping a second layout alive for ever, so it is
  // named for what it is and the game is installed again.
  if (h.revision >= 1 && h.revision < kOldestReadableRevision) {
    throw std::runtime_error("kgpack container revision " + std::to_string(h.revision) +
                             " was made by an older kretro; install the game again");
  }
  // Anything above this build's is a body laid out by a build that came after
  // this one, and mounting it on a guess is how you hand a game the wrong
  // directory.
  if (h.revision < kOldestReadableRevision || h.revision > kContainerRevision) {
    throw std::runtime_error("kgpack container revision " + std::to_string(h.revision) +
                             " is not one this build understands (it reads " +
                             std::to_string(kOldestReadableRevision) + " to " +
                             std::to_string(kContainerRevision) + ")");
  }
  h.format_version = le::get_u16(raw, 8);
  h.flags = le::get_u16(raw, 10);
  uint16_t k = le::get_u16(raw, 12);
  if (k < 1 || k > 3) throw std::runtime_error("kgpack declares an unknown kind");
  h.kind = static_cast<PackKind>(k);
  h.meta_off = le::get_u64(raw, 16);
  h.meta_len = le::get_u64(raw, 24);
  if (h.meta_len > kMaxMetaLen) {
    throw std::runtime_error("kgpack declares an implausible amount of metadata");
  }
  h.body_off = le::get_u64(raw, 32);
  h.body_len = le::get_u64(raw, 40);
  if (h.body_len > kMaxBodyLen) {
    throw std::runtime_error("kgpack declares an implausible amount of body");
  }
  h.sig_off = le::get_u64(raw, 48);
  h.sig_len = le::get_u64(raw, 56);
  std::memcpy(h.blake3_root.data(), raw.data() + 64, h.blake3_root.size());
  if (h.has_body() && h.body_len == 0) throw std::runtime_error("kgpack claims a body but declares none");
  if (!h.has_body() && h.body_len != 0) throw std::runtime_error("kgpack declares a body it does not claim");
  return h;
}

}  // namespace kg
