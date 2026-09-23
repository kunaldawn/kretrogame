// Tier 1 unit tests for the Bundles page, with the page taken away: the id a
// title becomes, what is remembered, the check list, the size, what "auto"
// means, what a preview inherits, and the build the page's button runs.
//
// The page itself is ImGui and SDL and cannot be linked here, which is why
// everything it decides lives in src/bundle/builder.cpp. Packs are fixtures
// written with write_pack; the one test that needs a real DwarFS image makes
// it with the dwarfs tool in build/, and says so and skips when there is none.
//
// With arguments - `test_builder real <kretro> <out-dir> <game-id>...` - it is
// instead the build path run for real, against the shelf in KRETRO_STATE and
// the player base inside <kretro>, for a person to time.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "bundle/build.h"
#include "bundle/builder.h"
#include "bundle/preview.h"
#include "install/keys.h"
#include "install/registry.h"
#include "pack/kgpack.h"
#include "player/player.h"
#include "player/prefix.h"
#include "util/cbor.h"
#include "util/hash.h"
#include "util/paths.h"
#include "util/pe.h"
#include "util/proc.h"

namespace fs = std::filesystem;
using namespace kg;
using namespace kg::bundle;
using BKind = kg::bundle::Kind;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++checks;                                                                \
    if (!(cond)) {                                                           \
      ++failures;                                                            \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    ++checks;                                                                            \
    auto va_ = (a);                                                                      \
    auto vb_ = (b);                                                                      \
    if (!(va_ == vb_)) {                                                                 \
      ++failures;                                                                        \
      std::ostringstream os_;                                                            \
      os_ << va_ << " != " << vb_;                                                       \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, os_.str().c_str()); \
    }                                                                                    \
  } while (0)

#define CHECK_THROWS_WITH(expr, needle)                                                     \
  do {                                                                                      \
    ++checks;                                                                               \
    std::string what_;                                                                      \
    bool threw_ = false;                                                                    \
    try { expr; } catch (const std::exception& e_) { threw_ = true; what_ = e_.what(); }    \
    if (!threw_ || what_.find(needle) == std::string::npos) {                               \
      ++failures;                                                                           \
      std::fprintf(stderr, "  FAIL %s:%d  %s: wanted a throw with '%s', got %s'%s'\n",      \
                   __FILE__, __LINE__, #expr, needle, threw_ ? "" : "no throw ",           \
                   what_.c_str());                                                          \
    }                                                                                       \
  } while (0)

static void section(const char* name) { std::fprintf(stderr, "%s\n", name); }

static void write_file(const fs::path& p, std::string_view content) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

static bool has(const std::vector<Check>& cs, const std::string& id) {
  return std::any_of(cs.begin(), cs.end(), [&](const Check& c) { return c.id == id; });
}

static std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

static bool has_text(const std::vector<std::string>& v, const std::string& needle) {
  return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

// ---- PE fixtures, as tests/unit/test_policy.cpp makes them ----------------------------

static void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = uint8_t(v);
  b[at + 1] = uint8_t(v >> 8);
}
static void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i));
}

// The smallest 32-bit executable the parser accepts, importing `dlls`.
static std::vector<uint8_t> make_pe(const std::vector<std::string>& dlls) {
  std::vector<uint8_t> b(0x400, 0);
  const size_t opt = 0x40 + 24, dirs = opt + 96, sections = opt + 224;
  b[0] = 'M';
  b[1] = 'Z';
  put32(b, 0x3c, 0x40);
  std::memcpy(&b[0x40], "PE\0\0", 4);
  put16(b, 0x44, 0x14c);
  put16(b, 0x46, 1);
  put16(b, 0x54, 224);
  put16(b, opt, 0x10b);
  put32(b, opt + 60, 0x200);
  put32(b, opt + 92, 16);
  put32(b, dirs + 8, 0x1000);
  put32(b, dirs + 12, uint32_t(20 * (dlls.size() + 1)));
  std::memcpy(&b[sections], ".idata\0\0", 8);
  put32(b, sections + 8, 0x1000);
  put32(b, sections + 12, 0x1000);
  put32(b, sections + 16, 0x200);
  put32(b, sections + 20, 0x200);
  uint32_t name_rva = 0x1100;
  for (size_t i = 0; i < dlls.size(); ++i) {
    put32(b, 0x200 + i * 20 + 12, name_rva);
    std::memcpy(&b[0x200 + (name_rva - 0x1000)], dlls[i].c_str(), dlls[i].size() + 1);
    name_rva += uint32_t(dlls[i].size() + 1);
  }
  return b;
}

static std::string as_string(const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); }

// ---- pack fixtures --------------------------------------------------------------------

struct PackSpec {
  std::string id;
  std::string fragment;                  // REGEDIT4
  std::vector<std::string> extra_files;  // beside GAME.EXE in the tree
  bool dgvoodoo = false;
  bool with_disc = false;
  size_t body = 20000;
  std::string exe = "GAME.EXE";          // where the executable is in the tree
};

// A capsule as install writes one, except that its body is not a DwarFS image:
// nothing here mounts it, and only its hash is ever checked.
static fs::path make_pack(const fs::path& tmp, const fs::path& shelf, const PackSpec& s) {
  fs::path root = tmp / ("tree-" + s.id);
  fs::remove_all(root);
  write_file(root / s.exe, as_string(make_pe({"KERNEL32.dll", "d3d8.dll"})));
  write_file(root / "data" / "level1.dat", std::string(3000, 'L') + s.id);
  for (const std::string& f : s.extra_files) write_file(root / f, "x");
  Meta m;
  m.id = s.id;
  m.name = "The game " + s.id;
  m.year = 1999;
  m.run.exe = s.exe;
  m.registry.fragment = s.fragment;
  m.runtime.dgvoodoo = s.dgvoodoo;
  m.tree = Tree::from_directory(root);
  if (s.with_disc) {
    m.layout = "rooted";
    Meta::Disc d;
    d.label = "DISC1";
    d.embedded = true;
    m.discs.push_back(d);
    DiscFingerprint fp;
    fp.size = 3 * m.tree.total_bytes();  // the disc weighs three times the game
    m.recipe.fingerprints.push_back(fp);
  }
  m.input["a"] = "Return";

  fs::path body = tmp / (s.id + ".body");
  std::string b;
  for (size_t i = 0; i < s.body; ++i) b.push_back(static_cast<char>('a' + (i * 7 + s.id.size()) % 26));
  write_file(body, b);
  fs::path out = shelf / (s.id + ".kgpack");
  fs::create_directories(shelf);
  write_pack(out, m, WriteOptions{kg::Kind::Game, body, false});
  return out;
}

static const char* kKeyFragment =
    "REGEDIT4\n"
    "\n"
    "[HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\EXAMPLE]\n"
    "\"CDKey\"=\"ABCD-1234-EFGH-5678\"\n"
    "\"InstallPath\"=\"C:\\\\Games\\\\EXAMPLE\"\n";

static const char* kCleanFragment =
    "REGEDIT4\n"
    "\n"
    "[HKEY_CURRENT_USER\\Software\\Example Publisher\\EXAMPLE\\Video]\n"
    "\"Width\"=dword:00000280\n"
    "\n"
    "[HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\EXAMPLE]\n"
    "\"InstallPath\"=\"C:\\\\Games\\\\EXAMPLE\"\n"
    "\"Version\"=\"1.0\"\n"
    "\n"
    "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\EXAMPLE]\n"
    "\"DisplayName\"=\"EXAMPLE\"\n"
    "\"UninstallString\"=\"x\"\n"
    "\"Publisher\"=\"Example Publisher\"\n";

// The dwarfs tool, when there is one to run: KRETRO_DWARFS, or build/'s.
static std::string dwarfs_tool() {
  if (const char* t = std::getenv("KRETRO_DWARFS"); t && *t) return t;
  if (fs::exists("build/dwarfs-universal")) return fs::absolute("build/dwarfs-universal").string();
  return "";
}

