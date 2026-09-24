#include "internal.h"

#include <unistd.h>

#include "../util/proc.h"
#include "../util/text.h"

namespace kg::session::detail {
namespace fs = std::filesystem;

void unmount(const fs::path& mountpoint, Unmount how) {
  if (how != Unmount::Lazy) kg::run({"fusermount3", "-u", mountpoint.string()});
  if (how != Unmount::Plain) kg::run({"fusermount3", "-u", "-z", mountpoint.string()});
}

bool wait_for(const std::function<bool()>& cond, int tries, int ms) {
  for (int i = 0; i < tries; ++i) {
    if (cond()) return true;
    usleep(static_cast<useconds_t>(ms) * 1000);
  }
  return false;
}

// Matches a name in a directory ignoring case. Wine is case-insensitive and
// the manifests record real case for the reader, so neither may be trusted to
// agree with the filesystem.
// Unlike kg::resolve_ci this matches one component and returns only the name,
// so a `want` with a '/' in it finds nothing.
std::string find_ci_in_dir(const fs::path& dir, const std::string& want) {
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    if (to_lower(de.path().filename().string()) == to_lower(want)) return de.path().filename().string();
  }
  return "";
}

}  // namespace kg::session::detail
