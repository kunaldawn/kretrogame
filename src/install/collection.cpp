#include "collection.h"

#include <system_error>

#include "../disc/iso.h"
#include "../util/env.h"
#include "../util/hash.h"
#include "../util/paths.h"
#include "../util/text.h"

namespace kg::install {
namespace fs = std::filesystem;

fs::path iso_dir() {
  if (const char* d = env_nonempty("KRETRO_ISO_DIR")) return d;
  fs::path in_state = state_dir() / "iso";
  std::error_code ec;
  if (fs::exists(in_state, ec)) return in_state;
  return "iso";
}

bool is_collection_name(const std::string& name) {
  if (name.empty() || name.size() > 1024) return false;
  fs::path p(name);
  // `dir / name` throws `dir` away when `name` is absolute, which is the whole
  // of the hole: a recipe naming /usr/bin/anything is a recipe that chose a
  // file on the importer's machine rather than one in his collection, and for
  // installer_exe that file is then run under Wine. ".." is the same trick with
  // one more step.
  if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
  for (const fs::path& part : p.lexically_normal()) {
    if (part == "..") return false;
  }
  return true;
}

fs::path find_iso(const std::string& name) {
  // A disc reference is a name in the collection directory. Nothing else is a
  // disc reference, however plausible it looks.
  if (!is_collection_name(name)) return {};
  std::error_code ec;
  fs::path dir = iso_dir();
  fs::path direct = dir / name;
  if (fs::exists(direct, ec)) return direct;
  if (!fs::exists(dir, ec)) return {};
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    if (to_lower(de.path().filename().string()) == to_lower(name)) return de.path();
  }
  return {};
}

fs::path find_iso_by_fingerprint(const DiscFingerprint& want) {
  std::error_code ec;
  fs::path dir = iso_dir();
  if (!fs::exists(dir, ec)) return {};
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    if (!de.is_regular_file(ec)) continue;
    // Size first: it costs a stat and rules out almost everything.
    if (want.size && fs::file_size(de.path(), ec) != want.size) continue;
    iso::Info info = iso::scan(de.path(), /*full=*/false);
    if (!want.volume_id.empty() && info.volume_id != want.volume_id) continue;
    if (want.size == 0) continue;
    // The recorded hash may be of the whole image or of the prefix, depending
    // on how the pack was made; a prefix match is enough to select a
    // candidate, and the tree hash is what actually proves the result.
    if (info.prefix == want.blake3) return de.path();
    if (hash_file(de.path()) == want.blake3) return de.path();
  }
  return {};
}

}  // namespace kg::install
