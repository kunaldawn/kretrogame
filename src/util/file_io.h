// Whole files and file descriptors: the small pieces of file handling that
// several places need done the same way.
#pragma once

#include <sys/types.h>

#include <filesystem>
#include <string>

namespace kg {

// An open file descriptor, closed when it goes. Opening with O_CLOEXEC, so a
// child a session starts never inherits it. Throws std::runtime_error naming
// the file and the reason when the open fails.
class Fd {
 public:
  Fd(const std::filesystem::path& p, int flags, mode_t mode = 0);
  ~Fd() { close(); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }
  // Returns whether the close itself failed, which on some filesystems is where
  // a full disk is first admitted.
  bool close();

 private:
  int fd_;
};

// The whole of a file, or an empty string if it cannot be read.
std::string read_file_or_empty(const std::filesystem::path& p);

// Writes `bytes` to a temporary of this writer's own beside `file` and renames
// it over, so a crash leaves the old file and two writers at once each leave
// a whole one. Throws std::runtime_error naming the file.
void write_atomically(const std::filesystem::path& file, const std::string& bytes);

}  // namespace kg