// A player base as the Makefile links one, but out of small files. With a
// dwarfs tool its runtime is a real DwarFS image carrying two licence notices
// (and Debian's common-licenses, which is not one), so the licence list can
// be read out of it; without one it is filler.
static fs::path make_base(const fs::path& tmp) {
  write_file(tmp / "in" / "bootstrap", std::string("\x7f" "ELF") + std::string(2000, 'b'));
  write_file(tmp / "in" / "tools", std::string(6000, 't'));
  write_file(tmp / "in" / "runtime", std::string(9000, 'r'));
  if (std::string tool = dwarfs_tool(); !tool.empty()) {
    fs::path rt = tmp / "base-rt";
    fs::remove_all(rt);
    write_file(rt / "usr/share/kretro/licenses/wine/COPYING.LIB", "x");
    write_file(rt / "usr/share/kretro/licenses/dxvk/LICENSE", "x");
    write_file(rt / "usr/share/kretro/licenses/common-licenses/GPL-2", "x");
    write_file(rt / "lib/ld-linux-x86-64.so.2", "not really");
    ProcResult mk = kg::run({tool, "--tool=mkdwarfs", "-i", rt.string(), "-o", (tmp / "in" / "runtime").string(),
                             "--log-level=error", "--force"});
    CHECK(mk.ok());
  }
  write_file(tmp / "in" / "app", std::string(3000, 'a'));
  fs::path base = tmp / "player-base";
  link_file(base, tmp / "in" / "bootstrap",
            {{BKind::Tools, tmp / "in" / "tools", ""},
             {BKind::Runtime, tmp / "in" / "runtime", ""},
             {BKind::App, tmp / "in" / "app", ""}});
  return base;
}

static std::string png(char fill, size_t n) { return std::string("\x89PNG\r\n\x1a\n", 8) + std::string(n, fill); }

// ---- identity -------------------------------------------------------------------------

static void test_slug() {
  section("the bundle id a title becomes");
  CHECK_EQ(id_from_title("Retro Shelf Classics"), std::string("retro-shelf-classics"));
  CHECK_EQ(id_from_title("  Example G.A.M.E.: It's Here  "), std::string("example-g-a-m-e-it-s-here"));
  CHECK_EQ(id_from_title("Classic 1 & 2"), std::string("classic-1-2"));
  // Nothing in it that can be a name: an id is still an id.
  CHECK_EQ(id_from_title(""), std::string("bundle"));
  CHECK_EQ(id_from_title("!!! ???"), std::string("bundle"));
  // Bytes outside ASCII are separators, never half a character in a file name.
  CHECK_EQ(id_from_title("\xc3\x9c" "bergame"), std::string("bergame"));
  // Capped, and never ending on the dash the cap cut before.
  std::string longer;
  for (int i = 0; i < 20; ++i) longer += "abc ";
  std::string id = id_from_title(longer);
  CHECK(id.size() <= 64);
  CHECK(id.back() != '-');
  CHECK(kg::id_is_safe(id));

  CHECK(version_is_safe("1.0"));
  CHECK(version_is_safe("2026.09-rc1"));
  CHECK(!version_is_safe(""));
  CHECK(!version_is_safe("1.0 beta"));
  CHECK(!version_is_safe("../1"));
  CHECK(!version_is_safe(".1"));
  CHECK(!version_is_safe(std::string(40, '1')));

  Draft d;
  d.id = "retro-shelf-classics";
  d.version = "1.2";
  CHECK_EQ(output_name(d), std::string("retro-shelf-classics-1.2.run"));
}

// ---- remembering ------------------------------------------------------------------------

static Draft full_draft() {
  Draft d;
  d.id = "retro-shelf";
  d.id_typed = true;
  d.published = true;
  d.title = "Retro Shelf";
  d.version = "1.1";
  d.banner = png('b', 300);
  d.icon = png('i', 30);
  d.banner_from = "/home/a/banner.png";
  d.icon_from = "/home/a/icon.png";
  DraftGame a;
  a.id = "classic";
  a.name = "Classic";
  a.year = 1997;
  a.cover = png('c', 100);
  a.cover_from = "/home/a/classic.png";
  a.backend = "cnc-ddraw";
  a.display = "fit";
  a.fullscreen = true;
  a.gamepad = "a=Return\nb=Escape\n";
  a.extra_dlls = {{"dgVoodoo.conf", std::string("conf\0bytes", 10)}, {"DDraw.dll", "MZ..."}};
  DraftGame b;
  b.id = "classic2";
  b.name = "Classic 2";
  b.year = 1998;
  b.needs_gpu = true;
  b.embed_key = true;
  b.key_path = "HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\Classic2";
  b.key_value = "CDKey";
  d.games = {a, b};
  d.rights = true;
  d.acknowledged = {"cover:classic2", "key-embedded:classic2"};
  d.out_dir = "/home/a/out";
  d.last_built = "/home/a/out/retro-shelf-1.1.run";
  d.last_size = 123456789012ull;
  d.last_built_at = "2026-09-23T12:00:00Z";
  return d;
}

static void test_remember(const fs::path& tmp) {
  section("a remembered bundle comes back as it was left");
  Draft d = full_draft();
  Draft back = decode_draft(encode_draft(d));
  CHECK(back == d);
  CHECK_EQ(back.games[0].extra_dlls[0].data.size(), size_t(10));  // a NUL inside survives
  CHECK(back.games[1].embed_key);

  // A fresh draft's defaults survive too: nothing is set by being absent.
  Draft empty;
  CHECK(decode_draft(encode_draft(empty)) == empty);

  fs::path dir = tmp / "bundles";
  save_draft(dir, d);
  Draft other = full_draft();
  other.id = "arcane";
  other.title = "Arcane";
  save_draft(dir, other);
  std::vector<Draft> all = load_drafts(dir);
  CHECK_EQ(all.size(), size_t(2));
  CHECK_EQ(all[0].title, std::string("Arcane"));  // by title
  CHECK(all[1] == d);

  // Renamed before publishing: the old file goes, or one bundle would be two.
  Draft renamed = d;
  renamed.id = "retro-shelf-classics";
  save_draft(dir, renamed, d.id);
  CHECK(!fs::exists(dir / "retro-shelf.cbor"));
  CHECK(fs::exists(dir / "retro-shelf-classics.cbor"));
  CHECK(!fs::exists(dir / "retro-shelf-classics.cbor.new"));
  CHECK_EQ(load_drafts(dir).size(), size_t(2));

  // A damaged file is named and the rest still load.
  write_file(dir / "broken.cbor", "not cbor at all");
  std::vector<std::string> bad;
  CHECK_EQ(load_drafts(dir, &bad).size(), size_t(2));
  CHECK_EQ(bad.size(), size_t(1));
  CHECK(has_text(bad, "broken.cbor"));

  // An id that is not a file name is refused, not joined onto the directory.
  Draft evil = d;
  evil.id = "../../escape";
  CHECK_THROWS_WITH(save_draft(dir, evil), "not a name");

  // A second bundle that lands on an id already remembered - a new one left at
  // its default title, or a title that slugs the same - is refused, and the
  // first is still there as it was.
  Draft clash = full_draft();
  clash.id = "arcane";
  clash.title = "Something else";
  CHECK_THROWS_WITH(save_draft(dir, clash), "already remembered as 'arcane'");
  CHECK_THROWS_WITH(save_draft(dir, clash, "retro-shelf-classics"), "already remembered");
  CHECK(fs::exists(dir / "retro-shelf-classics.cbor"));
  CHECK(!fs::exists(dir / "arcane.cbor.new"));
  for (const Draft& x : load_drafts(dir)) {
    if (x.id == "arcane") CHECK_EQ(x.title, std::string("Arcane"));
  }
  // Saving a bundle over itself is what every edit does.
  other.version = "2.0";
  save_draft(dir, other, other.id);
  CHECK_EQ(decode_draft(slurp(dir / "arcane.cbor")).version, std::string("2.0"));
  // What a new bundle starts as never is one of these.
  CHECK_EQ(unused_id(dir, "fresh"), std::string("fresh"));
  CHECK_EQ(unused_id(dir, "arcane"), std::string("arcane-2"));
  write_file(dir / "arcane-2.cbor", "x");
  CHECK_EQ(unused_id(dir, "arcane"), std::string("arcane-3"));
  fs::remove(dir / "arcane-2.cbor");

  // A finished build is recorded on the file as it is now, and not on a
  // bundle that has gone - renamed while it built - which would come back.
  CHECK(stamp_built(dir, "arcane", "/out/arcane-2.0.run", 1234, "2026-09-23 10:00"));
  Draft stamped = decode_draft(slurp(dir / "arcane.cbor"));
  CHECK_EQ(stamped.last_built, std::string("/out/arcane-2.0.run"));
  CHECK_EQ(stamped.last_size, uint64_t(1234));
  CHECK_EQ(stamped.version, std::string("2.0"));
  CHECK(!stamp_built(dir, "retro-shelf", "/out/x.run", 1, "now"));
  CHECK(!fs::exists(dir / "retro-shelf.cbor"));
  CHECK(!stamp_built(dir, "../escape", "/out/x.run", 1, "now"));

  forget_draft(dir, "arcane");
  CHECK(!fs::exists(dir / "arcane.cbor"));

  // A field of the wrong type is named rather than read as a default.
  cbor::Encoder e;
  e.map(2);
  e.text("format"); e.uint_val(kDraftFormat);
  e.text("title"); e.uint_val(7);
  CHECK_THROWS_WITH(decode_draft(e.take()), "'title' should be text");
  cbor::Encoder f;
  f.map(1);
  f.text("format"); f.uint_val(kDraftFormat + 1);
  CHECK_THROWS_WITH(decode_draft(f.take()), "format");
}

