#include "saves.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "../util/paths.h"
#include "lock.h"
#include "saves_layout.h"

namespace kg::session {
namespace fs = std::filesystem;

bool link_tree(const fs::path& from, const fs::path& to, size_t* files) {
  std::error_code ec;
  bool ok = true;
  fs::create_directories(to, ec);
  if (ec) return false;
  auto it = fs::recursive_directory_iterator(from, ec);
  if (ec) return false;
  for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
    // A directory this walk cannot descend into - it went away underneath us,
    // or it is not ours to read - ends the walk. Whatever is under it is not
    // in `to`, and saying so is the whole point of the return value. The same
    // check again below the loop, because the increment that fails is also the
    // increment that reaches the end: the loop condition is tested before this
    // line is, and an error found on the way out would otherwise be read as a
    // walk that finished.
    if (ec) return false;
    std::error_code e;
    fs::path rel = fs::relative(it->path(), from, e);
    if (e) { ok = false; continue; }
    fs::path dst = to / rel;
    const bool dir = it->is_directory(e);
    if (e) { ok = false; continue; }
    if (dir) {
      fs::create_directories(dst, e);
      if (e) ok = false;
      continue;
    }
    fs::create_directories(dst.parent_path(), e);
    e.clear();
    fs::create_hard_link(it->path(), dst, e);
    if (e) {  // different filesystem, or a symlink
      e.clear();
      fs::copy(it->path(), dst,
               fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks, e);
      // Not counted, and not survivable: a generation missing a file is not
      // the state the game was in, and a caller about to delete the only other
      // copy has to hear about it.
      if (e) { ok = false; continue; }
    }
    if (files) ++*files;
  }
  if (ec) return false;
  return ok;
}

fs::path snapshot(const std::string& id, const fs::path& upper) {
  std::error_code ec;
  if (!fs::exists(upper, ec) || fs::is_empty(upper, ec)) return {};

  fs::create_directories(generations_dir(id), ec);
  int next = 0;
  for (const fs::directory_entry& de : fs::directory_iterator(generations_dir(id), ec)) {
    next = std::max(next, std::atoi(de.path().filename().c_str()));
  }
  char name[16];
  std::snprintf(name, sizeof(name), "%04d", next + 1);
  fs::path dst = generations_dir(id) / name;
  size_t files = 0;
  if (!link_tree(upper, dst, &files)) {
    // Half a snapshot is worse than none: it looks like a generation, it is
    // offered as one, and restoring it would put the game back into a state it
    // was never in. So it is removed and the caller told - which matters most
    // to the caller that takes a snapshot precisely so that it may then delete
    // something.
    fs::remove_all(dst, ec);
    throw std::runtime_error("could not snapshot what " + id + " has written");
  }
  return files ? dst : fs::path{};
}

std::vector<std::string> generations(const std::string& id) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(generations_dir(id), ec)) {
    if (de.is_directory(ec)) out.push_back(de.path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

void restore(const std::string& id, const std::string& generation) {
  std::error_code ec;
  fs::path src = generations_dir(id) / generation;
  if (!fs::exists(src, ec)) throw std::runtime_error("no such generation: " + generation);
  // The live directory this is about to replace is the upper layer of a
  // running session's overlay, if there is one. Replacing it underneath a
  // playing game loses whatever that game has written since it started and
  // leaves it writing into a directory that is no longer there.
  GameLock lock = lock_game(id);
  if (lock.busy()) {
    throw std::runtime_error(id + " is being played by another kretro right now, and its "
                                  "saves are what would be replaced. Close that one first.");
  }
  fs::path live = live_dir(id);
  // The current state becomes a generation of its own first, so restoring is
  // never the move that loses something. This throws when that copy did not
  // complete, and it has to: the whole reason it is taken is that what comes
  // next may be the only other copy.
  snapshot(id, live);

  // Assembled beside the live directory and moved into place only once every
  // file has arrived. Deleting the saves and then copying would let a
  // generation on a filesystem that has just filled up take the live saves
  // with it and leave whatever part of itself had landed.
  fs::path staged = game_saves_dir(id) / "restoring";
  fs::remove_all(staged, ec);
  size_t n = 0;
  if (!link_tree(src, staged, &n)) {
    fs::remove_all(staged, ec);
    throw std::runtime_error("could not lay out " + generation + "; nothing was replaced");
  }

  // Two renames within one directory, so there is no moment where neither copy
  // exists. If the second fails the first is undone and the live saves are
  // exactly where they were.
  fs::path replaced = game_saves_dir(id) / "restoring.old";
  fs::remove_all(replaced, ec);
  ec.clear();
  const bool had_live = fs::exists(live, ec);
  if (had_live) {
    fs::rename(live, replaced, ec);
    if (ec) {
      // Read before the cleanup, which has an error_code of its own to report
      // and would otherwise overwrite the one being complained about.
      const std::string why = ec.message();
      std::error_code back;
      fs::remove_all(staged, back);
      throw std::runtime_error("could not move the live saves aside: " + why);
    }
  }
  ec.clear();
  fs::rename(staged, live, ec);
  if (ec) {
    const std::string why = ec.message();
    std::error_code back;
    if (had_live) fs::rename(replaced, live, back);
    fs::remove_all(staged, back);
    throw std::runtime_error("could not put " + generation + " in place: " + why);
  }
  fs::remove_all(replaced, ec);
}

}  // namespace kg::session
