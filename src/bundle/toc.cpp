#include "toc.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "../util/bytes.h"
#include "../util/safe_names.h"

namespace kg::bundle {
namespace fs = std::filesystem;

namespace {

[[noreturn]] void fail(Failure f, const std::string& what) { throw FormatError(f, what); }

std::string n(uint64_t v) { return std::to_string(v); }

// "entry 3 (pack example-game)": which one, in words that find it again.
std::string describe(size_t i, const Entry& e) {
  std::string s = "entry " + n(i) + " (" + kind_name(e.kind);
  if (!e.name.empty()) s += " " + e.name;
  return s + ")";
}

bool valid_utf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    uint8_t c = static_cast<uint8_t>(s[i]);
    size_t more = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xe ? 2 : (c >> 3) == 0x1e ? 3 : 9;
    if (more == 9 || c == 0xc0 || c == 0xc1 || c > 0xf4) return false;
    if (more > s.size() - i - 1) return false;
    for (size_t k = 1; k <= more; ++k) {
      if ((static_cast<uint8_t>(s[i + k]) & 0xc0) != 0x80) return false;
    }
    i += more + 1;
  }
  return true;
}

// Reads exactly `len` bytes at `off`, or says why not. pread, not a stream, so
// nothing about a previous read's position can leak into this one.
std::string read_at(int fd, const fs::path& p, uint64_t off, size_t len) {
  std::string out(len, '\0');
  size_t got = 0;
  while (got < len) {
    ssize_t r = ::pread(fd, out.data() + got, len - got, static_cast<off_t>(off + got));
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) fail(Failure::Unreadable, "cannot read " + p.string() + ": " + std::strerror(errno));
    if (r == 0) fail(Failure::Truncated, p.string() + " ended while it was being read");
    got += static_cast<size_t>(r);
  }
  return out;
}

// The checks every table gets, old or new. `limit` is where payloads must end:
// the start of the table in v4, the start of the trailer in v2 and v3.
void check_entries(const std::vector<Entry>& es, uint64_t limit, bool v4) {
  constexpr uint32_t kLastKnownKind = static_cast<uint32_t>(Kind::PlayerBase);
  bool seen[kLastKnownKind + 1] = {};
  std::vector<std::string_view> ids;
  for (size_t i = 0; i < es.size(); ++i) {
    const Entry& e = es[i];
    uint32_t k = static_cast<uint32_t>(e.kind);
    if (k == 0) fail(Failure::BadEntry, describe(i, e) + " has no kind");
    if (e.len == 0) fail(Failure::BadEntry, describe(i, e) + " is empty");
    // A payload somewhere other than on a page cannot be mounted in place, and
    // a writer that put one there is not a writer that followed this format.
    if (v4 && e.off % kAlign != 0) {
      fail(Failure::BadEntry, describe(i, e) + " starts at byte " + n(e.off) + ", which is not on a page");
    }
    // Subtraction, so an offset and a length that sum past 2^64 cannot wrap
    // back to somewhere inside the file.
    if (e.off > limit || e.len > limit - e.off) {
      fail(Failure::OutOfBounds, describe(i, e) + " runs to byte " +
                                     (e.off > UINT64_MAX - e.len ? std::string("past 2^64") : n(e.off + e.len)) +
                                     ", past the " + n(limit) + " bytes where payloads must end");
    }
    // Kinds past what this build knows are skipped by every caller, not
    // refused: a newer builder may carry something an older player need not
    // understand, and bounds and overlap still hold them to the same rules.
    if (k <= kLastKnownKind && e.kind != Kind::Pack) {
      if (seen[k]) fail(Failure::BadEntry, "the table names two " + kind_name(e.kind) + " payloads");
      seen[k] = true;
    }
    if (e.kind == Kind::Pack && v4) {
      // The id becomes a path under the player's state directory; the same
      // rule a pack's own id is held to.
      if (!kg::id_is_safe(e.name)) {
        fail(Failure::BadEntry, describe(i, e) + " is not named with a game id that can be used here");
      }
      if (std::find(ids.begin(), ids.end(), e.name) != ids.end()) {
        fail(Failure::BadEntry, "the table carries game " + e.name + " twice");
      }
      ids.push_back(e.name);
    }
  }

  std::vector<size_t> order(es.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return es[a].off < es[b].off; });
  for (size_t j = 1; j < order.size(); ++j) {
    const Entry& a = es[order[j - 1]];
    const Entry& b = es[order[j]];
    // Bounds above make a.off + a.len exact.
    if (b.off < a.off + a.len) {
      fail(Failure::Overlap, describe(order[j - 1], a) + " and " + describe(order[j], b) +
                                 " claim the same bytes");
    }
  }
}

