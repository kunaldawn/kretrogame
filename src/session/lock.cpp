#include "lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "../util/paths.h"

namespace kg::session {
namespace fs = std::filesystem;

fs::path lock_file(const std::string& id) { return game_saves_dir(id) / "lock"; }

GameLock::~GameLock() { release(); }

GameLock::GameLock(GameLock&& other) noexcept : fd_(other.fd_), busy_(other.busy_) {
  other.fd_ = -1;
}

GameLock& GameLock::operator=(GameLock&& other) noexcept {
  if (this != &other) {
    release();
    fd_ = other.fd_;
    busy_ = other.busy_;
    other.fd_ = -1;
  }
  return *this;
}

void GameLock::release() {
  if (fd_ >= 0) {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
  }
}

GameLock lock_game(const std::string& id) { return lock_path(lock_file(id)); }

GameLock lock_path(const fs::path& file) {
  GameLock l;
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  // O_CLOEXEC: the game, the compositor and the screenshooter are all forked
  // from here, and a descriptor that survived into one of them would keep the
  // lock alive after this process had let it go.
  int fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return l;  // no lock file, so no lock, and no accusation either
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    l.busy_ = true;
    return l;
  }
  l.fd_ = fd;
  return l;
}

}  // namespace kg::session
