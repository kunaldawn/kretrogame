#include "dwarfs.h"

#include "../util/env.h"

namespace kg {
namespace fs = std::filesystem;

fs::path dwarfs_tool() {
  if (const char* d = env_nonempty("KRETRO_DWARFS")) return d;
  return {};
}

ProcResult mkdwarfs_body(const fs::path& tool, const fs::path& in, const fs::path& out) {
  return kg::run({tool.string(), "--tool=mkdwarfs", "-i", in.string(), "-o", out.string(), "--categorize",
                  "-S", "22", "--log-level=error", "--no-progress", "-f"});
}

}  // namespace kg
