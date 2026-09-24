// Recognising a disc.
//
// A collection built over twenty years contains discs of uncertain provenance,
// and a playthrough that dies forty hours in on a bad sector is the worst way
// to find out. Fingerprinting answers two questions before anything is
// installed: which release is this, and is it intact?
//
// What is on the disc is in members.h, and naming it against the shipped
// database is in database.h.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "../pack/kgpack.h"
#include "../util/hash.h"

namespace kg::iso {

// How much of the image the quick fingerprint reads. Enough to be decisive
// between different masterings, cheap enough to run on a whole directory of
// discs without anyone noticing.
inline constexpr uint64_t kPrefixBytes = 64ull << 20;

struct Info {
  std::filesystem::path path;
  uint64_t size = 0;
  std::string volume_id;   // from the primary volume descriptor
  std::string publisher;
  std::string created;     // as recorded on the disc
  Hash prefix{};           // BLAKE3 of the first kPrefixBytes
  Hash whole{};            // BLAKE3 of the whole image; only if asked for
  bool has_whole = false;
  bool valid_iso9660 = false;
};

// Reads the volume descriptor and the quick fingerprint. `full` additionally
// hashes the entire image, which for a 3.6 GB disc takes a few seconds.
Info scan(const std::filesystem::path& p, bool full = false);

DiscFingerprint fingerprint(const Info& info);

}  // namespace kg::iso