Toc read_legacy(int fd, const fs::path& p, uint64_t base, uint64_t len) {
  std::string raw = read_at(fd, p, base + len - kLegacyTrailerSize, kLegacyTrailerSize);
  bool v3 = raw.compare(0, 8, "KRETROv3") == 0;
  if (!v3 && raw.compare(0, 8, "KRETROv2") != 0) {
    fail(Failure::BadMagic, p.string() + " is " + n(len) +
                                " bytes and ends in no kretro trailer: it is damaged or incomplete, "
                                "or it is not a kretro file");
  }
  Toc t;
  t.base = base;
  t.size = len;
  t.version = le::get_u32(raw, 8);
  if (t.version != (v3 ? 3u : 2u)) {
    fail(Failure::BadVersion, "trailer version " + n(t.version) + " is not one this build reads");
  }
  auto slot = [&](Kind k, size_t at) {
    Entry e;
    e.kind = k;
    e.off = le::get_u64(raw, at);
    e.len = le::get_u64(raw, at + 8);
    e.hashed = false;
    // An empty slot is how v3 says "no game"; it is not an entry.
    if (e.len != 0) t.entries.push_back(e);
  };
  slot(Kind::Tools, 16);
  slot(Kind::Runtime, 64);
  slot(Kind::App, 112);
  if (v3) slot(Kind::Pack, 160);
  check_entries(t.entries, len - kLegacyTrailerSize, false);
  return t;
}

}  // namespace

std::string kind_name(Kind k) {
  switch (k) {
    case Kind::Tools: return "tools";
    case Kind::Runtime: return "runtime";
    case Kind::App: return "app";
    case Kind::Meta: return "meta";
    case Kind::Pack: return "pack";
    case Kind::PlayerBase: return "player base";
  }
  return "kind " + std::to_string(static_cast<uint32_t>(k));
}

const Entry* Toc::find(Kind k) const {
  for (const Entry& e : entries) {
    if (e.kind == k) return &e;
  }
  return nullptr;
}

const Entry* Toc::pack(std::string_view id) const {
  for (const Entry& e : entries) {
    if (e.kind == Kind::Pack && e.name == id) return &e;
  }
  return nullptr;
}

std::vector<const Entry*> Toc::all(Kind k) const {
  std::vector<const Entry*> out;
  for (const Entry& e : entries) {
    if (e.kind == k) out.push_back(&e);
  }
  return out;
}

uint64_t Toc::payload_end() const {
  uint64_t end = 0;
  for (const Entry& e : entries) end = std::max(end, e.off + e.len);
  return end;
}

std::string encode_toc(const std::vector<Entry>& entries) {
  if (entries.size() > kMaxEntries) throw std::invalid_argument("too many entries for one table");
  std::string s;
  s.reserve(kTocHeaderSize + kRecordSize * entries.size());
  s.append(kTocMagic);
  le::put_u32(s, kTocVersion);
  le::put_u32(s, static_cast<uint32_t>(entries.size()));
  le::put_u32(s, 0);
  for (const Entry& e : entries) {
    if (e.name.size() > kNameSize) {
      throw std::invalid_argument("the name " + e.name + " is longer than the table's " +
                                  std::to_string(kNameSize) + " bytes");
    }
    le::put_u32(s, static_cast<uint32_t>(e.kind));
    le::put_u32(s, e.flags);
    le::put_u64(s, e.off);
    le::put_u64(s, e.len);
    s.append(reinterpret_cast<const char*>(e.blake3.data()), e.blake3.size());
    s.append(e.name);
    s.append(kNameSize - e.name.size(), '\0');
    s.append(8, '\0');
  }
  return s;
}

std::string encode_trailer(uint64_t toc_off, uint64_t toc_len, const Hash& toc_hash) {
  std::string s;
  s.reserve(kTrailerSize);
  s.append(kTrailerMagic);
  le::put_u32(s, kTrailerVersion);
  le::put_u32(s, 0);
  le::put_u64(s, toc_off);
  le::put_u64(s, toc_len);
  s.append(reinterpret_cast<const char*>(toc_hash.data()), toc_hash.size());
  return s;
}

