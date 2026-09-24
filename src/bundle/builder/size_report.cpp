#include "size_report.h"

#include <cstdio>
#include <utility>

#include "../../util/bytes.h"
#include "../toc.h"

namespace kg::bundle {

// ---- size ---------------------------------------------------------------------------------

SizeReport size_report(uint64_t base_bytes, uint64_t meta_bytes, const std::vector<const PackFacts*>& packs) {
  SizeReport r;
  r.runtime = {"runtime", base_bytes, base_bytes};
  r.meta = meta_bytes;
  // build_bundle's layout exactly: the base's payloads, then each payload of
  // ours on a page of its own, then a table of 128 bytes an entry and the
  // trailer - so the number is the one the author will see in a file manager.
  uint64_t entries = 4 + packs.size();  // tools, runtime, app, meta
  uint64_t tail = kTocHeaderSize + kRecordSize * entries + kTrailerSize;
  uint64_t with = align_up(base_bytes, kAlign) + meta_bytes, without = with;
  for (const PackFacts* p : packs) {
    SizePart s{p->meta.name.empty() ? p->meta.id : p->meta.name, p->bytes, p->without_discs};
    with = align_up(with, kAlign) + s.bytes;
    without = align_up(without, kAlign) + s.without_discs;
    r.games.push_back(std::move(s));
  }
  r.total = with + tail;
  r.total_without_discs = without + tail;
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
  if (!r.warnings.empty() && r.total_without_discs < r.total) {
    uint64_t limit = r.total >= kWarn4G ? kWarn4G : kWarn2G;
    if (r.total_without_discs < limit) {
      r.warnings.push_back("Without their discs the games would come to about " + gib(r.total_without_discs) +
                           ": installing them again without \"include the discs\" would bring it under.");
    }
  }
  return r;
}

}  // namespace kg::bundle
