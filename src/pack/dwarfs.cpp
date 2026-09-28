#include "dwarfs.h"

#include <stdexcept>
#include <string>
#include <system_error>

#include "../util/env.h"
#include "pack.h"

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

void extract_body_tree(const fs::path& tool, const Pack& pack, const fs::path& dir, const fs::path& scratch_image) {
  const Header& h = pack.header();
  fs::path image = pack.path();
  uint64_t offset = h.body_off;
  fs::path copied;
  std::error_code sz;
  const uint64_t whole = fs::file_size(pack.path(), sz);
  if (pack.base() != 0 || sz || h.body_off + h.body_len != whole) {
    copied = scratch_image;
    pack.extract_body(copied);
    image = copied;
    offset = 0;
  }
  ProcResult r = kg::run({tool.string(), "--tool=dwarfsextract", "-i", image.string(), "-O", std::to_string(offset),
                          "-o", dir.string()});
  std::error_code ec;
  if (!copied.empty()) fs::remove(copied, ec);
  if (!r.ok()) throw std::runtime_error("could not unpack the game:\n" + r.out);
}

}  // namespace kg
