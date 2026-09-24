#include "safe_names.h"

#include <algorithm>
#include <filesystem>
#include <string>

namespace kg {
namespace fs = std::filesystem;

bool id_is_safe(std::string_view id) {
  // One path component and a conservative one: letters, digits, dash,
  // underscore and dot, starting with a letter or a digit. That admits every id
  // slug() has ever produced and every id anybody has typed into the wizard,
  // and it admits nothing that is a separator, nothing that is "." or "..",
  // and nothing that begins with a dot and hides.
  if (id.empty() || id.size() > 100) return false;
  for (char c : id) {
    if (c >= 'a' && c <= 'z') continue;
    if (c >= 'A' && c <= 'Z') continue;
    if (c >= '0' && c <= '9') continue;
    if (c == '-' || c == '_' || c == '.') continue;
    return false;
  }
  char first = id.front();
  return (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
         (first >= '0' && first <= '9');
}

bool install_dir_is_safe(std::string_view dir) {
  if (dir.empty()) return true;         // no install directory recorded; nothing is linked
  if (dir.size() > 1024) return false;
  // A path recorded on Windows spells itself with backslashes, and drive_c is
  // reached through a POSIX join, so the separators are made one kind first -
  // otherwise "..\..\.bashrc" is one innocent-looking component.
  std::string s(dir);
  std::replace(s.begin(), s.end(), '\\', '/');
  fs::path p(s);
  if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
  fs::path norm = p.lexically_normal();
  for (const fs::path& part : norm) {
    if (part == "..") return false;
  }
  return !norm.empty() && norm != ".";
}

}  // namespace kg