// ---- keys and registry.reg ----------------------------------------------------------------

// `kretro bundle build` remembers what it built, as the page's Build does, so
// `bundle list` shows it and `bundle rebuild` makes the next one: the README's
// build, list, rebuild runs as written.
static void test_remember_built(const fs::path& tmp) {
  section("a bundle built from the command line is remembered, and can be rebuilt");
  fs::path dir = tmp / "cli-bundles";
  Draft d = full_draft();
  d.id = "classics";
  d.rights = true;
  d.acknowledged.clear();
  const std::vector<Check> said = {{"cover:classic", "classic", "Classic has no cover art", ""}};
  CHECK(!ready_to_build(d, said, {}));
  CHECK(remember_built(dir, d, said, "/out/classics.run", 1234, "2026-09-23 10:00"));
  std::vector<Draft> all = load_drafts(dir);
  CHECK_EQ(all.size(), size_t(1));
  if (!all.empty()) {
    CHECK_EQ(all[0].id, std::string("classics"));
    CHECK_EQ(all[0].last_built, std::string("/out/classics.run"));
    CHECK_EQ(all[0].last_size, uint64_t(1234));
    // What the build printed and went ahead with, rebuild is held to - and
    // only that: a check that turns up later still stops it.
    CHECK(ready_to_build(all[0], said, {}));
    std::vector<Check> more = said;
    more.push_back({"glide:classic", "classic", "Glide only", ""});
    CHECK(!ready_to_build(all[0], more, {}));
  }
  // One already remembered is the page's, with what the author set on it.
  Draft again = d;
  again.title = "Something else";
  CHECK(!remember_built(dir, again, said, "/out/other.run", 1, "2026-09-23 11:00"));
  all = load_drafts(dir);
  CHECK(!all.empty() && all[0].title == d.title && all[0].last_built == "/out/classics.run");
}

