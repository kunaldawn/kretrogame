#include "pe.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>

#include "text.h"

namespace kg::pe {
namespace fs = std::filesystem;

namespace {

// Real executables have a handful of sections and a few dozen imports. The
// caps are far above that and far below what a hostile header could ask for,
// so a crafted file costs a bounded amount of work however it lies.
constexpr uint32_t kMaxSections = 96;
constexpr uint32_t kMaxImports = 4096;
constexpr size_t kMaxName = 256;
constexpr uint64_t kMaxFile = 512ull << 20;

// Every read goes through here. The arithmetic is done in 64 bits, so an
// offset near 4 GiB plus a length cannot wrap round to something small and
// look as though it fits.
struct Buf {
  const uint8_t* p;
  uint64_t n;
  bool has(uint64_t off, uint64_t len) const { return off <= n && len <= n - off; }
  uint16_t u16(uint64_t off) const { return uint16_t(p[off] | p[off + 1] << 8); }
  uint32_t u32(uint64_t off) const {
    return uint32_t(p[off]) | uint32_t(p[off + 1]) << 8 | uint32_t(p[off + 2]) << 16 |
           uint32_t(p[off + 3]) << 24;
  }
};

struct Section {
  uint32_t va, vsize, raw_size, raw_off;
};

// An RVA is an address in the loaded image; the file is laid out differently,
// section by section. Finds the file offset the loader would have mapped to
// this address, or fails if no section's file bytes cover it.
bool rva_to_off(const std::vector<Section>& secs, uint32_t headers_size, const Buf& b,
                uint32_t rva, uint64_t* off) {
  for (const Section& s : secs) {
    uint64_t span = std::max(s.vsize, s.raw_size);
    if (rva >= s.va && uint64_t(rva) < uint64_t(s.va) + span) {
      uint64_t delta = uint64_t(rva) - s.va;
      // Past the raw data is the zero-filled tail of the section: it exists in
      // memory but not in the file, and nothing a name could be read from.
      if (delta >= s.raw_size) return false;
      *off = uint64_t(s.raw_off) + delta;
      return *off < b.n;
    }
  }
  // Addresses inside the headers map one to one; some packers put the import
  // table there.
  if (rva < headers_size && rva < b.n) {
    *off = rva;
    return true;
  }
  return false;
}

std::string strip_dll(std::string s) {
  s = to_lower(s);
  if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0) s.resize(s.size() - 4);
  return s;
}

// A GUID as it sits in memory: the first three fields little-endian, the last
// eight bytes as written.
std::array<uint8_t, 16> guid(uint32_t d1, uint16_t d2, uint16_t d3,
                             std::array<uint8_t, 8> d4) {
  std::array<uint8_t, 16> g{};
  for (int i = 0; i < 4; ++i) g[i] = uint8_t(d1 >> (8 * i));
  for (int i = 0; i < 2; ++i) g[4 + i] = uint8_t(d2 >> (8 * i));
  for (int i = 0; i < 2; ++i) g[6 + i] = uint8_t(d3 >> (8 * i));
  for (int i = 0; i < 8; ++i) g[8 + i] = d4[i];
  return g;
}

// IID_IDirect3D, 2, 3 and 7, from d3d.h. Direct3D 5 is IDirect3D2 and 6 is
// IDirect3D3; there was never an IDirect3D4 through 6.
const std::array<std::array<uint8_t, 16>, 4>& d3d_iids() {
  static const std::array<std::array<uint8_t, 16>, 4> ids = {
      guid(0x3bba0080, 0x2421, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}),
      guid(0x6aae1ec1, 0x662a, 0x11d0, {0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a}),
      guid(0xbb223240, 0xe72b, 0x11d0, {0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e}),
      guid(0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}),
  };
  return ids;
}

bool mentions_d3d_iid(const uint8_t* data, size_t size) {
  for (const auto& id : d3d_iids()) {
    if (std::search(data, data + size, id.begin(), id.end()) != data + size) return true;
  }
  return false;
}

}  // namespace

bool Imports::imports(const std::string& dll) const {
  std::string want = strip_dll(dll);
  for (const std::string& d : dlls) {
    if (strip_dll(d) == want) return true;
  }
  return false;
}

