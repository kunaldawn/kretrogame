#include "state_dir.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "../util/env.h"
#include "../util/paths.h"

namespace kg::player {
namespace fs = std::filesystem;

fs::path portable_dir(const fs::path& exe) {
  return exe.parent_path() / (exe.filename().string() + "-data");
}

bool writable_dir(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
  // access(W_OK) answers from the mode bits and says yes on a read-only
  // mount of a directory those bits allow; only a write says no there.
  fs::path probe = dir / (".kretro-write-test-" + std::to_string(getpid()));
  int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  ::close(fd);
  ::unlink(probe.c_str());
  return true;
}

bool holds_links(const fs::path& dir) {
  // A prefix's drives are symbolic links - dosdevices/c: is ../drive_c - and
  // FAT and exFAT, what most USB sticks come formatted with, have none: the
  // kernel says EPERM. Taken as the state, such a directory passes every
  // other test and then fails every game's first launch.
  fs::path probe = dir / (".kretro-link-test-" + std::to_string(getpid()));
  if (::symlink(".", probe.c_str()) != 0) return false;
  ::unlink(probe.c_str());
  return true;
}

StateChoice choose_state(const fs::path& exe, const std::string& bundle_id,
                         const std::string& xdg_data_home, const std::string& home, uid_t me) {
  StateChoice c;
  std::error_code ec;
  fs::path portable = portable_dir(exe);
  if (!exe.empty() && fs::is_directory(portable, ec)) {
    // The directory it names, followed through a link: whose it is decides
    // whose prefix and whose HOME the game is given.
    struct stat st {};
    if (::stat(portable.c_str(), &st) == 0 && st.st_uid != me) {
      c.refused_why = "belongs to another user";
    } else if (!writable_dir(portable)) {
      c.refused_why = "cannot be written";
    } else if (!holds_links(portable)) {
      c.refused_why = "is on a disk that cannot hold symbolic links (FAT or exFAT), which a Wine prefix is "
                      "made of";
    } else {
      c.dir = portable;
      c.portable = true;
      return c;
    }
    c.refused_portable = portable;
  }
  // XDG says a relative XDG_DATA_HOME is invalid and is to be ignored.
  if (!xdg_data_home.empty() && fs::path(xdg_data_home).is_absolute()) {
    c.dir = fs::path(xdg_data_home) / bundle_id;
  } else if (!home.empty()) {
    c.dir = fs::path(home) / ".local" / "share" / bundle_id;
  } else {
    c.private_root = fs::path("/tmp") / ("kretro-" + std::to_string(me));
    c.dir = c.private_root / bundle_id;
  }
  return c;
}

void claim_private_dir(const fs::path& dir, uid_t me) {
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    throw std::runtime_error("cannot make " + dir.string() + ": " + std::strerror(errno));
  }
  // lstat: a link there is somebody pointing us somewhere, whatever it points at.
  struct stat st {};
  if (::lstat(dir.c_str(), &st) != 0) {
    throw std::runtime_error("cannot look at " + dir.string() + ": " + std::strerror(errno));
  }
  if (!S_ISDIR(st.st_mode) || st.st_uid != me) {
    throw std::runtime_error(dir.string() + " belongs to someone else, and this player keeps its saves "
                             "there when there is no home directory. Remove it, or set HOME.");
  }
  if ((st.st_mode & 077) != 0 && ::chmod(dir.c_str(), 0700) != 0) {
    throw std::runtime_error("cannot make " + dir.string() + " private: " + std::strerror(errno));
  }
}

StateChoice settle_state(const Bundle& b, const fs::path& exe) {
  StateChoice c = choose_state(exe, b.meta.id, env_nonempty("XDG_DATA_HOME") ? env_nonempty("XDG_DATA_HOME") : "",
                               env_nonempty("HOME") ? env_nonempty("HOME") : "");
  setenv("KRETRO_STATE", c.dir.c_str(), 1);
  use_bundle_layout(b.meta.id);
  if (!c.private_root.empty()) claim_private_dir(c.private_root);
  std::error_code ec;
  fs::create_directories(c.dir, ec);
  if (ec) {
    throw std::runtime_error("cannot make a place for this player's saves at " + c.dir.string() + ": " +
                             ec.message());
  }
  return c;
}

StateChoice inherited_state(const Bundle& b, const fs::path& exe) {
  const char* s = env_nonempty("KRETRO_STATE");
  if (!s) return settle_state(b, exe);
  StateChoice c;
  c.dir = s;
  use_bundle_layout(b.meta.id);
  return c;
}

}  // namespace kg::player
