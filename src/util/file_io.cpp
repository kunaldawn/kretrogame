#include "file_io.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace kg {
namespace fs = std::filesystem;

Fd::Fd(const fs::path& p, int flags, mode_t mode) : fd_(::open(p.c_str(), flags | O_CLOEXEC, mode)) {
  if (fd_ < 0) throw std::runtime_error("cannot open " + p.string() + ": " + std::strerror(errno));
}

bool Fd::close() {
  if (fd_ < 0) return true;
  int r = ::close(fd_);
  fd_ = -1;
  return r == 0;
}

std::string read_file_or_empty(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

void write_atomically(const fs::path& file, const std::string& bytes) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  // A temporary of this writer's own. One fixed <file>.tmp was shared by
  // every copy of the player on one state - two launchers, a launcher and a
  // terminal - and the second to rename found it gone and said "cannot
  // write", or the first renamed the other's half-written bytes into place.
  // mkstemp's name is new, and O_EXCL: nothing already there is followed.
  std::string tmpl = file.string() + ".XXXXXX";
  int fd = ::mkstemp(tmpl.data());
  if (fd < 0) throw std::runtime_error("cannot write " + file.string() + ": " + std::strerror(errno));
  const fs::path tmp = tmpl;
  bool ok = ::fchmod(fd, 0644) == 0;
  for (size_t done = 0; ok && done < bytes.size();) {
    ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) ok = false;
    else done += static_cast<size_t>(n);
  }
  if (::close(fd) != 0) ok = false;
  if (ok) fs::rename(tmp, file, ec);
  if (!ok || ec) {
    fs::remove(tmp, ec);
    throw std::runtime_error("cannot write " + file.string());
  }
}

}  // namespace kg
