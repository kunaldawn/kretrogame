#include "files.h"

#include <fstream>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace kg::gpu {
namespace fs = std::filesystem;

namespace {
fs::path temporary_beside(const fs::path& p) {
  fs::path t = p;
  t += "." + std::to_string(::getpid()) + ".tmp";
  return t;
}
}  // namespace

void replace_file(const fs::path& dst, const std::string& bytes) {
  const fs::path tmp = temporary_beside(dst);
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << bytes;
    if (!f) {
      fs::remove(tmp, ec);
      return;
    }
  }
  fs::rename(tmp, dst, ec);
  if (ec) fs::remove(tmp, ec);
}

void replace_symlink(const fs::path& target, const fs::path& link) {
  const fs::path tmp = temporary_beside(link);
  std::error_code ec;
  fs::remove(tmp, ec);
  fs::create_symlink(target, tmp, ec);
  if (ec) return;
  fs::rename(tmp, link, ec);
  if (ec) fs::remove(tmp, ec);
}

void keep_only(const fs::path& dir, const std::set<std::string>& names) {
  std::error_code ec;
  std::vector<fs::path> gone;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    const std::string n = de.path().filename().string();
    // Another start's temporaries, about to be renamed into place.
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tmp") == 0) continue;
    if (!names.count(n)) gone.push_back(de.path());
  }
  for (const fs::path& p : gone) fs::remove_all(p, ec);
}

}  // namespace kg::gpu
