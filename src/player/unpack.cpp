#include "unpack.h"

#include <sys/statvfs.h>

#include <system_error>

namespace kg::player {
namespace fs = std::filesystem;

uint64_t unpacked_estimate(const Meta& m) {
  uint64_t n = m.tree.total_bytes() + m.system.bytes;
  bool carries_discs = false;
  for (const Meta::Disc& d : m.discs) carries_discs = carries_discs || d.embedded;
  // A carried disc's tree is about its image's size; the fingerprints are
  // the only sizes the pack records for them.
  if (carries_discs) {
    for (const DiscFingerprint& f : m.recipe.fingerprints) n += f.size;
  }
  return n;
}

uint64_t free_bytes(const fs::path& p) {
  fs::path at = p;
  std::error_code ec;
  while (!at.empty() && !fs::exists(at, ec)) {
    fs::path up = at.parent_path();
    if (up == at) break;
    at = up;
  }
  struct statvfs s {};
  if (::statvfs(at.empty() ? "/" : at.c_str(), &s) != 0) return 0;
  return static_cast<uint64_t>(s.f_bavail) * s.f_frsize;
}

}  // namespace kg::player
