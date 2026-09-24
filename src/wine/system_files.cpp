#include "system_files.h"

#include <algorithm>
#include <system_error>

#include "../util/text.h"

namespace kg::wine {
namespace fs = std::filesystem;

bool is_outside_the_game(const std::string& path, const std::string& install_dir) {
  std::string p = to_lower(path);
  std::replace(p.begin(), p.end(), '\\', '/');
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  if (p.empty()) return false;

  // The scratch, first: an installer that unpacked itself into a Temp folder
  // has left behind something no game reads, and it is often the largest thing
  // in the diff.
  static const char* const kScratch[] = {"windows/temp/", "temp/", "tmp/"};
  for (const char* s : kScratch) {
    if (p.rfind(s, 0) == 0) return false;
  }
  // users/<whoever>/Temp and users/<whoever>/Local Settings/Temp, without
  // knowing what the account on this machine is called.
  if (p.rfind("users/", 0) == 0) {
    size_t slash = p.find('/', 6);
    if (slash != std::string::npos) {
      std::string rest = p.substr(slash + 1);
      if (rest.rfind("temp/", 0) == 0 || rest.rfind("local settings/temp/", 0) == 0 ||
          rest.rfind("appdata/local/temp/", 0) == 0) {
        return false;
      }
    }
  }

  std::string dir = to_lower(install_dir);
  std::replace(dir.begin(), dir.end(), '\\', '/');
  while (!dir.empty() && dir.front() == '/') dir.erase(dir.begin());
  while (!dir.empty() && dir.back() == '/') dir.pop_back();
  if (dir.empty()) return true;   // nothing is the game's, so everything travels
  if (p == dir) return false;
  return p.rfind(dir + "/", 0) != 0;
}

SystemFiles gather_system_files(const fs::path& drive_c, const std::vector<std::string>& paths,
                                const fs::path& into) {
  SystemFiles out;
  std::error_code ec;
  for (const std::string& rel : paths) {
    fs::path from = drive_c / rel;
    fs::file_status st = fs::symlink_status(from, ec);
    if (ec || !fs::exists(st)) { ec.clear(); continue; }
    fs::path to = into / rel;
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    if (fs::is_symlink(st)) {
      fs::remove(to, ec);
      ec.clear();
      fs::copy_symlink(from, to, ec);
      if (!ec) ++out.files;
      ec.clear();
      continue;
    }
    if (fs::is_directory(st)) {
      // A directory the installer made and left empty still has to exist: a
      // game that writes its config into C:\GameData on first run wants the
      // folder there. Nothing is counted for it - it is no bytes and no file.
      fs::create_directories(to, ec);
      ec.clear();
      continue;
    }
    if (!fs::is_regular_file(st)) continue;
    uint64_t sz = fs::file_size(from, ec);
    if (ec) { ec.clear(); sz = 0; }
    // A hardlink where the filesystem allows it: the staging prefix is thrown
    // away when the Build is destroyed, so these bytes need never be copied.
    fs::remove(to, ec);
    ec.clear();
    fs::create_hard_link(from, to, ec);
    if (ec) {
      ec.clear();
      fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
      if (ec) { ec.clear(); continue; }
    }
    ++out.files;
    out.bytes += sz;
  }
  return out;
}

size_t restore_system_files(const fs::path& system, const fs::path& drive_c) {
  std::error_code ec;
  if (!fs::is_directory(system, ec)) return 0;
  size_t placed = 0;
  std::error_code walk;
  for (const fs::directory_entry& de : fs::recursive_directory_iterator(system, walk)) {
    std::error_code one;
    fs::path rel = fs::relative(de.path(), system, one);
    if (one || rel.empty()) continue;
    fs::path dst = drive_c / rel;
    if (de.is_symlink(one)) {
      fs::create_directories(dst.parent_path(), one);
      fs::remove(dst, one);
      one.clear();
      fs::copy_symlink(de.path(), dst, one);
      if (!one) ++placed;
      continue;
    }
    if (de.is_directory(one)) {
      fs::create_directories(dst, one);
      continue;
    }
    fs::create_directories(dst.parent_path(), one);
    // A real copy, not a link: the body is a read-only mount and the prefix is
    // a place Wine writes.
    fs::remove(dst, one);
    one.clear();
    fs::copy_file(de.path(), dst, fs::copy_options::overwrite_existing, one);
    if (!one) ++placed;
  }
  return placed;
}

}  // namespace kg::wine
