// Tier 1 unit tests for the bundle format: trailer v4, the table of contents,
// bundle.meta, and building and verifying a player. No GPU, no container, no
// game: the payloads are a few kilobytes of letters and the packs are made the
// way test_pack.cpp makes them.
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bundle/build.h"
#include "bundle/meta.h"
#include "bundle/toc.h"
#include "pack/kgpack.h"
#include "pack/tree.h"
#include "util/cbor.h"
#include "util/hash.h"

namespace fs = std::filesystem;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++checks;                                                             \
    if (!(cond)) {                                                        \
      ++failures;                                                         \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    ++checks;                                                                \
    auto va = (a);                                                           \
    auto vb = (b);                                                           \
    if (!(va == vb)) {                                                       \
      ++failures;                                                            \
      std::fprintf(stderr, "  FAIL %s:%d  %s != %s\n", __FILE__, __LINE__, #a, #b); \
    }                                                                        \
  } while (0)

// The failure has to be the named one: a table with overlapping entries that
// is refused for its hash instead would be a test passing by accident.
#define CHECK_FAILS(expr, why)                                                        \
  do {                                                                                \
    ++checks;                                                                         \
    bool right = false;                                                               \
    std::string got = "no exception";                                                 \
    try {                                                                             \
      expr;                                                                           \
    } catch (const FormatError& e) {                                                  \
      right = e.failure() == (why);                                                   \
      got = e.what();                                                                 \
    } catch (const std::exception& e) {                                               \
      got = std::string("not a FormatError: ") + e.what();                            \
    }                                                                                 \
    if (!right) {                                                                     \
      ++failures;                                                                     \
      std::fprintf(stderr, "  FAIL %s:%d  %s -> %s\n", __FILE__, __LINE__, #expr, got.c_str()); \
    }                                                                                 \
  } while (0)

// And a message a person can act on has to say what it is about.
#define CHECK_THROWS_WITH(expr, needle)                                               \
  do {                                                                                \
    ++checks;                                                                         \
    std::string got = "no exception";                                                 \
    try {                                                                             \
      expr;                                                                           \
    } catch (const std::exception& e) {                                               \
      got = e.what();                                                                 \
    }                                                                                 \
    if (got.find(needle) == std::string::npos) {                                      \
      ++failures;                                                                     \
      std::fprintf(stderr, "  FAIL %s:%d  %s -> \"%s\", wanted \"%s\"\n", __FILE__, __LINE__, #expr, \
                   got.c_str(), needle);                                              \
    }                                                                                 \
  } while (0)

// Everything below lives in kg::bundle so that Kind is the bundle's; a pack's
// own kind is spelled kg::Kind where one is written.
namespace kg::bundle {

static void section(const char* name) { std::fprintf(stderr, "%s\n", name); }

static void write_file(const fs::path& p, std::string_view content) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

static std::string read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void flip(const fs::path& p, uint64_t off) {
  std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
  f.seekg(static_cast<std::streamoff>(off));
  char c = 0;
  f.read(&c, 1);
  c = static_cast<char>(c ^ 0x5a);
  f.seekp(static_cast<std::streamoff>(off));
  f.write(&c, 1);
}

static std::string pad(std::string s) {
  if (s.size() % kAlign) s.append(kAlign - s.size() % kAlign, '\0');
  return s;
}

// A file with exactly the table given, and a trailer that hashes it honestly:
// what is being tested is then the table's content, never its hash.
static std::string craft(const std::string& body, const std::vector<Entry>& es) {
  std::string toc = encode_toc(es);
  return body + toc + encode_trailer(body.size(), toc.size(), hash_string(toc));
}

static Entry entry(Kind k, uint64_t off, uint64_t len, std::string name = "") {
  Entry e;
  e.kind = k;
  e.off = off;
  e.len = len;
  e.name = std::move(name);
  return e;
}

// Three pages of distinct letters after a short bootstrap, and a table for
// them, so a correct reader has something non-trivial to agree with.
static std::string body_of_three() {
  std::string b = pad(std::string(300, 'B'));
  b += std::string(1000, 'T');
  b = pad(b);
  b += std::string(5000, 'R');
  b = pad(b);
  b += std::string(700, 'A');
  return b;
}

static std::vector<Entry> three() {
  return {entry(Kind::Tools, 4096, 1000), entry(Kind::Runtime, 8192, 5000), entry(Kind::App, 16384, 700)};
}

static Meta pack_meta(const fs::path& tmp, const std::string& id) {
  fs::path root = tmp / ("tree-" + id);
  write_file(root / "GAME.EXE", "MZ this is " + id);
  write_file(root / "data" / "level1.dat", std::string(3000, 'L') + id);
  Meta m;
  m.id = id;
  m.name = "The game " + id;
  m.year = 1999;
  m.run.exe = "GAME.EXE";
  m.tree = Tree::from_directory(root);
  return m;
}

// A capsule with a body, as install writes one. The body is not a DwarFS image;
// nothing here mounts it, and its hash is all that is checked.
static fs::path make_pack(const fs::path& tmp, const std::string& id, size_t body_size = 20000) {
  fs::path body = tmp / (id + ".body");
  std::string b;
  for (size_t i = 0; i < body_size; ++i) b.push_back(static_cast<char>('a' + (i * 7 + id.size()) % 26));
  write_file(body, b);
  fs::path out = tmp / "shelf" / (id + ".kgpack");
  fs::create_directories(out.parent_path());
  write_pack(out, pack_meta(tmp, id), WriteOptions{kg::Kind::Game, body, false});
  return out;
}

static fs::path make_recipe(const fs::path& tmp, const std::string& id) {
  fs::path out = tmp / "shelf" / (id + ".recipe.kgpack");
  fs::create_directories(out.parent_path());
  write_pack(out, pack_meta(tmp, id), WriteOptions{kg::Kind::Game, std::nullopt, false});
  return out;
}

// A player base as the Makefile links one, but out of small files.
static fs::path make_base(const fs::path& tmp) {
  write_file(tmp / "in" / "bootstrap", std::string("\x7f" "ELF") + std::string(2000, 'b'));
  write_file(tmp / "in" / "tools", std::string(6000, 't'));
  write_file(tmp / "in" / "runtime", std::string(9000, 'r'));
  write_file(tmp / "in" / "app", std::string(3000, 'a'));
  fs::path base = tmp / "player-base";
  link_file(base, tmp / "in" / "bootstrap",
            {{Kind::Tools, tmp / "in" / "tools", ""},
             {Kind::Runtime, tmp / "in" / "runtime", ""},
             {Kind::App, tmp / "in" / "app", ""}});
  return base;
}

static BundleMeta sample_meta(std::vector<std::string> ids) {
  BundleMeta m;
  m.id = "retro-shelf-classics";
  m.title = "Retro Shelf Classics";
  m.version = "1.0";
  m.built_at = "2026-09-23T12:00:00Z";
  m.kretro_version = "0.9";
  m.banner = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(40, 'x');
  m.icon = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(10, 'i');
  m.rights_acknowledged = true;
  m.licenses = {"Wine: LGPL-2.1", "DXVK: zlib"};
  for (const std::string& id : ids) {
    GameMeta g;
    g.id = id;
    g.name = "Game " + id;
    g.year = 1998;
    m.games.push_back(g);
  }
  return m;
}

// --- the table of contents ---------------------------------------------------

static void test_toc_round_trip(const fs::path& tmp) {
  section("toc round trip");
  std::vector<Entry> es = three();
  es[0].blake3 = hash_string("tools");
  es[1].flags = 7;
  es.push_back(entry(Kind::Meta, 20480, 10));
  es.push_back(entry(Kind::Pack, 24576, 99, "classic2"));
  std::string body = body_of_three();
  body = pad(body) + std::string(10, 'M');
  body = pad(body) + std::string(99, 'P');
  fs::path p = tmp / "rt.bin";
  write_file(p, craft(body, es));

  Toc t = read_toc(p);
  CHECK_EQ(t.version, 4u);
  CHECK_EQ(t.size, fs::file_size(p));
  CHECK_EQ(t.toc_off, body.size());
  CHECK_EQ(t.toc_len, kTocHeaderSize + kRecordSize * es.size());
  CHECK_EQ(t.entries.size(), es.size());
  for (size_t i = 0; i < es.size() && i < t.entries.size(); ++i) {
    CHECK_EQ(t.entries[i].kind, es[i].kind);
    CHECK_EQ(t.entries[i].flags, es[i].flags);
    CHECK_EQ(t.entries[i].off, es[i].off);
    CHECK_EQ(t.entries[i].len, es[i].len);
    CHECK_EQ(t.entries[i].blake3, es[i].blake3);
    CHECK_EQ(t.entries[i].name, es[i].name);
    CHECK(t.entries[i].hashed);
  }
  CHECK(t.is_player());
  CHECK(t.pack("classic2") != nullptr);
  CHECK(t.pack("classic") == nullptr);
  CHECK_EQ(t.all(Kind::Pack).size(), 1u);
  CHECK_EQ(t.find(Kind::Runtime)->off, 8192u);

  // The exact bytes of the format, so the C bootstrap and the Python linker
  // have something fixed to agree with.
  std::string tr = encode_trailer(0x1122, 0x80, hash_string("x"));
  CHECK_EQ(tr.size(), kTrailerSize);
  CHECK_EQ(tr.substr(0, 8), std::string("KRETROv4"));
  CHECK_EQ(tr.substr(8, 8), std::string("\x04\0\0\0\0\0\0\0", 8));
  CHECK_EQ(tr.substr(16, 8), std::string("\x22\x11\0\0\0\0\0\0", 8));
  std::string toc = encode_toc({entry(Kind::Pack, 4096, 5, "abc")});
  CHECK_EQ(toc.size(), 16u + 128u);
  CHECK_EQ(toc.substr(0, 16), std::string("KTOC\x01\0\0\0\x01\0\0\0\0\0\0\0", 16));
  CHECK_EQ(toc.substr(16, 4), std::string("\x05\0\0\0", 4));
  CHECK_EQ(toc.substr(16 + 56, 4), std::string("abc\0", 4));
  CHECK(encode_toc({}).size() == kTocHeaderSize);
  bool threw = false;
  try {
    encode_toc({entry(Kind::Pack, 4096, 5, std::string(65, 'a'))});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // Read as a range inside a larger file: offsets stay relative to the range.
  fs::path outer = tmp / "outer.bin";
  std::string inner = craft(body, es);
  write_file(outer, std::string(8192, 'O') + inner + std::string(100, 'Z'));
  Toc r = read_toc(outer, 8192, inner.size());
  CHECK_EQ(r.base, 8192u);
  CHECK_EQ(r.entries.size(), es.size());
  CHECK_EQ(r.at(r.entries[1]), 8192u + 8192u);
  CHECK_FAILS(read_toc(outer, 8192, inner.size() + 200), Failure::Truncated);
}

static void test_toc_zero_and_hundred(const fs::path& tmp) {
  section("toc with no entries, and with a hundred");
  fs::path p = tmp / "zero.bin";
  write_file(p, craft(std::string(100, 'B'), {}));
  Toc t = read_toc(p);
  CHECK(t.entries.empty());
  CHECK(!t.is_player());
  CHECK(t.find(Kind::Runtime) == nullptr);

  std::string body = pad(std::string(10, 'B'));
  std::vector<Entry> es;
  for (int i = 0; i < 100; ++i) {
    es.push_back(entry(Kind::Pack, body.size(), 1 + i, "game-" + std::to_string(i)));
    body = pad(body + std::string(1 + i, static_cast<char>('a' + i % 26)));
  }
  p = tmp / "hundred.bin";
  write_file(p, craft(body, es));
  t = read_toc(p);
  CHECK_EQ(t.entries.size(), 100u);
  CHECK_EQ(t.all(Kind::Pack).size(), 100u);
  CHECK(t.pack("game-99") != nullptr);
  CHECK_EQ(t.pack("game-42")->len, 43u);

  // More than any table may hold: refused on the count, before the table's
  // claimed size is allocated.
  std::string toc = encode_toc({});
  toc[8] = '\x01';
  toc[9] = '\x10';  // 4097
  write_file(p, std::string(10, 'B') + toc + encode_trailer(10, toc.size(), hash_string(toc)));
  CHECK_FAILS(read_toc(p), Failure::TooMany);
  // A trailer that says the table is a terabyte.
  write_file(p, std::string(10, 'B') + encode_trailer(10, 1ull << 40, hash_string("")));
  CHECK_FAILS(read_toc(p), Failure::TooMany);
  // A count the table's length does not hold.
  toc = encode_toc({entry(Kind::Tools, 4096, 1)});
  toc[8] = '\x02';
  write_file(p, pad(std::string(10, 'B')) + "x" + toc + encode_trailer(4097, toc.size(), hash_string(toc)));
  CHECK_FAILS(read_toc(p), Failure::BadToc);
  // Wrong table magic and version.
  toc = encode_toc({});
  toc[0] = 'X';
  write_file(p, toc + encode_trailer(0, toc.size(), hash_string(toc)));
  CHECK_FAILS(read_toc(p), Failure::BadToc);
  toc = encode_toc({});
  toc[4] = '\x02';
  write_file(p, toc + encode_trailer(0, toc.size(), hash_string(toc)));
  CHECK_FAILS(read_toc(p), Failure::BadVersion);
  // Wrong trailer version.
  toc = encode_toc({});
  std::string tr = encode_trailer(0, toc.size(), hash_string(toc));
  tr[8] = '\x05';
  write_file(p, toc + tr);
  CHECK_FAILS(read_toc(p), Failure::BadVersion);
}

static void test_toc_bad_entries(const fs::path& tmp) {
  section("toc refuses bad entries");
  fs::path p = tmp / "bad.bin";
  std::string body = body_of_three();
  auto with = [&](std::vector<Entry> es) {
    write_file(p, craft(body, es));
    return p;
  };

  std::vector<Entry> es = three();
  es[2].len = 700 + 1;  // one byte into the table
  CHECK_FAILS(read_toc(with(es)), Failure::OutOfBounds);
  es = three();
  es[2].off = 1ull << 62;
  CHECK_FAILS(read_toc(with(es)), Failure::OutOfBounds);
  // An offset and a length that sum past 2^64 and wrap to inside the file.
  es = three();
  es[0].off = 4096;
  es[0].len = ~0ull - 4095;
  CHECK_FAILS(read_toc(with(es)), Failure::OutOfBounds);

  es = three();
  es[1].off = 4096;  // on top of the tools
  CHECK_FAILS(read_toc(with(es)), Failure::Overlap);
  es = three();
  es[0].len = 4097;  // one byte into the runtime
  CHECK_FAILS(read_toc(with(es)), Failure::Overlap);
  es = three();
  es.push_back(entry(Kind::Pack, 8192, 100, "inside-runtime"));
  CHECK_FAILS(read_toc(with(es)), Failure::Overlap);

  es = three();
  es[1].off = 8193;
  CHECK_FAILS(read_toc(with(es)), Failure::BadEntry);  // not on a page
  es = three();
  es[1].len = 0;
  CHECK_FAILS(read_toc(with(es)), Failure::BadEntry);  // empty
  es = three();
  es[1].kind = Kind::Tools;
  CHECK_FAILS(read_toc(with(es)), Failure::BadEntry);  // two tools
  es = three();
  es[2].kind = static_cast<Kind>(0);
  CHECK_FAILS(read_toc(with(es)), Failure::BadEntry);
  es = three();
  es[2] = entry(Kind::Pack, 16384, 700, "../../evil");
  CHECK_FAILS(read_toc(with(es)), Failure::BadEntry);
  es[2] = entry(Kind::Pack, 16384, 700, "");
  CHECK_FAILS(read_toc(with(es)), Failure::BadEntry);
  es = three();
  es[1] = entry(Kind::Pack, 8192, 5000, "same");
  es[2] = entry(Kind::Pack, 16384, 700, "same");
  CHECK_FAILS(read_toc(with(es)), Failure::BadEntry);  // one game twice

  // A kind this build does not know is carried, not refused; a newer builder
  // may add one an older player need not understand.
  es = three();
  es[2].kind = static_cast<Kind>(99);
  Toc t = read_toc(with(es));
  CHECK_EQ(t.entries.size(), 3u);
  CHECK_EQ(kind_name(static_cast<Kind>(99)), std::string("kind 99"));

  // Bytes after a name's NUL, and a name that is not UTF-8.
  es = three();
  std::string toc = encode_toc(es);
  toc[kTocHeaderSize + 56 + 10] = 'x';
  write_file(p, body + toc + encode_trailer(body.size(), toc.size(), hash_string(toc)));
  CHECK_FAILS(read_toc(p), Failure::BadEntry);
  toc = encode_toc({entry(Kind::Pack, 4096, 1000, "ok")});
  toc[kTocHeaderSize + 56] = '\xff';
  write_file(p, body + toc + encode_trailer(body.size(), toc.size(), hash_string(toc)));
  CHECK_FAILS(read_toc(p), Failure::BadEntry);

  // A table that starts past the end of the file, or runs into its trailer.
  toc = encode_toc(three());
  write_file(p, body + toc + encode_trailer(body.size() + 1, toc.size(), hash_string(toc)));
  CHECK_FAILS(read_toc(p), Failure::OutOfBounds);
  write_file(p, body + toc + encode_trailer(~0ull - 10, toc.size(), hash_string(toc)));
  CHECK_FAILS(read_toc(p), Failure::OutOfBounds);

  // Not a kretro file at all, and one too small to be anything.
  write_file(p, std::string(4000, 'x'));
  CHECK_FAILS(read_toc(p), Failure::BadMagic);
  write_file(p, std::string(100, 'x'));
  CHECK_FAILS(read_toc(p), Failure::BadMagic);
  write_file(p, std::string(10, 'x'));
  CHECK_FAILS(read_toc(p), Failure::Truncated);
  CHECK_FAILS(read_toc(tmp / "no-such-file"), Failure::Unreadable);
}

// The property the bootstrap's first message rests on: however a download was
// cut short, the result is refused, and never read as some other table.
static void test_toc_truncation(const fs::path& tmp) {
  section("toc truncated at every offset");
  fs::path p = tmp / "trunc.bin";
  write_file(p, craft(body_of_three(), three()));
  uint64_t size = fs::file_size(p);
  CHECK_EQ(read_toc(p).entries.size(), 3u);
  int accepted = 0, wrong_kind = 0;
  for (uint64_t k = 0; k < size; ++k) {
    try {
      read_toc(p, 0, k);
      ++accepted;
    } catch (const FormatError&) {
    } catch (const std::exception&) {
      ++wrong_kind;
    }
  }
  CHECK_EQ(accepted, 0);
  CHECK_EQ(wrong_kind, 0);

  // And on real files, not just ranges, at the edges and a sample between.
  fs::path cut = tmp / "cut.bin";
  std::string whole = read_file(p);
  int real_accepted = 0;
  for (uint64_t k = 0; k < size; k += (k < 200 || k > size - 300) ? 1 : 97) {
    write_file(cut, std::string_view(whole).substr(0, k));
    try {
      read_toc(cut);
      ++real_accepted;
    } catch (const FormatError&) {
    }
  }
  CHECK_EQ(real_accepted, 0);
}

static void test_toc_flipped_bytes(const fs::path& tmp) {
  section("toc with a flipped byte");
  fs::path p = tmp / "flip.bin";
  std::string good = craft(body_of_three(), three());
  uint64_t toc_off = body_of_three().size();
  uint64_t toc_end = good.size() - kTrailerSize;

  // Every byte of the table, and every byte of the hash in the trailer: each
  // must be caught by the hash, not survive to be believed.
  int missed = 0;
  for (uint64_t k = toc_off; k < toc_end; ++k) {
    std::string bad = good;
    bad[k] = static_cast<char>(bad[k] ^ 0x01);
    write_file(p, bad);
    try {
      read_toc(p);
      ++missed;
    } catch (const FormatError& e) {
      if (e.failure() != Failure::TocHash) ++missed;
    }
  }
  CHECK_EQ(missed, 0);
  missed = 0;
  for (uint64_t k = good.size() - 32; k < good.size(); ++k) {
    std::string bad = good;
    bad[k] = static_cast<char>(bad[k] ^ 0x80);
    write_file(p, bad);
    try {
      read_toc(p);
      ++missed;
    } catch (const FormatError& e) {
      if (e.failure() != Failure::TocHash) ++missed;
    }
  }
  CHECK_EQ(missed, 0);

  // The rest of the trailer - magic, version, where the table is - may fail
  // several ways, but must fail.
  missed = 0;
  for (uint64_t k = toc_end; k < good.size() - 32; ++k) {
    if (k >= toc_end + 12 && k < toc_end + 16) continue;  // flags: carried, not judged
    std::string bad = good;
    bad[k] = static_cast<char>(bad[k] ^ 0x01);
    write_file(p, bad);
    try {
      read_toc(p);
      ++missed;
    } catch (const FormatError&) {
    }
  }
  CHECK_EQ(missed, 0);

  // A flipped payload byte is not the table's business: it reads, and it is
  // the entry's own hash (see verify, below) that catches it.
  std::string bad = good;
  bad[5000] = static_cast<char>(bad[5000] ^ 1);
  write_file(p, bad);
  CHECK_EQ(read_toc(p).entries.size(), 3u);
}

// --- binaries linked before v4 ------------------------------------------------

static std::string legacy_trailer(int version, uint64_t t0, uint64_t t1, uint64_t r0, uint64_t r1, uint64_t a0,
                                  uint64_t a1, uint64_t g0, uint64_t g1) {
  std::string t(kLegacyTrailerSize, '\0');
  std::memcpy(t.data(), version == 3 ? "KRETROv3" : "KRETROv2", 8);
  t[8] = static_cast<char>(version);
  auto put = [&](size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i) t[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
  };
  put(16, t0); put(24, t1);
  std::memset(t.data() + 32, 'h', 32);  // a SHA-256, once; nothing checks it
  put(64, r0); put(72, r1);
  put(112, a0); put(120, a1);
  if (version == 3) { put(160, g0); put(168, g1); }
  return t;
}

static void test_legacy(const fs::path& tmp) {
  section("v2 and v3 binaries still read");
  fs::path p = tmp / "legacy.bin";
  std::string body = body_of_three();
  write_file(p, body + legacy_trailer(3, 4096, 1000, 8192, 5000, 16384, 700, 0, 0));
  Toc t = read_toc(p);
  CHECK_EQ(t.version, 3u);
  CHECK_EQ(t.entries.size(), 3u);  // an empty game slot is no entry
  CHECK_EQ(t.find(Kind::Runtime)->off, 8192u);
  CHECK_EQ(t.find(Kind::Runtime)->len, 5000u);
  CHECK_EQ(t.find(Kind::App)->len, 700u);
  CHECK(!t.find(Kind::Tools)->hashed);
  CHECK(!t.is_player());

  write_file(p, body + legacy_trailer(2, 4096, 1000, 8192, 5000, 16384, 700, 0, 0));
  t = read_toc(p);
  CHECK_EQ(t.version, 2u);
  CHECK_EQ(t.entries.size(), 3u);

  // A v3 binary carrying a game, the way `export --standalone` appended one: the
  // game slot comes back as a pack, and the pack opens where it sits.
  fs::path pk = make_pack(tmp, "legacy-game");
  std::string with_game = pad(body) + read_file(pk);
  uint64_t goff = pad(body).size(), glen = fs::file_size(pk);
  write_file(p, with_game + legacy_trailer(3, 4096, 1000, 8192, 5000, 16384, 700, goff, glen));
  t = read_toc(p);
  CHECK_EQ(t.entries.size(), 4u);
  const Entry* g = t.find(Kind::Pack);
  CHECK(g != nullptr);
  CHECK(g && g->name.empty());
  CHECK(g && !g->hashed);
  if (g) {
    Pack inside = Pack::open(p, t.at(*g), g->len);
    CHECK_EQ(inside.meta().id, std::string("legacy-game"));
    CHECK(inside.verify().ok);
  }

  // The old trailer's numbers are held to the same rules as the new table's.
  write_file(p, body + legacy_trailer(3, 4096, 1000, 8192, 50000, 16384, 700, 0, 0));
  CHECK_FAILS(read_toc(p), Failure::OutOfBounds);
  write_file(p, body + legacy_trailer(3, 4096, 5000, 8192, 5000, 16384, 700, 0, 0));
  CHECK_FAILS(read_toc(p), Failure::Overlap);
  std::string lie = legacy_trailer(3, 4096, 1000, 8192, 5000, 16384, 700, 0, 0);
  lie[8] = 2;  // a v3 magic claiming version 2
  write_file(p, body + lie);
  CHECK_FAILS(read_toc(p), Failure::BadVersion);
}

// --- packs inside something larger --------------------------------------------

static void test_pack_in_a_range(const fs::path& tmp) {
  section("kgpack opened at an offset");
  fs::path pk = make_pack(tmp, "ranged");
  std::string bytes = read_file(pk);
  fs::path host = tmp / "host.bin";
  write_file(host, std::string(8192, 'H') + bytes + std::string(4096, 'N'));
  Pack p = Pack::open(host, 8192, bytes.size());
  CHECK_EQ(p.base(), 8192u);
  CHECK_EQ(p.meta().id, std::string("ranged"));
  CHECK(p.verify().ok);
  fs::path out = tmp / "ranged.out";
  p.extract_body(out);
  CHECK_EQ(hash_file(out), hash_file(tmp / "ranged.body"));
  CHECK_EQ(Pack::open(pk).base(), 0u);

  // A range shorter than the pack: its body would be its neighbour's bytes.
  CHECK_THROWS_WITH(Pack::open(host, 8192, bytes.size() - 1), "past the end");
  CHECK_THROWS_WITH(Pack::open(host, 8192, 50), "too short");
  CHECK_THROWS_WITH(Pack::open(host, 8192, 1ull << 50), "past the end");
  CHECK_THROWS_WITH(Pack::open(host, ~0ull - 5, 100), "past the end");
  CHECK_THROWS_WITH(Pack::open(host, 0, 4096), "bad magic");
}

// --- bundle.meta ---------------------------------------------------------------

static void test_meta(const fs::path&) {
  section("bundle.meta");
  BundleMeta m = sample_meta({"classic2", "example-game"});
  m.games[0].cover = std::string("\x89PNG", 4) + std::string(100, 'c');
  m.games[0].backend = "cnc-ddraw";
  m.games[0].display = "fit";
  m.games[0].fullscreen = true;
  m.games[0].gamepad = "a=enter,b=escape";
  m.games[0].extra_dlls = {{"ddraw.dll", std::string("MZ\0\1\2", 5)}, {"dgVoodoo.conf", "x"}};
  m.games[1].needs_gpu = true;
  m.games[1].backend = "dxvk";
  m.games[1].key = GameMeta::Key{"1234-5678", "HKLM\\Software\\EXAMPLE", "Key"};
  BundleMeta back = BundleMeta::decode(m.encode());
  CHECK(back == m);
  CHECK_EQ(back.games[0].extra_dlls[0].data, std::string("MZ\0\1\2", 5));
  CHECK(back.games[1].key.has_value());
  CHECK(!back.games[0].key.has_value());
  CHECK_EQ(BundleMeta::decode(back.encode()).encode(), m.encode());
  // A key with no view is read back with none - a meta from before the field
  // - and a view is kept; one that is neither 32 nor 64 is refused.
  CHECK(back.games[1].key->view.empty());
  CHECK(cbor::decode(m.encode()).find("games")->arr[1].find("key")->find("view") == nullptr);
  m.games[1].key->view = "32";
  CHECK_EQ(BundleMeta::decode(m.encode()).games[1].key->view, std::string("32"));
  m.games[1].key->view = "16";
  CHECK_THROWS_WITH(BundleMeta::decode(m.encode()), "neither 32 nor 64");
  m.games[1].key->view = "";

  // The smallest a meta can be; everything optional is absent, not empty.
  BundleMeta small;
  small.id = "one";
  small.title = "One";
  GameMeta g;
  g.id = "g";
  g.name = "G";
  small.games.push_back(g);
  BundleMeta sb = BundleMeta::decode(small.encode());
  CHECK(sb == small);
  CHECK(sb.banner.empty());
  CHECK_EQ(sb.games[0].backend, std::string("auto"));
  CHECK_EQ(sb.games[0].display, std::string("integer"));
  cbor::Value raw = cbor::decode(small.encode());
  CHECK(raw.find("banner") == nullptr);
  CHECK(raw.find("games")->arr[0].find("key") == nullptr);
  CHECK(raw.find("games")->arr[0].find("extra_dlls") != nullptr);

  // Keys from a newer builder are skipped, at the top and inside a game.
  auto doc = [](bool extra, const char* year_type) {
    cbor::Encoder e;
    e.map(extra ? 6 : 5);
    e.text("format"); e.uint_val(1);
    e.text("id"); e.text("b");
    e.text("title"); e.text("B");
    if (extra) { e.text("shiny_new_thing"); e.array(2); e.uint_val(1); e.text("x"); }
    e.text("licenses"); e.array(0);
    e.text("games"); e.array(1);
    e.map(extra ? 4 : 3);
    e.text("id"); e.text("g");
    e.text("name"); e.text("G");
    e.text("year");
    if (std::strcmp(year_type, "text") == 0) e.text("1998");
    else if (std::strcmp(year_type, "neg") == 0) e.int_val(-5);
    else e.uint_val(1998);
    if (extra) { e.text("haptics"); e.map(1); e.text("level"); e.uint_val(3); }
    return e.take();
  };
  BundleMeta u = BundleMeta::decode(doc(true, "uint"));
  CHECK_EQ(u.games.size(), 1u);
  CHECK_EQ(u.games[0].year, 1998u);
  CHECK_EQ(u.id, std::string("b"));

  // A known key of the wrong type is named, not read as zero.
  CHECK_THROWS_WITH(BundleMeta::decode(doc(false, "text")), "'year' of game 1 (g) should be a whole number");
  CHECK_THROWS_WITH(BundleMeta::decode(doc(false, "neg")), "'year'");

  auto one_off = [&](auto mutate) {
    BundleMeta x = sample_meta({"a", "b"});
    mutate(x);
    return x.encode();
  };
  CHECK_THROWS_WITH(BundleMeta::decode(one_off([](BundleMeta& x) { x.format = 2; })), "newer");
  CHECK_THROWS_WITH(BundleMeta::decode(one_off([](BundleMeta& x) { x.id = "../x"; })), "bundle id");
  CHECK_THROWS_WITH(BundleMeta::decode(one_off([](BundleMeta& x) { x.title = ""; })), "no title");
  CHECK_THROWS_WITH(BundleMeta::decode(one_off([](BundleMeta& x) { x.games.clear(); })), "no games");
  CHECK_THROWS_WITH(BundleMeta::decode(one_off([](BundleMeta& x) { x.games[1].id = "a"; })), "two games");
  CHECK_THROWS_WITH(BundleMeta::decode(one_off([](BundleMeta& x) { x.games[1].backend = "glide"; })), "glide");
  CHECK_THROWS_WITH(BundleMeta::decode(one_off([](BundleMeta& x) { x.games[0].display = "stretch"; })), "stretch");
  CHECK_THROWS_WITH(BundleMeta::decode(one_off([](BundleMeta& x) { x.games[0].name = ""; })), "no name");
  CHECK_THROWS_WITH(BundleMeta::decode(one_off([](BundleMeta& x) { x.games[0].id = std::string(65, 'g'); })),
                    "cannot be used");
  CHECK_THROWS_WITH(
      BundleMeta::decode(one_off([](BundleMeta& x) { x.games[0].extra_dlls = {{"../../x.dll", "y"}}; })),
      "cannot be placed");
  CHECK_THROWS_WITH(BundleMeta::decode(one_off([](BundleMeta& x) { x.games[0].key = GameMeta::Key{}; })),
                    "empty key");

  // Shapes that are wrong from the top.
  cbor::Encoder arr;
  arr.array(0);
  CHECK_THROWS_WITH(BundleMeta::decode(arr.data()), "should be a map");
  cbor::Encoder nofmt;
  nofmt.map(1);
  nofmt.text("id"); nofmt.text("x");
  CHECK_THROWS_WITH(BundleMeta::decode(nofmt.data()), "no 'format'");
  cbor::Encoder games_map;
  games_map.map(4);
  games_map.text("format"); games_map.uint_val(1);
  games_map.text("id"); games_map.text("x");
  games_map.text("title"); games_map.text("X");
  games_map.text("games"); games_map.map(0);
  CHECK_THROWS_WITH(BundleMeta::decode(games_map.data()), "'games' of the bundle should be an array");
  CHECK_THROWS_WITH(BundleMeta::decode(std::string_view("\xa1\x61", 2)), "bundle.meta: malformed CBOR");
}

// --- building and verifying a player -----------------------------------------

static void test_gamepad_form() {
  section("the gamepad map's one written form");
  std::map<std::string, std::string> m = {{"a", "Return"}, {"start", "p"}, {"b", "Escape"}};
  CHECK_EQ(format_gamepad(m), std::string("a=Return\nb=Escape\nstart=p\n"));
  CHECK((parse_gamepad(format_gamepad(m)) == m));
  CHECK(format_gamepad({}).empty());
  CHECK(parse_gamepad("").empty());
  // What a person types: blanks, ',' and ';' between entries, a line that is
  // not an entry, and a button named twice.
  auto typed = parse_gamepad("  a = Return , b=Escape;start=p\r\nnot an entry\n=x\ny=\nstart=q\n");
  CHECK_EQ(typed.size(), 3u);
  CHECK_EQ(typed["a"], std::string("Return"));
  CHECK_EQ(typed["start"], std::string("q"));
  CHECK_EQ(parse_gamepad("a=enter,b=escape").size(), 2u);
}

static void test_build_and_verify(const fs::path& tmp) {
  section("build and verify a player");
  fs::path base = make_base(tmp);
  Toc bt = read_toc(base);
  CHECK_EQ(bt.entries.size(), 3u);
  CHECK(!bt.is_player());

  fs::path f2 = make_pack(tmp, "classic2", 30000);
  fs::path example = make_pack(tmp, "example-game", 12345);
  BundleMeta meta = sample_meta({"classic2", "example-game"});
  fs::path out = tmp / "out" / "classics-1.0.run";
  fs::create_directories(out.parent_path());

  std::vector<Progress> seen;
  std::vector<std::string> stages;
  Callbacks cb;
  cb.progress = [&](const Progress& p) {
    seen.push_back(p);
    stages.emplace_back(p.stage);
  };
  Built b = build_bundle(BaseSource{base, 0, std::nullopt, std::nullopt}, meta, {example, f2}, out, cb);
  CHECK(fs::exists(out));
  CHECK(!fs::exists(fs::path(out.string() + ".partial")));
  CHECK_EQ(b.size, fs::file_size(out));
  CHECK((fs::status(out).permissions() & fs::perms::owner_exec) != fs::perms::none);

  // Progress only goes forward, and ends where it said it would.
  bool forward = true;
  for (size_t i = 1; i < seen.size(); ++i) forward = forward && seen[i].done >= seen[i - 1].done;
  CHECK(forward);
  CHECK(!seen.empty() && seen.back().done == seen.back().total);
  CHECK(!stages.empty() && stages.front() == "copying" && stages.back() == "verifying");

  Toc t = read_toc(out);
  CHECK(t.is_player());
  CHECK_EQ(t.entries.size(), 6u);
  for (const Entry& e : t.entries) CHECK_EQ(e.off % kAlign, 0u);
  // The base's payloads are where they were in the base, byte for byte.
  for (Kind k : {Kind::Tools, Kind::Runtime, Kind::App}) {
    CHECK_EQ(t.find(k)->off, bt.find(k)->off);
    CHECK_EQ(t.find(k)->blake3, bt.find(k)->blake3);
  }
  CHECK_EQ(read_file(out).substr(0, 2004), read_file(tmp / "in" / "bootstrap").substr(0, 2004));
  // Packs are in the order given, named by their game ids, and byte for byte.
  std::vector<const Entry*> ps = t.all(Kind::Pack);
  CHECK_EQ(ps.size(), 2u);
  CHECK_EQ(ps[0]->name, std::string("example-game"));
  CHECK_EQ(ps[1]->name, std::string("classic2"));
  CHECK_EQ(ps[1]->blake3, hash_file(f2));
  CHECK_EQ(ps[1]->len, fs::file_size(f2));
  Pack inside = Pack::open(out, t.at(*ps[1]), ps[1]->len);
  CHECK(inside.verify().ok);
  CHECK_EQ(inside.header().blake3_root, Pack::open(f2).header().blake3_root);
  CHECK_EQ((inside.base() + inside.header().body_off) % kAlign, 0u);  // mountable in place

  Verified v = verify_bundle(out);
  CHECK(v.meta == meta);
  CHECK_EQ(v.toc.entries.size(), 6u);

  // Building over an existing player replaces it.
  Built again = build_bundle(BaseSource{base, 0, std::nullopt, std::nullopt}, sample_meta({"classic2"}), {f2}, out);
  CHECK_EQ(read_toc(out).all(Kind::Pack).size(), 1u);
  CHECK_EQ(verify_bundle(out).meta.games.size(), 1u);
  (void)again;
}

// kretro carries the player base as its kind 6 entry; the builder finds it in
// its own file and builds from there, checking it as it copies.
static void test_build_from_kretro(const fs::path& tmp) {
  section("build from the player base inside kretro");
  fs::path base = make_base(tmp);
  write_file(tmp / "in" / "kretro-app", std::string(4000, 'k'));
  fs::path kretro = tmp / "kretro";
  link_file(kretro, tmp / "in" / "bootstrap",
            {{Kind::Tools, tmp / "in" / "tools", ""},
             {Kind::Runtime, tmp / "in" / "runtime", ""},
             {Kind::App, tmp / "in" / "kretro-app", ""},
             {Kind::PlayerBase, base, ""}});
  Toc kt = read_toc(kretro);
  CHECK(!kt.is_player());
  CHECK(kt.find(Kind::PlayerBase) != nullptr);

  BaseSource src = extract_player_base(kretro);
  CHECK_EQ(src.off, kt.at(*kt.find(Kind::PlayerBase)));
  CHECK_EQ(*src.len, fs::file_size(base));
  CHECK(src.blake3.has_value());

  fs::path f2 = make_pack(tmp, "classic2", 30000);
  fs::path out = tmp / "from-kretro.run";
  build_bundle(src, sample_meta({"classic2"}), {f2}, out);
  CHECK(verify_bundle(out).meta.games.size() == 1u);
  // The same player as one built from the base file itself.
  fs::path direct = tmp / "direct.run";
  build_bundle(BaseSource{base, 0, std::nullopt, std::nullopt}, sample_meta({"classic2"}), {f2}, direct);
  CHECK_EQ(hash_file(out), hash_file(direct));

  // kretro's own file cut exactly where its player base ends: the last 64
  // bytes are then the base's trailer, whose table is relative to somewhere
  // else. That must not read as a table.
  const Entry* pbe = kt.find(Kind::PlayerBase);
  CHECK_FAILS(read_toc(kretro, 0, pbe->off + pbe->len), Failure::TocHash);

  // A kretro that was linked without a base says so.
  CHECK_THROWS_WITH(extract_player_base(base), "carries no player base");

  // The base damaged inside kretro: the build refuses, and leaves nothing.
  fs::path hurt = tmp / "kretro-hurt";
  fs::copy_file(kretro, hurt, fs::copy_options::overwrite_existing);
  flip(hurt, src.off + 100);  // inside the base's bootstrap, which no inner entry covers
  fs::path out2 = tmp / "hurt.run";
  CHECK_THROWS_WITH(build_bundle(extract_player_base(hurt), sample_meta({"classic2"}), {f2}, out2),
                    "player base inside");
  CHECK(!fs::exists(out2));
  CHECK(!fs::exists(fs::path(out2.string() + ".partial")));

  // Every truncation of the player just built is refused too.
  uint64_t size = fs::file_size(out);
  int accepted = 0;
  for (uint64_t k = 0; k < size; ++k) {
    try {
      read_toc(out, 0, k);
      ++accepted;
    } catch (const FormatError&) {
    }
  }
  CHECK_EQ(accepted, 0);
}

static void test_build_cancel(const fs::path& tmp) {
  section("a cancelled build leaves nothing");
  fs::path base = make_base(tmp);
  fs::path f2 = make_pack(tmp, "classic2", 3 << 20);  // several chunks
  fs::path out = tmp / "cancel.run";
  fs::path partial = out.string() + ".partial";

  for (int after : {0, 1, 2, 5}) {
    int asked = 0;
    Callbacks cb;
    cb.cancelled = [&] { return ++asked > after; };
    bool cancelled = false;
    try {
      build_bundle(BaseSource{base, 0, std::nullopt, std::nullopt}, sample_meta({"classic2"}), {f2}, out, cb);
    } catch (const Cancelled&) {
      cancelled = true;
    }
    CHECK(cancelled);
    CHECK(!fs::exists(out));
    CHECK(!fs::exists(partial));
  }

  // Cancelled during the verify pass, after every byte was already written.
  std::string stage;
  Callbacks late;
  late.progress = [&](const Progress& p) { stage = p.stage; };
  late.cancelled = [&] { return stage == "verifying"; };
  bool cancelled = false;
  try {
    build_bundle(BaseSource{base, 0, std::nullopt, std::nullopt}, sample_meta({"classic2"}), {f2}, out, late);
  } catch (const Cancelled&) {
    cancelled = true;
  }
  CHECK(cancelled);
  CHECK(!fs::exists(out));
  CHECK(!fs::exists(partial));

  // A stale .partial from a build that was killed is not mistaken for anything.
  write_file(partial, "left over");
  build_bundle(BaseSource{base, 0, std::nullopt, std::nullopt}, sample_meta({"classic2"}), {f2}, out);
  CHECK(fs::exists(out));
  CHECK(!fs::exists(partial));
}

static void test_build_refuses(const fs::path& tmp) {
  section("build refuses what it cannot build");
  fs::path base = make_base(tmp);
  BaseSource src{base, 0, std::nullopt, std::nullopt};
  fs::path f2 = make_pack(tmp, "classic2");
  fs::path example = make_pack(tmp, "example-game");
  fs::path out = tmp / "refused.run";
  auto nothing_left = [&] { return !fs::exists(out) && !fs::exists(fs::path(out.string() + ".partial")); };

  CHECK_THROWS_WITH(build_bundle(src, sample_meta({"classic2", "example-game"}), {f2}, out),
                    "no pack was given for game example-game");
  CHECK_THROWS_WITH(build_bundle(src, sample_meta({"classic2"}), {f2, example}, out), "does not list");
  CHECK_THROWS_WITH(build_bundle(src, sample_meta({"classic2"}), {f2, f2}, out), "given twice");
  CHECK_THROWS_WITH(build_bundle(src, sample_meta({"recipe-only"}), {make_recipe(tmp, "recipe-only")}, out),
                    "recipe");
  BundleMeta bad = sample_meta({"classic2"});
  bad.games[0].backend = "glide";
  CHECK_THROWS_WITH(build_bundle(src, bad, {f2}, out), "glide");
  CHECK(nothing_left());

  // A bundle.meta bigger than verify will read is refused before a byte is
  // copied, not after every game has been.
  BundleMeta huge = sample_meta({"classic2"});
  huge.games[0].extra_dlls.push_back({"dgvoodoo.dll", std::string((64u << 20) + 1, 'd')});
  bool copied = false;
  Callbacks watch;
  watch.progress = [&](const Progress&) { copied = true; };
  CHECK_THROWS_WITH(build_bundle(src, huge, {f2}, out, watch), "bundle.meta would be");
  CHECK(!copied);
  CHECK(nothing_left());

  // Written over one of its own inputs, the rename would replace the author's
  // only copy of it with the player.
  Hash f2_before = hash_file(f2), base_before = hash_file(base);
  CHECK_THROWS_WITH(build_bundle(src, sample_meta({"classic2"}), {f2}, f2), "which it is built from");
  CHECK_THROWS_WITH(build_bundle(src, sample_meta({"classic2"}), {f2}, base), "which it is built from");
  CHECK(hash_file(f2) == f2_before);
  CHECK(hash_file(base) == base_before);
  CHECK(!fs::exists(fs::path(f2.string() + ".partial")));

  // A base that is not a player base: a legacy binary, and a finished player.
  fs::path legacy = tmp / "legacy-base";
  write_file(legacy, body_of_three() + legacy_trailer(3, 4096, 1000, 8192, 5000, 16384, 700, 0, 0));
  CHECK_THROWS_WITH(build_bundle(BaseSource{legacy, 0, std::nullopt, std::nullopt}, sample_meta({"classic2"}),
                                 {f2}, out),
                    "v3 trailer");
  fs::path player = tmp / "a-player.run";
  build_bundle(src, sample_meta({"classic2"}), {f2}, player);
  CHECK_THROWS_WITH(build_bundle(BaseSource{player, 0, std::nullopt, std::nullopt}, sample_meta({"classic2"}),
                                 {f2}, out),
                    "which a player base does not");
  fs::path no_app = tmp / "no-app-base";
  link_file(no_app, tmp / "in" / "bootstrap",
            {{Kind::Tools, tmp / "in" / "tools", ""}, {Kind::Runtime, tmp / "in" / "runtime", ""}});
  CHECK_THROWS_WITH(build_bundle(BaseSource{no_app, 0, std::nullopt, std::nullopt}, sample_meta({"classic2"}),
                                 {f2}, out),
                    "carries no app");
  CHECK(nothing_left());

  // A pack whose body rotted on the shelf is copied, found out by the verify
  // pass, and the half-made player goes with it.
  fs::path rotten = make_pack(tmp, "rotten");
  flip(rotten, Pack::open(rotten).header().body_off + 10);
  CHECK_THROWS_WITH(build_bundle(src, sample_meta({"rotten"}), {rotten}, out), "game rotten is damaged");
  CHECK(nothing_left());
}

static void test_verify_catches_damage(const fs::path& tmp) {
  section("verify finds damage in a built player");
  fs::path base = make_base(tmp);
  fs::path f2 = make_pack(tmp, "classic2", 30000);
  fs::path example = make_pack(tmp, "example-game", 12345);
  fs::path good = tmp / "good.run";
  build_bundle(BaseSource{base, 0, std::nullopt, std::nullopt}, sample_meta({"classic2", "example-game"}),
               {f2, example}, good);
  Toc t = read_toc(good);
  fs::path p = tmp / "damaged.run";
  auto damaged_at = [&](uint64_t off) {
    fs::copy_file(good, p, fs::copy_options::overwrite_existing);
    flip(p, off);
    return p;
  };

  const Entry* example_e = t.pack("example-game");
  Pack example_pack = Pack::open(good, t.at(*example_e), example_e->len);
  // In a game's body: that game, by name.
  CHECK_THROWS_WITH(verify_bundle(damaged_at(t.at(*example_e) + example_pack.header().body_off + 77)),
                    "game example-game is damaged");
  // In a game's metadata or header: still that game.
  CHECK_THROWS_WITH(verify_bundle(damaged_at(t.at(*example_e) + 100)), "game example-game is damaged");
  CHECK_THROWS_WITH(verify_bundle(damaged_at(t.at(*example_e) + 70)), "game example-game is damaged");
  // In the last byte of a game, which is the last byte of its body.
  CHECK_THROWS_WITH(verify_bundle(damaged_at(t.at(*t.pack("classic2")) + t.pack("classic2")->len - 1)),
                    "game classic2 is damaged");
  // In the runtime, the tools, the app, bundle.meta.
  CHECK_THROWS_WITH(verify_bundle(damaged_at(t.find(Kind::Runtime)->off + 4000)), "the runtime is damaged");
  CHECK_THROWS_WITH(verify_bundle(damaged_at(t.find(Kind::Tools)->off)), "the dwarfs tool is damaged");
  CHECK_THROWS_WITH(verify_bundle(damaged_at(t.find(Kind::App)->off + 2999)), "the app is damaged");
  CHECK_THROWS_WITH(verify_bundle(damaged_at(t.find(Kind::Meta)->off + 5)), "bundle.meta is damaged");
  // In the table.
  CHECK_THROWS_WITH(verify_bundle(damaged_at(t.toc_off + 40)), "does not match its hash");

  // Files that are not players at all.
  CHECK_THROWS_WITH(verify_bundle(base), "not a player");
  CHECK_THROWS_WITH(verify_bundle(f2), "no kretro trailer");

  // A table that names one game but carries another: craft it by relabelling.
  {
    std::string bytes = read_file(good);
    std::vector<Entry> es = t.entries;
    for (Entry& e : es) {
      if (e.name == "classic2") e.name = "classic1";
    }
    std::string body = bytes.substr(0, t.toc_off);
    write_file(p, craft(body, es));
    CHECK_THROWS_WITH(verify_bundle(p), "the pack there is game classic2");
  }
  // bundle.meta and the table disagreeing about which games there are.
  {
    std::string bytes = read_file(good);
    std::string body = bytes.substr(0, t.toc_off);
    std::vector<Entry> es;
    for (const Entry& e : t.entries) {
      if (e.name != "example-game") es.push_back(e);
    }
    write_file(p, craft(body, es));
    CHECK_THROWS_WITH(verify_bundle(p), "bundle.meta lists game example-game");
  }
  // And the good one still verifies after all that.
  CHECK(verify_bundle(good).meta.games.size() == 2u);
}

// The Python linker and link_file are two writers of one format. Given the
// same inputs they must write the same bytes, or one of them is wrong.
static void test_python_linker_agrees(const fs::path& tmp) {
  section("scripts/kretro-link.py writes what link_file writes");
  const char* b3_env = std::getenv("KRETRO_B3");
  fs::path b3 = b3_env ? b3_env : "build/kretro-b3";
  fs::path script = "scripts/kretro-link.py";
  if (!fs::exists(b3) || !fs::exists(script) || std::system("python3 -c '' 2>/dev/null") != 0) {
    std::fprintf(stderr, "  skipped: needs python3, %s and %s\n", script.c_str(), b3.c_str());
    return;
  }
  fs::path base = make_base(tmp);
  fs::path in = tmp / "in";
  fs::path py_base = tmp / "py-player-base";
  std::string cmd = "python3 " + script.string() + " --bootstrap " + (in / "bootstrap").string() + " --tools " +
                    (in / "tools").string() + " --image " + (in / "runtime").string() + " --app " +
                    (in / "app").string() + " --b3 " + b3.string() + " --out " + py_base.string() + " >/dev/null";
  CHECK_EQ(std::system(cmd.c_str()), 0);
  CHECK(fs::exists(py_base) && hash_file(py_base) == hash_file(base));
  CHECK(!fs::exists(fs::path(py_base.string() + ".partial")));

  write_file(in / "kretro-app", std::string(4000, 'k'));
  fs::path cpp_kretro = tmp / "cpp-kretro";
  link_file(cpp_kretro, in / "bootstrap",
            {{Kind::Tools, in / "tools", ""},
             {Kind::Runtime, in / "runtime", ""},
             {Kind::App, in / "kretro-app", ""},
             {Kind::PlayerBase, base, ""}});
  fs::path py_kretro = tmp / "py-kretro";
  cmd = "python3 " + script.string() + " --bootstrap " + (in / "bootstrap").string() + " --tools " +
        (in / "tools").string() + " --image " + (in / "runtime").string() + " --app " +
        (in / "kretro-app").string() + " --player-base " + py_base.string() + " --b3 " + b3.string() +
        " --out " + py_kretro.string() + " >/dev/null";
  CHECK_EQ(std::system(cmd.c_str()), 0);
  CHECK(fs::exists(py_kretro) && hash_file(py_kretro) == hash_file(cpp_kretro));
  CHECK(extract_player_base(py_kretro).len == fs::file_size(base));

  // And what the Python linker wrote is a base a player can be built from.
  fs::path out = tmp / "from-python.run";
  build_bundle(extract_player_base(py_kretro), sample_meta({"classic2"}), {make_pack(tmp, "classic2")}, out);
  CHECK(verify_bundle(out).meta.id == "retro-shelf-classics");

  // A --player-base that is not a linked base is refused.
  cmd = "python3 " + script.string() + " --bootstrap " + (in / "bootstrap").string() + " --tools " +
        (in / "tools").string() + " --image " + (in / "runtime").string() + " --app " +
        (in / "kretro-app").string() + " --player-base " + (in / "app").string() + " --b3 " + b3.string() +
        " --out " + (tmp / "never").string() + " >/dev/null 2>&1";
  CHECK(std::system(cmd.c_str()) != 0);
  CHECK(!fs::exists(tmp / "never"));
  // Nor is one too short to hold a trailer; refused with a message, not a
  // traceback from seeking before its start.
  write_file(in / "tiny", "tiny");
  cmd = "python3 " + script.string() + " --bootstrap " + (in / "bootstrap").string() + " --tools " +
        (in / "tools").string() + " --image " + (in / "runtime").string() + " --app " +
        (in / "kretro-app").string() + " --player-base " + (in / "tiny").string() + " --b3 " + b3.string() +
        " --out " + (tmp / "never").string() + " 2>" + (tmp / "tiny.err").string() + " >/dev/null";
  CHECK(std::system(cmd.c_str()) != 0);
  CHECK(read_file(tmp / "tiny.err").find("is not a linked v4 player base") != std::string::npos);
  CHECK(!fs::exists(tmp / "never"));

  // A link that fails after it started writing leaves no .partial behind: here
  // a kretro-b3 that hashes files but dies hashing the table, the last thing
  // written.
  fs::path flaky = tmp / "flaky-b3";
  write_file(flaky, "#!/bin/sh\n[ \"$1\" = - ] && exit 1\nexec " + fs::absolute(b3).string() + " \"$@\"\n");
  fs::permissions(flaky, fs::perms::owner_all);
  fs::path died = tmp / "died";
  cmd = "python3 " + script.string() + " --bootstrap " + (in / "bootstrap").string() + " --tools " +
        (in / "tools").string() + " --image " + (in / "runtime").string() + " --app " + (in / "app").string() +
        " --b3 " + flaky.string() + " --out " + died.string() + " >/dev/null 2>&1";
  CHECK(std::system(cmd.c_str()) != 0);
  CHECK(!fs::exists(died));
  CHECK(!fs::exists(fs::path(died.string() + ".partial")));

  // --b3 named without a directory is the file in the current one, not a
  // command to look up on PATH.
  fs::copy_file(b3, in / "kretro-b3", fs::copy_options::overwrite_existing);
  cmd = "cd " + in.string() + " && python3 " + fs::absolute(script).string() +
        " --bootstrap bootstrap --tools tools --image runtime --app app --b3 kretro-b3 --out " +
        (tmp / "bare-b3").string() + " >/dev/null 2>&1";
  CHECK_EQ(std::system(cmd.c_str()), 0);
  CHECK(fs::exists(tmp / "bare-b3") && hash_file(tmp / "bare-b3") == hash_file(base));
}

}  // namespace kg::bundle

int main() {
  using namespace kg::bundle;
  fs::path tmp = fs::temp_directory_path() / "kretro-test-bundle";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  try {
    test_toc_round_trip(tmp);
    test_toc_zero_and_hundred(tmp);
    test_toc_bad_entries(tmp);
    test_toc_truncation(tmp);
    test_toc_flipped_bytes(tmp);
    test_legacy(tmp);
    test_pack_in_a_range(tmp);
    test_meta(tmp);
    test_gamepad_form();
    test_build_and_verify(tmp);
    test_build_from_kretro(tmp);
    test_build_cancel(tmp);
    test_build_refuses(tmp);
    test_verify_catches_damage(tmp);
    test_python_linker_agrees(tmp);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  FAIL unexpected exception: %s\n", e.what());
    ++failures;
  }

  fs::remove_all(tmp);
  std::fprintf(stderr, "\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
