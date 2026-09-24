#include "iso.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace kg::iso {
namespace fs = std::filesystem;

namespace {

std::string rtrim(const std::string& s) {
  size_t b = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? "" : s.substr(0, b + 1);
}

// ISO 9660 puts the primary volume descriptor in sector 16, and every sector
// is 2048 bytes. Multiplied as 64-bit: the product, 32768, fits an int with
// room to spare, so the wider type changes nothing but the lint's reading.
constexpr uint64_t kPvdOffset = uint64_t{16} * 2048;
static_assert(kPvdOffset == 32768);

std::string field(const unsigned char* pvd, size_t off, size_t len) {
  return rtrim(std::string(reinterpret_cast<const char*>(pvd + off), len));
}

}  // namespace

Info scan(const fs::path& p, bool full) {
  Info info;
  info.path = p;
  std::error_code ec;
  info.size = fs::file_size(p, ec);
  if (ec) throw std::runtime_error("cannot stat " + p.string());

  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open " + p.string());
  unsigned char pvd[2048] = {};
  if (std::fseek(f, static_cast<long>(kPvdOffset), SEEK_SET) == 0) {
    if (std::fread(pvd, 1, sizeof(pvd), f) == sizeof(pvd)) {
      // type 1 (primary) followed by the "CD001" standard identifier
      if (pvd[0] == 1 && std::memcmp(pvd + 1, "CD001", 5) == 0) {
        info.valid_iso9660 = true;
        info.volume_id = field(pvd, 40, 32);
        info.publisher = field(pvd, 318, 128);
        info.created = field(pvd, 813, 16);
      }
    }
  }
  std::fclose(f);

  info.prefix = hash_file_prefix(p, kPrefixBytes);
  if (full) {
    info.whole = hash_file(p);
    info.has_whole = true;
  }
  return info;
}

DiscFingerprint fingerprint(const Info& info) {
  DiscFingerprint f;
  f.filename = info.path.filename().string();
  f.size = info.size;
  f.blake3 = info.has_whole ? info.whole : info.prefix;
  f.volume_id = info.volume_id;
  f.created = info.created;
  return f;
}

}  // namespace kg::iso
