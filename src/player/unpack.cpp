#include "unpack.h"

#include <sys/statvfs.h>

#include <system_error>

namespace kg::player {
namespace fs = std::filesystem;

uint64_t unpacked_estimate(const SetMeta& s) {
  uint64_t n = 0;
  for (const Meta& g : s.games) n += g.tree.total_bytes() + g.system.bytes;
  // Each disc once, its tree and audio as the pack recorded them when it laid
  // them out.
  for (const Meta::Disc& d : s.discs) n += d.bytes;
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
