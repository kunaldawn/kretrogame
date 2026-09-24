#include "player_base.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "../../util/proc.h"

namespace kg::bundle {
namespace fs = std::filesystem;

// ---- the player base ----------------------------------------------------------------

uint64_t player_base_bytes(const BaseSource& b) { return read_base_toc(b).payload_end(); }

BaseSource find_player_base(const fs::path& self) {
  if (const char* p = std::getenv("KRETRO_PLAYER_BASE"); p && *p) {
    // A player base on its own, as `make player-base` links one: the whole file.
    return BaseSource::whole_file(fs::path(p));
  }
  static const std::string how =
      " A development build is linked without one when build/player-base is missing: run "
      "'make player-base', then 'make', and use the build/kretro that makes - or point "
      "KRETRO_PLAYER_BASE at a player base.";
  if (self.empty()) {
    throw std::runtime_error("kretro cannot find its own file (KRETRO_SELF is not set), so it has no player "
                             "base to build from: it was started outside its bootstrap." + how);
  }
  Toc t;
  try {
    t = read_toc(self);
  } catch (const std::exception& ex) {
    throw std::runtime_error("kretro cannot read its own file " + self.string() + " (" + ex.what() +
                             "), so it has no player base to build from." + how);
  }
  if (!t.find(Kind::PlayerBase)) {
    throw std::runtime_error("This kretro carries no player base, so it cannot build players." + how);
  }
  return extract_player_base(self);
}

namespace {

// `len` bytes at `off` in `from`, into a new file `to`. copy_file_range lets a
// filesystem that can share blocks share them, so a copy of a 300 MB runtime
// out of a player base costs next to nothing on btrfs or XFS - the entries are
// page-aligned, which is what a shared extent needs - and a kernel-side copy
// elsewhere.
bool copy_range(const fs::path& from, uint64_t off, uint64_t len, const fs::path& to) {
  int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return false;
  int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    ::close(in);
    return false;
  }
  loff_t src = static_cast<loff_t>(off);
  uint64_t left = len;
  bool ok = true;
  while (left > 0) {
    ssize_t n = ::copy_file_range(in, &src, out, nullptr, left, 0);
    if (n <= 0) {
      // No copy_file_range here (an old kernel, a filesystem that refuses it
      // across files): the ordinary way, from where it stopped.
      std::vector<char> buf(1 << 20);
      while (left > 0) {
        ssize_t r = ::pread(in, buf.data(), std::min<uint64_t>(left, buf.size()), src);
        if (r <= 0 || ::write(out, buf.data(), static_cast<size_t>(r)) != r) {
          ok = false;
          break;
        }
        src += r;
        left -= static_cast<uint64_t>(r);
      }
      break;
    }
    left -= static_cast<uint64_t>(n);
  }
  ::close(in);
  if (::close(out) != 0) ok = false;
  return ok;
}

}  // namespace

std::vector<std::string> base_licenses(const BaseSource& b, const fs::path& tool, const fs::path& cache,
                                       bool compute) {
  std::vector<std::string> out;
  std::error_code ec;
  Toc t;
  try {
    t = read_base_toc(b);
  } catch (const std::exception&) {
    return out;
  }
  const Entry* rt = t.find(Kind::Runtime);
  if (!rt || cache.empty()) return out;
  const fs::path memo = cache / ("player-licenses-" + to_hex(rt->blake3) + ".txt");
  if (std::ifstream f{memo}) {
    for (std::string line; std::getline(f, line);) {
      if (!line.empty()) out.push_back(line);
    }
    return out;
  }
  if (!compute || tool.empty() || !fs::exists(tool, ec)) return out;

  fs::create_directories(cache, ec);
  const fs::path image = cache / ("player-runtime-" + to_hex(rt->blake3) + ".image");
  if (!copy_range(b.path, t.at(*rt), rt->len, image)) {
    fs::remove(image, ec);
    return out;
  }
  ProcResult r = kg::run({tool.string(), "--tool=dwarfsck", "-i", image.string(), "-l", "--log-level=error"});
  fs::remove(image, ec);
  if (!r.ok()) return out;
  const std::string dir = "usr/share/kretro/licenses/";
  std::istringstream in(r.out);
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.rfind(dir, 0) != 0) continue;
    std::string n = line.substr(dir.size());
    // Only what is directly in the directory; common-licenses is Debian's
    // full texts, which the others point into, and is not a notice itself.
    if (n.empty() || n.find('/') != std::string::npos || n == "common-licenses") continue;
    out.push_back(n);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  if (out.empty()) return out;
  const fs::path tmp = fs::path(memo).concat(".new");
  {
    std::ofstream f(tmp, std::ios::trunc);
    for (const std::string& n : out) f << n << "\n";
  }
  fs::rename(tmp, memo, ec);
  if (ec) fs::remove(tmp, ec);
  return out;
}

}  // namespace kg::bundle
