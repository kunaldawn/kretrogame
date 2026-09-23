// Recognising a disc.
//
// A collection built over twenty years contains discs of uncertain provenance,
// and a playthrough that dies forty hours in on a bad sector is the worst way
// to find out. Fingerprinting answers two questions before anything is
// installed: which release is this, and is it intact?
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../pack/kgpack.h"
#include "../rt/env.h"
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
  std::string system_id;
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

// The disc's contents, via the bundled 7z. Paths are as the disc spells them.
// A directory - a mounted CD, or a disc somebody already extracted - is walked
// directly instead: 7z exits non-zero on one, and a directory is the shape the
// headline "load game CD/DVD" path arrives in.
std::vector<std::string> list(const rt::Env& e, const std::filesystem::path& p);

// Case-insensitive lookup against a real listing. Joliet casing is not
// predictable - a disc's top-level directory may read `pc`, not `PC` - so
// nothing may trust a manifest's spelling.
std::string resolve(const std::vector<std::string>& listing, const std::string& want);

// Extracts one member to `out_dir`, keeping its name. Used for anchors and for
// the unzip recipe. A directory source is copied from, not extracted.
bool extract_member(const rt::Env& e, const std::filesystem::path& iso, const std::string& member,
                    const std::filesystem::path& out_dir, std::string* err = nullptr);

// Extracts a whole subtree of the disc into `out_dir`, flattening the subtree
// prefix away so `out_dir` becomes the game root. A directory source is copied
// recursively, into real files: what this leaves behind goes into a pack that
// has to outlive this machine.
bool extract_subtree(const rt::Env& e, const std::filesystem::path& iso, const std::string& subdir,
                     const std::filesystem::path& out_dir, std::string* err = nullptr);

// Hashes a member without writing it to disk, by reading 7z's stdout - or, for
// a directory source, the file itself.
bool hash_member(const rt::Env& e, const std::filesystem::path& iso, const std::string& member,
                 Hash* out, uint64_t* size);

DiscFingerprint fingerprint(const Info& info);

// An entry in the shipped disc database.
struct Known {
  Hash prefix{};
  uint64_t size = 0;
  std::string volume_id;
  std::string set_id;   // which multi-disc set this belongs to, if any
  int disc_no = 1;      // its position in that set
  std::string name;
};

// Loads db/discs.txt from the runtime, or from the checkout when running from
// one. Missing is not an error - it only means we cannot name a disc.
std::vector<Known> load_database(const rt::Env& e);

// Parses one database file. Split out from load_database so it can be tested
// without a runtime. Accepts both the four-column v1 rows and the six-column
// v2 rows; a v1 row is disc 1 of an unnamed set.
std::vector<Known> load_database_file(const std::filesystem::path& p);

// Names the disc, or an empty optional. Matching is by size and prefix hash;
// a size match with the right volume id but a different hash is reported
// separately, because that is what a bad dump looks like.
struct Match {
  const Known* entry = nullptr;
  bool exact = false;
  bool suspect_bad_dump = false;
};
Match identify(const std::vector<Known>& db, const Info& info);

}  // namespace kg::iso