// "Repack for faster loading": an older pack's body packed again the way
// install packs one now, with the Merkle root it had.
static void test_repack(const fs::path& tmp) {
  section("an older pack is repacked for faster loading, and is the same game after");
  const std::string tool = dwarfs_tool();
  if (tool.empty()) {
    std::fprintf(stderr, "  skip: no dwarfs tool (build/dwarfs-universal or KRETRO_DWARFS) to repack with\n");
    return;
  }
  fs::path img = tmp / "repack-img";
  fs::remove_all(img);
  write_file(img / "game" / "GAME.EXE", "MZ the game");
  write_file(img / "game" / "data" / "level1.dat", std::string(200000, 'L'));
  write_file(img / "system" / "windows" / "system32" / "old.dll", "a 1998 DLL");
  write_file(img / "registry.reg", "REGEDIT4\n");
  fs::path body = tmp / "repack.dwarfs";
  CHECK(kg::run({tool, "--tool=mkdwarfs", "-i", img.string(), "-o", body.string(), "--log-level=error", "--force"})
            .ok());
  Meta m;
  m.id = "old";
  m.name = "An old pack";
  m.layout = "rooted";
  m.run.exe = "GAME.EXE";
  m.tree = Tree::from_directory(img / "game");
  fs::path pack = tmp / "old.kgpack";
  write_pack(pack, m, WriteOptions{kg::Kind::Game, body, false});
  const Hash old_body = Pack::open(pack).meta().body.blake3;
  CHECK(packed_before_faster_loading(read_pack_facts(pack)));

  std::vector<std::string> stages;
  Callbacks cb;
  cb.progress = [&](const Progress& p) { stages.emplace_back(p.stage); };
  repack_for_faster_loading(pack, tool, tmp / "repack-scratch", cb);
  Pack after = Pack::open(pack);
  CHECK(after.verify().ok);
  CHECK_EQ(to_hex(after.meta().tree.root()), to_hex(m.tree.root()));
  CHECK_EQ(to_hex(after.header().blake3_root), to_hex(m.tree.root()));
  CHECK(after.meta().body.blake3 != old_body);
  CHECK_EQ(after.meta().body.packing, std::string(kBodyPacking));
  CHECK(!packed_before_faster_loading(read_pack_facts(pack)));
  CHECK_EQ(after.meta().name, m.name);
  CHECK(!fs::exists(tmp / "repack-scratch"));
  CHECK(!fs::exists(pack.string() + ".repack"));
  CHECK(!stages.empty() && stages.back() == "done");
  // What is outside game/ went back in too.
  fs::path out = tmp / "repack-out";
  after.extract_body(tmp / "after.dwarfs");
  fs::create_directories(out);
  CHECK(kg::run({tool, "--tool=dwarfsextract", "-i", (tmp / "after.dwarfs").string(), "-o", out.string()}).ok());
  CHECK(fs::exists(out / "system" / "windows" / "system32" / "old.dll"));
  CHECK(fs::exists(out / "registry.reg"));

  // A pack whose body is not the tree it names is left exactly as it was.
  Meta wrong = m;
  wrong.tree = Tree::from_directory(img / "system");
  fs::path liar = tmp / "liar.kgpack";
  write_pack(liar, wrong, WriteOptions{kg::Kind::Game, body, false});
  const std::string before = [&] {
    std::ifstream f(liar, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }();
  CHECK_THROWS_WITH(repack_for_faster_loading(liar, tool, tmp / "repack-scratch"), "not its own");
  std::ifstream f(liar, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  CHECK(ss.str() == before);
  CHECK(!fs::exists(tmp / "repack-scratch"));
}

static void test_fragment() {
  section("a pack's registry fragment read back, and where a key goes");
  // What install writes, read back value for value.
  std::vector<install::RegValue> in = {
      {"Software\\Example Publisher\\EXAMPLE", "InstallPath", "sz", "C:\\Games\\EXAMPLE \"quoted\"", std::string(install::kHiveLocalMachine)},
      {"Software\\Example Publisher\\EXAMPLE", "@", "sz", "default", std::string(install::kHiveLocalMachine)},
      {"Software\\Example Publisher\\EXAMPLE\\Video", "Width", "dword", "00000280", std::string(install::kHiveCurrentUser)},
  };
  std::vector<install::RegValue> got = read_fragment(install::to_reg_fragment(in));
  CHECK_EQ(got.size(), size_t(3));
  bool found_quoted = false, found_dword = false, found_default = false;
  for (const install::RegValue& v : got) {
    if (v.name == "InstallPath" && v.data == "C:\\Games\\EXAMPLE \"quoted\"" && v.type == "sz" &&
        v.hive == install::kHiveLocalMachine && v.key == "Software\\Example Publisher\\EXAMPLE") {
      found_quoted = true;
    }
    if (v.name == "Width" && v.type == "dword" && v.data == "00000280" && v.hive == install::kHiveCurrentUser) {
      found_dword = true;
    }
    if (v.name == "@" && v.data == "default") found_default = true;
  }
  CHECK(found_quoted);
  CHECK(found_dword);
  CHECK(found_default);

  // A key still in the fragment is found, by its shape or by being the vault's.
  auto k = key_in_fragment(kKeyFragment, "");
  CHECK(k.has_value());
  if (k) CHECK_EQ(k->name, std::string("CDKey"));
  CHECK(!key_in_fragment(kCleanFragment, "").has_value());
  CHECK(!key_in_fragment(kCleanFragment, "ZZZZ-0000").has_value());
  const char* odd =
      "REGEDIT4\n\n[HKEY_LOCAL_MACHINE\\Software\\X]\n\"Owner\"=\"ZZZZ-0000\"\n";
  CHECK(key_in_fragment(odd, "ZZZZ-0000").has_value());

  // Where an embedded key would go: its old place, or the game's own key.
  KeySpot s1 = suggest_key_spot(kKeyFragment, "");
  CHECK_EQ(s1.path, std::string("HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\EXAMPLE"));
  CHECK_EQ(s1.value, std::string("CDKey"));
  KeySpot s2 = suggest_key_spot(kCleanFragment, "");
  CHECK_EQ(s2.path, std::string("HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\EXAMPLE"));  // not Microsoft's, which has more
  CHECK_EQ(s2.value, std::string(""));
  CHECK_EQ(suggest_key_spot("", "").path, std::string(""));
}

// ---- the check list ------------------------------------------------------------------------

static void test_checks(const fs::path& tmp) {
  section("the check list against fixture packs");
  fs::path shelf = tmp / "shelf";
  PackFacts keyed = read_pack_facts(make_pack(tmp, shelf, {"keyed", kKeyFragment, {}, false, false, 20000}));
  PackFacts clean = read_pack_facts(make_pack(tmp, shelf, {"clean", kCleanFragment, {}, false, false, 20000}));
  PackFacts protected_ = read_pack_facts(
      make_pack(tmp, shelf, {"safedisc", kCleanFragment, {"drvmgt.dll", "sub/SECDRV.SYS", "sintf32.dll"}, false, false, 20000}));
  PackFacts dgv = read_pack_facts(make_pack(tmp, shelf, {"dgv", kCleanFragment, {}, true, false, 20000}));
  PackFacts glide = read_pack_facts(make_pack(tmp, shelf, {"glide", kCleanFragment, {"glide2x.dll"}, false, false, 20000}));

  CHECK_EQ(keyed.meta.id, std::string("keyed"));
  CHECK(keyed.bytes > 20000u);

  Draft d;
  d.id = "fixture";
  d.title = "Fixture";
  d.out_dir = tmp.string();
  for (const PackFacts* p : {&keyed, &clean, &protected_, &dgv, &glide}) d.games.push_back(game_from_pack(*p));
  CHECK_EQ(d.games[0].name, std::string("The game keyed"));
  CHECK_EQ(d.games[0].year, uint32_t(1999));
  CHECK_EQ(d.games[0].gamepad, std::string("a=Return\n"));
  // Every game in this draft has a cover but the second.
  for (DraftGame& g : d.games) g.cover = png('c', 10);
  d.games[1].cover.clear();

  std::vector<GameFacts> facts = {
      {&keyed, std::nullopt, ""},
      {&clean, std::nullopt, "ABCD-1234-EFGH-5678"},
      {&protected_, std::nullopt, ""},
      {&dgv, std::nullopt, ""},
      {&glide, pe::parse(make_pe({"KERNEL32.dll", "glide2x.dll"})), ""},
  };
  std::vector<Check> cs = run_checks(d, facts);
  // The key the author's install left in registry.reg.
  CHECK(has(cs, "key-in-pack:keyed"));
  // The key install removed, which the vault still has.
  CHECK(has(cs, "key-removed:clean"));
  CHECK(!has(cs, "key-in-pack:clean"));
  // No cover.
  CHECK(has(cs, "cover:clean"));
  CHECK(!has(cs, "cover:keyed"));
  // SafeDisc named once however many of its files there are, SecuROM too.
  CHECK(has(cs, "protection:safedisc:safedisc"));
  CHECK(has(cs, "protection:securom:safedisc"));
  CHECK_EQ(std::count_if(cs.begin(), cs.end(), [](const Check& c) { return c.game == "safedisc"; }), 2);
  CHECK(!has(cs, "protection:safedisc:clean"));
  // A pack that asked for the bundled dgVoodoo the player does not carry.
  CHECK(has(cs, "dgvoodoo-missing:dgv"));
  // Glide and nothing else.
  CHECK(has(cs, "glide:glide"));
  // Nothing about a game that is fine.
  CHECK(!has(cs, "glide:clean"));

  // Supplying dgVoodoo turns the missing note into the licence note.
  d.games[3].extra_dlls = {{"dgVoodoo.conf", "x"}, {"DDraw.dll", "MZ"}};
  cs = run_checks(d, facts);
  CHECK(has(cs, "dgvoodoo:dgv"));
  CHECK(!has(cs, "dgvoodoo-missing:dgv"));

  // Glide libraries beside an executable that imports no renderer at all.
  facts[4].imports = pe::parse(make_pe({"KERNEL32.dll", "USER32.dll"}));
  cs = run_checks(d, facts);
  CHECK(!has(cs, "glide:glide"));
  CHECK(has(cs, "glide-maybe:glide"));
  // A Glide game with a Direct3D renderer beside it is not Glide-only.
  CHECK(!glide_only(pe::parse(make_pe({"glide2x.dll", "ddraw.dll"}))));

  // Embedding: a warning to acknowledge, and a blocker without a key or a place.
  d.games[1].embed_key = true;
  cs = run_checks(d, facts);
  CHECK(has(cs, "key-embedded:clean"));
  CHECK(!has(cs, "key-removed:clean"));
  std::vector<std::string> bl = blockers(d, facts);
  CHECK(has_text(bl, "where The game clean's key goes"));
  d.games[1].key_path = "HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\EXAMPLE";
  d.games[1].key_value = "CDKey";
  CHECK(!has_text(blockers(d, facts), "key goes"));
  facts[1].vault_key.clear();
  CHECK(has_text(blockers(d, facts), "the keys vault has none"));
  facts[1].vault_key = "ABCD-1234-EFGH-5678";

  // Ready only when nothing blocks, every check is acknowledged and the
  // rights box is ticked.
  cs = run_checks(d, facts);
  bl = blockers(d, facts);
  CHECK(bl.empty());
  CHECK(!ready_to_build(d, cs, bl));
  for (const Check& c : cs) d.acknowledged.push_back(c.id);
  CHECK(!ready_to_build(d, cs, bl));
  d.rights = true;
  CHECK(ready_to_build(d, cs, bl));
  // A game that has left the shelf blocks; so does one that is there twice.
  facts[2].pack = nullptr;
  CHECK(has_text(blockers(d, facts), "no longer on the shelf"));
  facts[2].pack = &protected_;
  d.games.push_back(d.games[0]);
  facts.push_back(facts[0]);
  CHECK(has_text(blockers(d, facts), "in the bundle twice"));
  d.games.pop_back();
  facts.pop_back();

  Draft nothing;
  nothing.version = "1 0";
  bl = blockers(nothing, {});
  CHECK(has_text(bl, "no title"));
  CHECK(has_text(bl, "at least one game"));
  CHECK(has_text(bl, "cannot be part of a file name"));
  CHECK(has_text(bl, "Choose a folder"));
}

// ---- size -------------------------------------------------------------------------------------

static void test_size(const fs::path& tmp) {
  section("size, per part, and the 2 GiB and 4 GiB warnings");
  // A pack with its disc: the estimate shares its bytes out by unpacked size,
  // and the disc weighs three times the game.
  PackFacts withdisc = read_pack_facts(make_pack(tmp, tmp / "shelf", {"withdisc", "", {}, false, true, 40000}));
  CHECK_EQ(withdisc.discs_carried, size_t(1));
  CHECK(withdisc.without_discs < withdisc.bytes);
  CHECK(withdisc.without_discs > withdisc.bytes / 4 - 16);
  CHECK(withdisc.without_discs < withdisc.bytes / 4 + 16);
  PackFacts nodisc = read_pack_facts(make_pack(tmp, tmp / "shelf", {"nodisc", "", {}, false, false, 40000}));
  CHECK_EQ(nodisc.without_discs, nodisc.bytes);

  // The arithmetic, exactly as build_bundle lays a file out.
  PackFacts a, b;
  a.meta.id = "a";
  a.meta.name = "A";
  a.bytes = 10000;
  a.without_discs = 5000;
  b.meta.id = "b";
  b.bytes = 4096;
  b.without_discs = 4096;
  SizeReport r = size_report(100000, 500, {&a, &b});
  uint64_t tail = kTocHeaderSize + kRecordSize * 6 + kTrailerSize;
  // 100000 -> 102400, + 500 meta, -> 106496 + 10000, -> 118784 + 4096.
  CHECK_EQ(r.total, uint64_t(122880) + tail);
  // The same with 5000 for the first game: -> 106496 + 5000, -> 114688 + 4096.
  CHECK_EQ(r.total_without_discs, uint64_t(118784) + tail);
  CHECK_EQ(r.games.size(), size_t(2));
  CHECK_EQ(r.games[0].label, std::string("A"));
  CHECK_EQ(r.games[1].label, std::string("b"));  // no name: the id
  CHECK_EQ(r.runtime.bytes, uint64_t(100000));
  CHECK(r.warnings.empty());

  // One byte under, exactly at, and over the limits. The base is a page, the
  // meta one byte, so the pack starts at 8192 and the tail follows it.
  auto total_for = [&](uint64_t pack) {
    PackFacts p;
    p.meta.id = "p";
    p.bytes = pack;
    p.without_discs = pack;
    return size_report(4096, 1, {&p});
  };
  uint64_t fixed = 8192 + kTocHeaderSize + kRecordSize * 5 + kTrailerSize;
  CHECK_EQ(total_for(kWarn2G - fixed - 1).total, kWarn2G - 1);
  CHECK(total_for(kWarn2G - fixed - 1).warnings.empty());
  CHECK_EQ(total_for(kWarn2G - fixed).total, kWarn2G);
  CHECK(has_text(total_for(kWarn2G - fixed).warnings, "over 2 GiB"));
  CHECK(has_text(total_for(kWarn4G - fixed - 1).warnings, "over 2 GiB"));
  CHECK(has_text(total_for(kWarn4G - fixed).warnings, "over 4 GiB"));
  SizeReport over2 = total_for(kWarn2G);
  CHECK(over2.total >= kWarn2G);
  CHECK_EQ(over2.warnings.size(), size_t(1));
  CHECK(has_text(over2.warnings, "over 2 GiB"));
  SizeReport over4 = total_for(kWarn4G);
  CHECK(has_text(over4.warnings, "over 4 GiB"));
  CHECK(has_text(over4.warnings, "FAT32"));
  CHECK(!has_text(over4.warnings, "over 2 GiB"));

  // Over the limit with its discs and under it without: said, with the way out.
  PackFacts big;
  big.meta.id = "big";
  big.bytes = kWarn2G + (100ull << 20);
  big.without_discs = 1ull << 30;
  SizeReport r2 = size_report(300ull << 20, 4096, {&big});
  CHECK(has_text(r2.warnings, "over 2 GiB"));
  CHECK(has_text(r2.warnings, "Without their discs"));
}

// ---- auto backend ---------------------------------------------------------------------------

static void test_auto_backend(const fs::path& tmp) {
  section("what auto means, from the executable's imports");
  auto pick = [](std::vector<std::string> dlls) { return auto_backend(pe::parse(make_pe(dlls))); };
  CHECK_EQ(pick({"KERNEL32.dll", "d3d9.dll"}).backend, std::string("dxvk"));
  CHECK_EQ(pick({"D3D8.DLL"}).backend, std::string("dxvk"));
  CHECK_EQ(pick({"DDRAW.dll", "WINMM.dll"}).backend, std::string("cnc-ddraw"));
  CHECK_EQ(pick({"opengl32.dll", "ddraw.dll"}).backend, std::string("native OpenGL"));
  CHECK_EQ(pick({"KERNEL32.dll"}).backend, std::string("wined3d-vk"));
  CHECK(pick({"d3d9.dll"}).known);
  CHECK(pick({"d3d9.dll"}).reason.find("d3d9") != std::string::npos);

  // Direct3D 7 through DirectDraw: the interface id in its bytes, not its imports.
  std::vector<uint8_t> d3d7 = make_pe({"DDRAW.dll"});
  const uint8_t iid7[16] = {0x77, 0x9e, 0x04, 0xf5, 0x61, 0x48, 0xd2, 0x11,
                            0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8};
  std::memcpy(&d3d7[0x380], iid7, 16);
  CHECK_EQ(auto_backend(pe::parse(d3d7)).backend, std::string("wined3d-vk"));

  // Unreadable: not known, and the policy's own fallback.
  pe::Imports junk = pe::parse(std::vector<uint8_t>(64, 0));
  CHECK(!auto_backend(junk).known);
  CHECK(glide_only(pe::parse(make_pe({"glide3x.dll", "KERNEL32.dll"}))));
  CHECK(!glide_only(junk));

  // Out of a real pack: GAME.EXE inside a DwarFS body, rooted, named in a
  // different case than the tree has it.
  std::string tool;
  if (const char* t = std::getenv("KRETRO_DWARFS"); t && *t) tool = t;
  else if (fs::exists("build/dwarfs-universal")) tool = fs::absolute("build/dwarfs-universal").string();
  if (tool.empty()) {
    std::fprintf(stderr, "  skip: no dwarfs tool (build/dwarfs-universal or KRETRO_DWARFS) for the pack read\n");
    return;
  }
  fs::path img = tmp / "img";
  fs::remove_all(img);
  write_file(img / "game" / "Bin" / "Game.EXE", as_string(make_pe({"KERNEL32.dll", "d3d8.dll"})));
  write_file(img / "game" / "readme.txt", "hello");
  write_file(img / "registry.reg", "REGEDIT4\n");
  fs::path body = tmp / "img.dwarfs";
  ProcResult mk = kg::run({tool, "--tool=mkdwarfs", "-i", img.string(), "-o", body.string(), "--log-level=error",
                           "--force"});
  CHECK(mk.ok());
  if (!mk.ok()) {
    std::fprintf(stderr, "  mkdwarfs: %s\n", mk.out.c_str());
    return;
  }
  Meta m;
  m.id = "example";
  m.name = "EXAMPLE";
  m.layout = "rooted";
  m.run.exe = "bin\\game.exe";
  m.tree = Tree::from_directory(img / "game");
  fs::path pack = tmp / "example.kgpack";
  write_pack(pack, m, WriteOptions{kg::Kind::Game, body, false});
  pe::Imports got = read_exe_imports(read_pack_facts(pack), tool, tmp / "scratch-exe");
  CHECK(got.ok);
  CHECK(got.imports("d3d8"));
  CHECK_EQ(auto_backend(got).backend, std::string("dxvk"));
  CHECK(!fs::exists(tmp / "scratch-exe"));  // emptied again

  // Nothing to read, said rather than thrown.
  PackFacts none = read_pack_facts(pack);
  none.meta.run.exe = "missing.exe";
  pe::Imports miss = read_exe_imports(none, tool, tmp / "scratch-exe");
  CHECK(!miss.ok);
  CHECK(miss.error.find("not in the pack") != std::string::npos);
  CHECK(!read_exe_imports(read_pack_facts(pack), "", tmp / "scratch-exe").ok);
}

// ---- preview --------------------------------------------------------------------------------

static void test_preview_env(const fs::path& tmp) {
  section("a preview inherits nothing of kretro's");
  std::vector<std::string> parent = {
      "KRETRO_SELF=/home/a/kretro",
      "KRETRO_TOC=4096:512",
      "KRETRO_RUNTIME=/run/user/1000/kretro/rt-abc",
      "KRETRO_DWARFS=/proc/self/fd/5",
      "KRETRO_STATE=/home/a/kretro-data",
      "KRETRO_MOUNT_MODE=fusermount",
      "HOME=/home/a",
      "XDG_CONFIG_HOME=/home/a/.config",
      "XDG_DATA_HOME=/home/a/.local/share",
      "XDG_CACHE_HOME=/home/a/.cache",
      "XDG_RUNTIME_DIR=/run/user/1000",
      "DISPLAY=:0",
      "WAYLAND_DISPLAY=wayland-0",
      "PATH=/run/user/1000/kretro/rt-abc/usr/bin:/usr/bin:/bin",
      "LD_LIBRARY_PATH=/run/user/1000/kretro/rt-abc/lib",
      "FONTCONFIG_PATH=/run/user/1000/kretro/rt-abc/etc/fonts",
      "XDG_DATA_DIRS=/usr/share:/run/user/1000/kretro/rt-abc/usr/share",
      "MYRUNTIME=/run/user/1000/kretro/rt-abcdef",  // a sibling, not inside
      "LANG=en_GB.UTF-8",
      "EMPTY=",
      "garbage-without-equals",
  };
  fs::path scratch = tmp / "preview";
  std::vector<std::string> env = preview_env(parent, "/run/user/1000/kretro/rt-abc/", scratch);
  auto get = [&](const std::string& k) -> std::optional<std::string> {
    for (const std::string& kv : env) {
      if (kv.rfind(k + "=", 0) == 0) return kv.substr(k.size() + 1);
    }
    return std::nullopt;
  };
  for (const std::string& kv : env) CHECK(kv.rfind("KRETRO_", 0) != 0);
  CHECK_EQ(get("HOME").value_or(""), (scratch / "home").string());
  CHECK_EQ(get("XDG_CONFIG_HOME").value_or(""), (scratch / "home" / ".config").string());
  CHECK_EQ(get("XDG_DATA_HOME").value_or(""), (scratch / "home" / ".local/share").string());
  CHECK_EQ(get("XDG_CACHE_HOME").value_or(""), (scratch / "home" / ".cache").string());
  CHECK(get("XDG_STATE_HOME").has_value());
  CHECK_EQ(std::count_if(env.begin(), env.end(), [](const std::string& s) { return s.rfind("HOME=", 0) == 0; }), 1);
  // What a stranger has too.
  CHECK_EQ(get("XDG_RUNTIME_DIR").value_or(""), std::string("/run/user/1000"));
  CHECK_EQ(get("DISPLAY").value_or(""), std::string(":0"));
  CHECK_EQ(get("WAYLAND_DISPLAY").value_or(""), std::string("wayland-0"));
  CHECK_EQ(get("LANG").value_or(""), std::string("en_GB.UTF-8"));
  CHECK(get("EMPTY").has_value());
  // Lists keep what is not kretro's; anything that is only kretro's goes.
  CHECK_EQ(get("PATH").value_or(""), std::string("/usr/bin:/bin"));
  CHECK_EQ(get("XDG_DATA_DIRS").value_or(""), std::string("/usr/share"));
  CHECK(!get("LD_LIBRARY_PATH").has_value());
  CHECK(!get("FONTCONFIG_PATH").has_value());
  CHECK_EQ(get("MYRUNTIME").value_or(""), std::string("/run/user/1000/kretro/rt-abcdef"));
  CHECK(!has_text(env, "garbage"));

  // No runtime known: KRETRO_* still goes, PATH is left alone.
  std::vector<std::string> env2 = preview_env(parent, "", scratch);
  CHECK(!has_text(env2, "KRETRO_"));
  CHECK(has_text(env2, "PATH=/run/user/1000/kretro/rt-abc/usr/bin:/usr/bin:/bin"));

  // Run for real: a script that says what it was given, then exits 3.
  fs::path script = tmp / "fake.run";
  write_file(script,
             "#!/bin/sh\n"
             "echo \"home=$HOME\"\n"
             "echo \"cwd=$(pwd)\"\n"
             "env | grep -c '^KRETRO_' || true\n"
             "printf 'no newline at the end'\n"
             "exit 3\n");
  chmod(script.c_str(), 0755);
  setenv("KRETRO_SELF", "/leak", 1);
  Preview pv;
  pv.start(script, {}, preview_env(current_env(), "", scratch), scratch);
  CHECK(pv.running());
  std::vector<std::string> out;
  for (int i = 0; i < 200 && (pv.running() || out.empty()); ++i) {
    for (std::string& l : pv.poll()) out.push_back(l);
    usleep(10 * 1000);
  }
  for (std::string& l : pv.poll()) out.push_back(l);
  CHECK(!pv.running());
  CHECK_EQ(pv.status(), 3);
  CHECK(has_text(out, "home=" + (scratch / "home").string()));
  CHECK(has_text(out, "cwd=" + (scratch / "home").string()));
  CHECK(std::find(out.begin(), out.end(), "0") != out.end());  // no KRETRO_ reached it
  CHECK(has_text(out, "no newline at the end"));
  CHECK(!fs::exists(scratch));  // thrown away when it ended
  unsetenv("KRETRO_SELF");

  // Stop reaches a player that would run forever.
  fs::path forever = tmp / "forever.run";
  write_file(forever, "#!/bin/sh\necho started\nsleep 600 & wait\n");
  chmod(forever.c_str(), 0755);
  Preview pv2;
  pv2.start(forever, {}, preview_env(current_env(), "", scratch), scratch);
  auto t0 = std::chrono::steady_clock::now();
  pv2.stop();
  CHECK(!pv2.running());
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
  CHECK(!fs::exists(scratch));

  CHECK_THROWS_WITH(pv2.start(tmp / "not-built.run", {}, {}, scratch), "build it first");

  // Where a preview runs, from the id as the author left it in the field. A
  // '..' in it would put the scratch outside the cache, and the scratch is
  // what the preview removes: a directory beside the cache must survive.
  fs::path cache = tmp / "state" / "cache";
  fs::create_directories(cache);
  write_file(tmp / "state" / "precious" / "keep", "x");
  CHECK_EQ(preview_scratch(cache, "retro-shelf"), cache / "preview-retro-shelf");
  CHECK_THROWS_WITH(preview_scratch(cache, "../precious"), "not a name a directory can have");
  CHECK_THROWS_WITH(preview_scratch(cache, "a/b"), "not a name");
  CHECK_THROWS_WITH(preview_scratch(cache, ""), "not a name");
  CHECK(fs::exists(tmp / "state" / "precious" / "keep"));
}

// ---- the build the page runs ---------------------------------------------------------------

static void test_build_from_draft(const fs::path& tmp) {
  section("the build the page's button runs");
  fs::path games = tmp / "games";
  make_pack(tmp, games, {"example", kCleanFragment, {}, false, false, 30000});
  make_pack(tmp, games, {"classic2", "", {}, false, false, 25000});
  fs::path keys = tmp / "keys.txt";
  std::vector<install::StoredKey> vault;
  install::put_key(vault, "example", "ABCD-1234-EFGH-5678", "sleeve");
  install::save_keys(keys, vault);
  fs::path out = tmp / "out";
  fs::create_directories(out);

  Draft d;
  d.title = "Two Games";
  d.id = id_from_title(d.title);
  d.version = "0.1";
  d.out_dir = out.string();
  d.rights = true;
  for (const char* id : {"classic2", "example"}) {
    DraftGame g = game_from_pack(read_pack_facts(games / (std::string(id) + ".kgpack")));
    d.games.push_back(g);
  }
  d.games[1].embed_key = true;
  d.games[1].key_path = "HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\EXAMPLE";
  d.games[1].key_value = "CDKey";

  // kretro with no player base, as a development build is linked.
  fs::path base = make_base(tmp);
  unsetenv("KRETRO_PLAYER_BASE");
  const std::string tool = dwarfs_tool();
  BuildInputs in{base, games, keys, tool, tmp / "cache", {}};
  CHECK_THROWS_WITH(build_from_draft(d, in), "make player-base");
  in.self.clear();
  CHECK_THROWS_WITH(build_from_draft(d, in), "KRETRO_SELF");
  CHECK(fs::is_empty(out));

  // kretro carrying one.
  write_file(tmp / "in" / "kretro-app", std::string(4000, 'k'));
  fs::path kretro = tmp / "kretro";
  link_file(kretro, tmp / "in" / "bootstrap",
            {{BKind::Tools, tmp / "in" / "tools", ""},
             {BKind::Runtime, tmp / "in" / "runtime", ""},
             {BKind::App, tmp / "in" / "kretro-app", ""},
             {BKind::PlayerBase, base, ""}});
  in.self = kretro;

  uint64_t last_done = 0;
  bool went_backwards = false;
  Callbacks cb;
  cb.progress = [&](const Progress& p) {
    if (p.done < last_done) went_backwards = true;
    last_done = p.done;
  };
  Built b = build_from_draft(d, in, cb);
  CHECK_EQ(b.path, out / "two-games-0.1.run");
  CHECK(!went_backwards);
  CHECK(last_done > 0);
  CHECK(!fs::exists(out / "two-games-0.1.run.partial"));
  Verified v = verify_bundle(b.path);
  CHECK_EQ(v.meta.id, std::string("two-games"));
  CHECK_EQ(v.meta.games.size(), size_t(2));
  CHECK_EQ(v.meta.games[0].id, std::string("classic2"));  // in draft order
  CHECK(v.meta.rights_acknowledged);
  CHECK(!v.meta.games[0].key.has_value());
  CHECK(v.meta.games[1].key.has_value());
  if (v.meta.games[1].key) {
    CHECK_EQ(v.meta.games[1].key->value, std::string("ABCD-1234-EFGH-5678"));
    CHECK_EQ(v.meta.games[1].key->registry_value, std::string("CDKey"));
    CHECK_EQ(v.meta.games[1].key->view, std::string("32"));  // no exe to read: the era's bitness
  }
  // The player base's own runtime's notices, read out of its DwarFS image.
  if (!tool.empty()) CHECK((v.meta.licenses == std::vector<std::string>{"dxvk", "wine"}));
  else CHECK(v.meta.licenses.empty());
  CHECK_EQ(v.toc.all(BKind::Pack).size(), size_t(2));

  // The size the page promised is the size written.
  std::vector<PackFacts> facts;
  for (const DraftGame& g : d.games) facts.push_back(read_pack_facts(games / (g.id + ".kgpack")));
  std::vector<const PackFacts*> ptrs;
  for (const PackFacts& f : facts) ptrs.push_back(&f);
  BaseSource src = find_player_base(kretro);
  BundleMeta meta = meta_from_draft(d, install::load_keys(keys), v.meta.built_at, v.meta.licenses);
  SizeReport sr = size_report(player_base_bytes(src), meta.encode().size(), ptrs);
  CHECK_EQ(sr.total, b.size);

  // Cancelled halfway: nothing is left, not even the .partial.
  fs::remove(b.path);
  Callbacks stop;
  int asked = 0;
  stop.cancelled = [&] { return ++asked > 2; };
  bool cancelled = false;
  try {
    build_from_draft(d, in, stop);
  } catch (const Cancelled&) {
    cancelled = true;
  }
  CHECK(cancelled);
  CHECK(fs::is_empty(out));

  // The embedded key the vault no longer has: refused, and nothing written.
  install::save_keys(keys, {});
  CHECK_THROWS_WITH(build_from_draft(d, in), "no key for example");
  CHECK(fs::is_empty(out));

  // An id that is not one path component would write outside the folder.
  Draft stray = d;
  stray.games[1].embed_key = false;
  stray.id = "../escaped";
  CHECK_THROWS_WITH(build_from_draft(stray, in), "cannot be part of a file name");
  CHECK(fs::is_empty(out));
  CHECK(!fs::exists(tmp / "escaped-0.1.run"));

  // Not ticked: refused before anything.
  d.games[1].embed_key = false;
  d.rights = false;
  CHECK_THROWS_WITH(build_from_draft(d, in), "right to distribute");
  d.rights = true;

  // A game gone from the shelf.
  fs::rename(games / "example.kgpack", tmp / "example.away");
  CHECK_THROWS_WITH(build_from_draft(d, in), "no longer on the shelf");
  fs::rename(tmp / "example.away", games / "example.kgpack");

  // KRETRO_PLAYER_BASE: a development tree's own player base, whole.
  setenv("KRETRO_PLAYER_BASE", base.c_str(), 1);
  in.self.clear();
  Built b2 = build_from_draft(d, in);
  CHECK(verify_bundle(b2.path).meta.games.size() == 2u);
  unsetenv("KRETRO_PLAYER_BASE");

  // The command line's --base: a base file named outright, kretro not asked.
  fs::remove(b2.path);
  in.base = base;
  Built b3 = build_from_draft(d, in);
  CHECK(verify_bundle(b3.path).meta.games.size() == 2u);
  in.base = tmp / "no-such-base";
  CHECK_THROWS_WITH(build_from_draft(d, in), "no-such-base");

  // The command line's -o FILE: built at that name, and nothing else in the
  // folder touched - not a player already there under <id>-<version>.run,
  // which a build-then-rename would have replaced and then moved away.
  in.base = base;
  fs::remove(b3.path);
  write_file(out / "two-games-0.1.run", "a player shipped last week");
  in.out = out / "named.run";
  Built b4 = build_from_draft(d, in);
  CHECK_EQ(b4.path, out / "named.run");
  CHECK(verify_bundle(b4.path).meta.games.size() == 2u);
  CHECK_EQ(slurp(out / "two-games-0.1.run"), std::string("a player shipped last week"));
  // And -o naming an input is refused before a byte is written: here the base,
  // which for kretro is kretro's own file.
  const std::string base_bytes = slurp(base);
  in.out = base;
  CHECK_THROWS_WITH(build_from_draft(d, in), "which it is built from");
  CHECK(slurp(base) == base_bytes);
  in.out = games / "example.kgpack";
  CHECK_THROWS_WITH(build_from_draft(d, in), "which it is built from");
  CHECK_EQ(read_pack_facts(games / "example.kgpack").meta.id, std::string("example"));
  in.out.clear();
}

// ---- the contract between the builder and the player -----------------------------------

// What the Bundles page writes into bundle.meta, read back by the code the
// player runs on it: the gamepad map through the helper's bindings, the key
// through key_registry, the author's files through the extra layer, the
// licence list against the base's own runtime. Two halves written on two
// tracks meet here, in one test, or they drift.
static void test_contract(const fs::path& tmp) {
  section("the builder's bundle.meta, read the way the player reads it");
  const std::string tool = dwarfs_tool();
  fs::path games = tmp / "contract-games";
  make_pack(tmp, games, {"example", kCleanFragment, {}, false, false, 20000, "Bin/GAME.exe"});
  fs::path keys = tmp / "contract-keys.txt";
  std::vector<install::StoredKey> vault;
  install::put_key(vault, "example", "ABCD-1234-EFGH-5678", "sleeve");
  install::save_keys(keys, vault);
  fs::path out = tmp / "contract-out";
  fs::create_directories(out);

  Draft d;
  d.title = "Contract";
  d.id = id_from_title(d.title);
  d.version = "1";
  d.out_dir = out.string();
  d.rights = true;
  DraftGame g = game_from_pack(read_pack_facts(games / "example.kgpack"));
  CHECK_EQ(g.gamepad, std::string("a=Return\n"));  // the pack's own, in the one form
  // What a person types into the page's box: spaces, and more than one a line.
  g.gamepad += " start = p ; b=Escape\nnonsense\n";
  g.extra_dlls = {{"D3D8.dll", "MZ d3d8"}, {"dgVoodoo.conf", "[General]"}};
  g.embed_key = true;
  g.key_path = "HKLM\\Software\\Example Publisher\\EXAMPLE";
  g.key_value = "CDKey";
  d.games.push_back(g);

  fs::path base = make_base(tmp / "contract");
  BuildInputs in{{}, games, keys, tool, tmp / "contract-cache", base};
  Built b = build_from_draft(d, in);
  player::Bundle pb = player::Bundle::open(b.path);
  const GameMeta* gm = pb.game("example");
  CHECK(gm != nullptr);
  if (!gm) return;
  const Entry* e = pb.pack("example");
  Meta pm = Pack::open(pb.self, pb.toc.at(*e), e->len).meta();

  // The gamepad: the pack's own map, the author's over it.
  player::GameSettings s = player::default_settings(gm->display, gm->fullscreen);
  CHECK_EQ(s.gamepad, std::string("author"));
  std::map<std::string, std::string> binds = player::gamepad_bindings(pm.input, *gm, s);
  CHECK_EQ(binds["a"], std::string("Return"));
  CHECK_EQ(binds["start"], std::string("p"));
  CHECK_EQ(binds["b"], std::string("Escape"));
  CHECK_EQ(binds.size(), size_t(3));
  s.gamepad = "kretro";
  CHECK((player::gamepad_bindings(pm.input, *gm, s) == pm.input));
  CHECK((parse_gamepad(format_gamepad(pm.input)) == pm.input));

  // The key: this exe is 32-bit (read out of a real body when there is a
  // tool, taken as 32-bit when there is none), so it lands under Wow6432Node.
  CHECK(gm->key.has_value());
  if (gm->key) {
    CHECK_EQ(gm->key->view, std::string("32"));
    CHECK(player::key_registry(*gm->key).find("[HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Example Publisher\\EXAMPLE]\n\"CDKey\"=") !=
          std::string::npos);
  }
  BundleMeta m64 = meta_from_draft(d, vault, "", {}, {{"example", true}});
  CHECK_EQ(m64.games[0].key->view, std::string("64"));
  CHECK(player::key_registry(*m64.games[0].key).find("[HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\EXAMPLE]") !=
        std::string::npos);
  CHECK((BundleMeta::decode(m64.encode()) == m64));

  // The author's files: beside the executable, in Bin/, in a layer of their
  // own that is not the game's writable one.
  fs::path layer = tmp / "contract-layer";
  CHECK_EQ(player::exe_dir_in_tree(pm), std::string("Bin"));
  CHECK_EQ(player::stage_extra_layer(gm->extra_dlls, player::exe_dir_in_tree(pm), layer), std::string("d3d8=n,b"));
  CHECK_EQ(slurp(layer / "Bin" / "D3D8.dll"), std::string("MZ d3d8"));
  CHECK_EQ(slurp(layer / "Bin" / "dgVoodoo.conf"), std::string("[General]"));
  CHECK(!fs::exists(layer / "D3D8.dll"));

  // The licences: the player base's runtime's, not kretro's.
  if (!tool.empty()) CHECK((pb.meta.licenses == std::vector<std::string>{"dxvk", "wine"}));
}

// ---- for real ---------------------------------------------------------------------------

// test_builder real <kretro> <out-dir> <game-id>... : the page's build, against
// the shelf in KRETRO_STATE, timed.
static int real(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: test_builder real <kretro> <out-dir> <game-id>...\n");
    return 2;
  }
  Draft d;
  d.title = "kretro builder check";
  d.id = id_from_title(d.title);
  d.version = "0.1";
  d.out_dir = argv[3];
  d.rights = true;
  for (int i = 4; i < argc; ++i) {
    PackFacts f = read_pack_facts(games_dir() / (std::string(argv[i]) + ".kgpack"));
    d.games.push_back(game_from_pack(f));
    std::fprintf(stderr, "%s: %llu bytes, %zu disc(s) carried, about %llu without\n", argv[i],
                 (unsigned long long)f.bytes, f.discs_carried, (unsigned long long)f.without_discs);
    const char* tool = std::getenv("KRETRO_DWARFS");
    pe::Imports im = read_exe_imports(f, tool ? tool : "build/dwarfs-universal", fs::path(d.out_dir) / ".exe-scratch");
    AutoBackend ab = auto_backend(im);
    std::fprintf(stderr, "  auto: %s (%s)%s%s\n", ab.backend.c_str(), ab.reason.c_str(), im.ok ? "" : " - ",
                 im.ok ? "" : im.error.c_str());
  }
  const char* dw = std::getenv("KRETRO_DWARFS");
  BuildInputs in{argv[2], games_dir(), install::keys_file(), dw ? dw : "build/dwarfs-universal", cache_dir(), {}};
  auto t0 = std::chrono::steady_clock::now();
  Built b = build_from_draft(d, in);
  auto t1 = std::chrono::steady_clock::now();
  Verified v = verify_bundle(b.path);
  auto t2 = std::chrono::steady_clock::now();
  auto secs = [](auto a, auto z) { return std::chrono::duration<double>(z - a).count(); };
  std::printf("built %s\n  %llu bytes, build (copy + verify) %.1fs, verify again %.1fs, %zu game(s), %zu entries\n",
              b.path.c_str(), (unsigned long long)b.size, secs(t0, t1), secs(t1, t2), v.meta.games.size(),
              v.toc.entries.size());
  std::vector<PackFacts> facts;
  for (const DraftGame& g : d.games) facts.push_back(read_pack_facts(games_dir() / (g.id + ".kgpack")));
  std::vector<const PackFacts*> ptrs;
  for (const PackFacts& f : facts) ptrs.push_back(&f);
  SizeReport sr = size_report(player_base_bytes(find_player_base(argv[2])), v.meta.encode().size(), ptrs);
  std::printf("  the size step said %llu bytes (about %llu without discs)%s\n", (unsigned long long)sr.total,
              (unsigned long long)sr.total_without_discs, sr.total == b.size ? ", exactly right" : ", WRONG");
  for (const std::string& w : sr.warnings) std::printf("  warning: %s\n", w.c_str());

  // The preview, as the page runs it. PREVIEW_ARGS is what to pass: the
  // stand-in app answers `info`, which is enough to see the bootstrap reach
  // the runtime inside the built file with none of kretro's environment.
  if (const char* args = std::getenv("PREVIEW_ARGS")) {
    std::vector<std::string> a;
    std::istringstream is(args);
    for (std::string w; is >> w;) a.push_back(w);
    fs::path scratch = fs::path(d.out_dir) / ".preview";
    Preview pv;
    const char* rt = std::getenv("KRETRO_RUNTIME");
    pv.start(b.path, a, preview_env(current_env(), rt ? rt : "", scratch), scratch);
    auto p0 = std::chrono::steady_clock::now();
    while (pv.running() && secs(p0, std::chrono::steady_clock::now()) < 180) {
      for (const std::string& l : pv.poll()) std::printf("  | %s\n", l.c_str());
      usleep(50 * 1000);
    }
    for (const std::string& l : pv.poll()) std::printf("  | %s\n", l.c_str());
    if (pv.running()) pv.stop();
    std::printf("  preview ended with status %d after %.1fs; scratch home %s\n", pv.status(),
                secs(p0, std::chrono::steady_clock::now()), fs::exists(scratch) ? "LEFT BEHIND" : "removed");
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "real") {
    try {
      return real(argc, argv);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "failed: %s\n", e.what());
      return 1;
    }
  }
  fs::path tmp = fs::temp_directory_path() / "kretro-test-builder";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  try {
    test_slug();
    test_remember(tmp);
    test_remember_built(tmp);
    test_repack(tmp);
    test_fragment();
    test_checks(tmp);
    test_size(tmp);
    test_auto_backend(tmp);
    test_preview_env(tmp);
    test_build_from_draft(tmp);
    test_contract(tmp);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  FAIL unexpected exception: %s\n", e.what());
    ++failures;
  }

  fs::remove_all(tmp);
  std::fprintf(stderr, "\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
