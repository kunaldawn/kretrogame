// Running a built player the way a stranger would.
//
// The point of a preview is the first run: the environment check, the
// launcher, a prefix made from nothing. None of that is seen if the player
// finds kretro's state, kretro's runtime or kretro's cache, and kretro's own
// environment is full of them - the bootstrap that started kretro exported
// KRETRO_SELF, KRETRO_TOC, KRETRO_RUNTIME and the rest, and a player's
// bootstrap that inherited them would believe it had already been set up. So
// the child gets the parent's environment with every KRETRO_* taken out,
// everything that points into kretro's runtime taken out, and HOME and the
// XDG directories pointed at a scratch directory that is removed afterwards.
// The display, the session bus and XDG_RUNTIME_DIR stay: a stranger has
// those too, and fusermount3 will only mount under the last.
#pragma once

#include <sys/types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace kg::bundle {

// The child's environment, as NAME=value strings, from the parent's.
// `runtime` is kretro's runtime root (KRETRO_RUNTIME), whose paths are taken
// out of every list and whose variables are dropped; `scratch` becomes HOME.
std::vector<std::string> preview_env(const std::vector<std::string>& parent,
                                     const std::filesystem::path& runtime,
                                     const std::filesystem::path& scratch);

// This process's environment, as preview_env wants it.
std::vector<std::string> current_env();

// The scratch directory a preview of bundle `id` runs in, under `cache`. The
// preview empties it before and removes it after, recursively, so the id is
// held to the one-component rule first: the id field is free text until the
// bundle is remembered, and "preview-" + "../../x" would be a directory
// outside the cache for remove_all to take. Throws for such an id.
std::filesystem::path preview_scratch(const std::filesystem::path& cache, const std::string& id);

// One running preview: the file, its output a line at a time, and a way to
// stop it. Not copyable; the destructor stops what is still running and
// removes the scratch directory.
class Preview {
 public:
  Preview() = default;
  ~Preview();
  Preview(const Preview&) = delete;
  Preview& operator=(const Preview&) = delete;

  // Starts `file args...` in its own process group, in `scratch` (emptied
  // first), with `env`. Throws std::runtime_error when it cannot start.
  void start(const std::filesystem::path& file, const std::vector<std::string>& args,
             const std::vector<std::string>& env, const std::filesystem::path& scratch);

  // What the child has written since the last call, whole lines only until it
  // exits. Never blocks. Also notices the exit.
  std::vector<std::string> poll();

  bool running() const { return pid_ > 0; }
  bool started() const { return started_; }
  // The exit status once it has exited: -1 when killed by a signal.
  int status() const { return status_; }

  // Asks the whole process group to stop, waits a moment, then insists; then
  // removes the scratch directory.
  void stop();

 private:
  void reap(bool wait);
  void clean();

  pid_t pid_ = -1;
  int fd_ = -1;
  bool started_ = false;
  int status_ = 0;
  std::string partial_;
  std::filesystem::path scratch_;
};

}  // namespace kg::bundle