Imports parse(const uint8_t* data, size_t size) {
  Imports r;
  Buf b{data, size};
  auto fail = [&](const char* why) {
    r.ok = false;
    r.error = why;
    return r;
  };

  if (!data || !b.has(0, 64)) return fail("too short to be an executable");
  if (data[0] != 'M' || data[1] != 'Z') return fail("not an executable (no MZ header)");

  uint64_t pe = b.u32(0x3c);
  if (!b.has(pe, 24)) return fail("the PE header lies outside the file");
  if (std::memcmp(data + pe, "PE\0\0", 4) != 0) {
    return fail("a DOS program, not a Windows one (no PE header)");
  }
  uint32_t nsec = b.u16(pe + 6);
  uint32_t opt_size = b.u16(pe + 20);
  uint64_t opt = pe + 24;
  if (nsec > kMaxSections) return fail("more sections than any real executable has");
  if (opt_size < 2 || !b.has(opt, opt_size)) return fail("the optional header is truncated");

  uint16_t magic = b.u16(opt);
  uint64_t count_at = 0, dirs_at = 0;
  if (magic == 0x10b) {
    count_at = 92;
    dirs_at = 96;
  } else if (magic == 0x20b) {
    r.is64 = true;
    count_at = 108;
    dirs_at = 112;
  } else {
    return fail("unknown optional header kind");
  }
  // SizeOfHeaders sits at the same place in both layouts.
  uint32_t headers_size = opt_size >= 64 ? b.u32(opt + 60) : 0;

  std::vector<Section> secs;
  uint64_t sec_at = opt + opt_size;
  for (uint32_t i = 0; i < nsec; ++i) {
    uint64_t s = sec_at + uint64_t(i) * 40;
    if (!b.has(s, 40)) return fail("the section table is truncated");
    secs.push_back({b.u32(s + 12), b.u32(s + 8), b.u32(s + 16), b.u32(s + 20)});
  }

  // Everything after this point is about imports. An executable that has
  // headers but no import directory is still an executable; it simply imports
  // nothing, which is not an error.
  r.ok = true;
  r.direct3d_im = mentions_d3d_iid(data, size);

  if (opt_size < count_at + 4) return r;
  uint32_t ndirs = b.u32(opt + count_at);
  // Directory 1 is imports. Both the count and the optional header's own size
  // have to agree that it is there.
  if (ndirs < 2 || opt_size < dirs_at + 16) return r;
  uint32_t imp_rva = b.u32(opt + dirs_at + 8);
  uint32_t imp_size = b.u32(opt + dirs_at + 12);
  if (imp_rva == 0 || imp_size == 0) return r;

  uint64_t desc = 0;
  if (!rva_to_off(secs, headers_size, b, imp_rva, &desc)) {
    return fail("the import table points outside the file");
  }

  for (uint32_t i = 0;; ++i) {
    if (i >= kMaxImports) return fail("the import table does not end");
    uint64_t d = desc + uint64_t(i) * 20;
    if (!b.has(d, 20)) return fail("the import table is truncated");
    // The table ends with an all-zero descriptor.
    bool zero = true;
    for (int k = 0; k < 20; ++k) zero = zero && data[d + k] == 0;
    if (zero) break;

    uint32_t name_rva = b.u32(d + 12);
    uint64_t name_off = 0;
    if (!rva_to_off(secs, headers_size, b, name_rva, &name_off)) {
      return fail("an imported DLL's name points outside the file");
    }
    std::string name;
    bool ended = false;
    for (uint64_t k = name_off; k < b.n && name.size() <= kMaxName; ++k) {
      uint8_t c = data[k];
      if (c == 0) {
        ended = true;
        break;
      }
      name.push_back(char(c));
    }
    if (!ended || name.empty() || name.size() > kMaxName) {
      return fail("an imported DLL's name is not terminated");
    }
    // A DLL name is printable ASCII. Anything else is not a name, and passing
    // it on would put a stranger's bytes into a log or a decision.
    for (unsigned char c : name) {
      if (c < 0x20 || c > 0x7e) return fail("an imported DLL's name is not text");
    }
    std::string low = to_lower(name);
    if (std::find(r.dlls.begin(), r.dlls.end(), low) == r.dlls.end()) r.dlls.push_back(low);
  }
  return r;
}

Imports parse(const std::vector<uint8_t>& bytes) { return parse(bytes.data(), bytes.size()); }

Imports parse_file(const fs::path& p) {
  Imports r;
  std::error_code ec;
  uint64_t size = fs::file_size(p, ec);
  if (ec) {
    r.error = "cannot read " + p.filename().string();
    return r;
  }
  if (size > kMaxFile) {
    r.error = p.filename().string() + " is too large to be a game's executable";
    return r;
  }
  std::vector<uint8_t> buf(size);
  std::ifstream f(p, std::ios::binary);
  if (!f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(size))) {
    r.error = "cannot read " + p.filename().string();
    return r;
  }
  return parse(buf);
}

}  // namespace kg::pe
