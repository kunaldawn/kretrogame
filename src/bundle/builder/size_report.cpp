#include "size_report.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "../../util/bytes.h"
#include "../toc.h"

namespace kg::bundle {

// ---- size ---------------------------------------------------------------------------------

SizeReport size_report(uint64_t base_bytes, uint64_t meta_bytes, const std::vector<const PackFacts*>& packs) {
  SizeReport r;
  r.runtime = {"runtime", base_bytes};
  r.meta = meta_bytes;
  // build_bundle's layout exactly: the base's payloads, then each payload of
  // ours on a page of its own, then a table of 128 bytes an entry and the
  // trailer - so the number is the one the author will see in a file manager.
  std::vector<std::filesystem::path> seen;
  std::vector<size_t> part_of;
  for (const PackFacts* p : packs) {
    const std::string label = p->meta.name.empty() ? p->meta.id : p->meta.name;
    auto it = p->path.empty() ? seen.end() : std::find(seen.begin(), seen.end(), p->path);
    if (it != seen.end()) {
      r.games[part_of[static_cast<size_t>(it - seen.begin())]].label += " + " + label;
      continue;
    }
    seen.push_back(p->path);
    part_of.push_back(r.games.size());
    r.games.push_back(SizePart{label, p->bytes});
  }
  uint64_t entries = 4 + r.games.size();  // tools, runtime, app, meta
  uint64_t tail = kTocHeaderSize + kRecordSize * entries + kTrailerSize;
  uint64_t with = align_up(base_bytes, kAlign) + meta_bytes;
  for (const SizePart& s : r.games) with = align_up(with, kAlign) + s.bytes;
  r.total = with + tail;
  auto gib = [](uint64_t n) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f GiB", static_cast<double>(n) / (1ull << 30));
    return std::string(b);
  };
  if (r.total >= kWarn4G) {
    r.warnings.push_back("It is " + gib(r.total) + ", over 4 GiB: it cannot be copied to a FAT32 drive, "
                         "and GitHub Releases and itch.io's web upload will refuse it.");
  } else if (r.total >= kWarn2G) {
    r.warnings.push_back("It is " + gib(r.total) + ", over 2 GiB: GitHub Releases and itch.io's web upload "
                         "will refuse it.");
  }
  return r;
}

}  // namespace kg::bundle
