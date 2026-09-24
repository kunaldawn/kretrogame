#include "fs_ci.h"

#include <string>
#include <system_error>

#include "text.h"

namespace kg {
namespace fs = std::filesystem;

fs::path resolve_ci(const fs::path& root, std::string_view rel) {
  fs::path at = root;
  std::error_code ec;
  size_t start = 0;
  while (start <= rel.size()) {
    size_t slash = rel.find_first_of("/\\", start);
    std::string part(rel.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
    if (!part.empty()) {
      fs::path found;
      for (const fs::directory_entry& de : fs::directory_iterator(at, ec)) {
        if (to_lower(de.path().filename().string()) == to_lower(part)) {
          found = de.path();
          break;
        }
      }
      if (found.empty()) return {};
      at = found;
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return at;
}

bool exists_ci(const fs::path& root, std::string_view rel) { return !resolve_ci(root, rel).empty(); }

}  // namespace kg