std::vector<Entry> decode_toc(std::string_view raw, uint64_t limit) {
  if (raw.size() < kTocHeaderSize) fail(Failure::BadToc, "the table of contents is shorter than its header");
  if (raw.substr(0, 4) != kTocMagic) fail(Failure::BadToc, "the table of contents does not start with KTOC");
  uint32_t version = le::get_u32(raw, 4);
  if (version != kTocVersion) {
    fail(Failure::BadVersion, "table of contents version " + n(version) + " is not one this build reads");
  }
  uint32_t count = le::get_u32(raw, 8);
  if (count > kMaxEntries) {
    fail(Failure::TooMany, "the table of contents claims " + n(count) + " entries; no bundle has more than " +
                               n(kMaxEntries));
  }
  if (raw.size() != kTocHeaderSize + kRecordSize * uint64_t{count}) {
    fail(Failure::BadToc, "the table of contents claims " + n(count) + " entries but is " + n(raw.size()) +
                              " bytes long");
  }

  std::vector<Entry> es;
  es.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    std::string_view r = raw.substr(kTocHeaderSize + kRecordSize * i, kRecordSize);
    Entry e;
    e.kind = static_cast<Kind>(le::get_u32(r, 0));
    e.flags = le::get_u32(r, 4);
    e.off = le::get_u64(r, 8);
    e.len = le::get_u64(r, 16);
    std::memcpy(e.blake3.data(), r.data() + 24, e.blake3.size());
    std::string_view name = r.substr(56, kNameSize);
    size_t nul = name.find('\0');
    if (nul != std::string_view::npos) {
      // NUL-padded means padded with NULs: a name with bytes after its end is
      // a record that was not written by this format.
      if (name.find_first_not_of('\0', nul) != std::string_view::npos) {
        fail(Failure::BadEntry, "entry " + n(i) + " has bytes after the end of its name");
      }
      name = name.substr(0, nul);
    }
    if (!valid_utf8(name)) fail(Failure::BadEntry, "entry " + n(i) + " has a name that is not UTF-8");
    e.name = std::string(name);
    es.push_back(std::move(e));
  }
  check_entries(es, limit, true);
  return es;
}

Toc read_toc(const fs::path& p) {
  std::error_code ec;
  uint64_t size = fs::file_size(p, ec);
  if (ec) fail(Failure::Unreadable, "cannot open " + p.string() + ": " + ec.message());
  return read_toc(p, 0, size);
}

Toc read_toc(const fs::path& p, uint64_t off, uint64_t len) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) fail(Failure::Unreadable, "cannot open " + p.string() + ": " + std::strerror(errno));
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};

  struct stat st {};
  if (::fstat(fd, &st) != 0) fail(Failure::Unreadable, "cannot stat " + p.string() + ": " + std::strerror(errno));
  uint64_t whole = static_cast<uint64_t>(st.st_size);
  if (off > whole || len > whole - off) {
    fail(Failure::Truncated, p.string() + " is " + n(whole) + " bytes; it should be at least " +
                                 (off > UINT64_MAX - len ? std::string("2^64") : n(off + len)));
  }
  if (len < kTrailerSize) {
    fail(Failure::Truncated, p.string() + " is " + n(len) + " bytes, too short to be a kretro file");
  }

  std::string tr = read_at(fd, p, off + len - kTrailerSize, kTrailerSize);
  if (std::string_view(tr).substr(0, 8) != kTrailerMagic) {
    if (len < kLegacyTrailerSize) {
      fail(Failure::BadMagic, p.string() + " is " + n(len) +
                                  " bytes and ends in no kretro trailer: it is damaged or incomplete, "
                                  "or it is not a kretro file");
    }
    return read_legacy(fd, p, off, len);
  }

  Toc t;
  t.base = off;
  t.size = len;
  t.version = le::get_u32(tr, 8);
  if (t.version != kTrailerVersion) {
    fail(Failure::BadVersion, "trailer version " + n(t.version) + " is not one this build reads");
  }
  t.toc_off = le::get_u64(tr, 16);
  t.toc_len = le::get_u64(tr, 24);
  std::memcpy(t.toc_hash.data(), tr.data() + 32, t.toc_hash.size());

  // The length is checked against the most a table can be before anything is
  // allocated for it: a trailer saying the table is a terabyte is a trailer
  // that is lying, and believing it would be the first thing a damaged file
  // got to decide.
  if (t.toc_len > kTocHeaderSize + kRecordSize * uint64_t{kMaxEntries}) {
    fail(Failure::TooMany, "the trailer says the table of contents is " + n(t.toc_len) +
                               " bytes, more than any table can be");
  }
  uint64_t end = len - kTrailerSize;
  if (t.toc_off > end || t.toc_len > end - t.toc_off) {
    // toc_len is bounded above, so only toc_off can make the sum wrap.
    std::string ends = t.toc_off > UINT64_MAX - t.toc_len ? "past 2^64" : "at byte " + n(t.toc_off + t.toc_len);
    fail(Failure::OutOfBounds, p.string() + " is " + n(len) + " bytes, but its table of contents ends " + ends +
                                   ": it is damaged or incomplete");
  }
  std::string raw = read_at(fd, p, off + t.toc_off, static_cast<size_t>(t.toc_len));
  if (hash_bytes(raw.data(), raw.size()) != t.toc_hash) {
    fail(Failure::TocHash, "the table of contents of " + p.string() +
                               " does not match its hash: the file is damaged");
  }
  t.entries = decode_toc(raw, t.toc_off);
  return t;
}

}  // namespace kg::bundle
