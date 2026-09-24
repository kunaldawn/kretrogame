#include "staging.h"

#include "../util/paths.h"

namespace kg::install {
namespace fs = std::filesystem;

fs::path staging_dir(const std::string& id) { return cache_dir() / ("install-" + id); }

char staging_drive_letter(size_t disc_index) { return static_cast<char>('d' + disc_index); }

fs::path staging_drive(const fs::path& work, size_t disc_index) {
  return work / (std::string("drive-") + staging_drive_letter(disc_index));
}

fs::path staging_prefix(const fs::path& work) { return work / "prefix"; }

fs::path staging_drive_c(const fs::path& work) { return staging_prefix(work) / "drive_c"; }

std::string id_of_staging(const fs::path& work) {
  std::string n = work.filename().string();
  return n.rfind("install-", 0) == 0 ? n.substr(8) : n;
}

}  // namespace kg::install
