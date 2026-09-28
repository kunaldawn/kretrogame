// Tier 1 unit tests: no GPU, no container, no game data.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "pack/kgpack.h"
#include "pack/tree.h"
#include "util/bytes.h"
#include "util/cbor.h"
#include "util/hash.h"
#include "util/toml.h"
#include "support/check.h"
#include "support/files.h"

namespace fs = std::filesystem;
using namespace kg;

using kgtest::section;
using kgtest::write_file;

static void test_hash() {
  section("hash");
  // Official BLAKE3 test vectors.
  CHECK_EQ(to_hex(hash_string("")),
           "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
  CHECK_EQ(to_hex(hash_string("abc")),
           "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");

  Hash h = hash_string("abc");
  CHECK_EQ(from_hex(to_hex(h)), h);
  CHECK_THROWS(from_hex("tooshort"));
  CHECK_THROWS(from_hex(std::string(64, 'z')));

  Hasher inc;
  inc.update("a");
  inc.update("b");
  inc.update("c");
  CHECK_EQ(inc.finish(), hash_string("abc"));
}

static void test_cbor() {
  section("cbor");
  cbor::Encoder e;
  e.map(4);
  e.text("n"); e.uint_val(1998);
  e.text("s"); e.text("Classic 2");
  e.text("list"); e.array(2); e.text("a"); e.text("b");
  e.text("flag"); e.boolean(true);

  cbor::Value v = cbor::decode(e.data());
  CHECK(v.is_map());
  CHECK_EQ(v.find("n")->uint_or(), 1998u);
  CHECK_EQ(v.find("s")->text_or(), std::string("Classic 2"));
  CHECK(v.find("list")->is_array());
  CHECK_EQ(v.find("list")->arr.size(), 2u);
  CHECK_EQ(v.find("flag")->bool_or(), true);
  CHECK(v.find("absent") == nullptr);

  // Large integers must survive every width the encoder picks.
  for (uint64_t n : {0ull, 23ull, 24ull, 255ull, 256ull, 65535ull, 65536ull,
                     4294967295ull, 4294967296ull, ~0ull}) {
    cbor::Encoder one;
    one.uint_val(n);
    CHECK_EQ(cbor::decode(one.data()).uint_or(), n);
  }

  // Hostile input: a pack from someone else must never be trusted.
  CHECK_THROWS(cbor::decode(std::string_view("\x1a\x00\x00", 3)));      // truncated
  CHECK_THROWS(cbor::decode(std::string_view("\x01\x01", 2)));          // trailing bytes
  CHECK_THROWS(cbor::decode(std::string_view("\x9b\xff\xff\xff\xff\xff\xff\xff\xff", 9)));  // absurd array length
  CHECK_THROWS(cbor::decode(std::string_view("\x5b\xff\xff\xff\xff\xff\xff\xff\xff", 9)));  // absurd byte length
  std::string deep;
  for (int i = 0; i < 200; ++i) deep.push_back('\x81');  // 200 nested arrays
  deep.push_back('\x01');
  CHECK_THROWS(cbor::decode(deep));
}

static void test_tree(const fs::path& tmp) {
  section("tree");
  fs::path root = tmp / "game";
  write_file(root / "CLASSIC2.EXE", "MZ fake executable");
  write_file(root / "DATA" / "worldmap.dat", "worldmap");
  write_file(root / "readme.txt", "read me");
  fs::create_directories(root / "empty");
  fs::create_symlink("readme.txt", root / "link.txt");

  Tree t = Tree::from_directory(root);
  CHECK_EQ(t.size(), 6u);  // 3 files + 1 dir DATA + 1 dir empty + 1 symlink

  // Canonical form is sorted, so hashing it twice gives the same root, and
  // re-walking the same tree gives the same root again.
  CHECK_EQ(t.root(), Tree::from_directory(root).root());
  CHECK_EQ(Tree::from_canonical(t.canonical()).root(), t.root());
  CHECK_EQ(Tree::from_canonical(t.canonical()).canonical(), t.canonical());

  CHECK_EQ(t.total_bytes(), std::string("MZ fake executable").size() +
                                std::string("worldmap").size() +
                                std::string("read me").size());

  // A changed file, a new file and a deleted file are each reported once.
  write_file(root / "readme.txt", "read me, changed");
  write_file(root / "SAVEGAME" / "SLOT01.SAV", "a save");
  fs::remove(root / "DATA" / "worldmap.dat");
  Tree after = Tree::from_directory(root);
  Tree::Diff d = t.diff_to(after);
  CHECK_EQ(d.changed.size(), 1u);
  CHECK_EQ(d.changed[0].path, std::string("readme.txt"));
  CHECK_EQ(d.removed.size(), 1u);
  CHECK_EQ(d.removed[0].path, std::string("DATA/worldmap.dat"));
  CHECK_EQ(d.added.size(), 2u);  // SAVEGAME/ and SAVEGAME/SLOT01.SAV
  CHECK(t.diff_to(t).empty());
}

static void test_pack_roundtrip(const fs::path& tmp) {
  section("kgpack round trip");
  fs::path root = tmp / "game";
  fs::path out = tmp / "classic2.kgpack";

  Meta m;
  m.id = "classic2";
  m.name = "Classic 2";
  m.year = 1998;
  m.developer = "Example Studios";
  m.run.exe = "CLASSIC2.EXE";
  m.run.width = 640;
  m.run.height = 480;
  m.run.windows_version = "win98";
  m.runtime.id = "wine-11.16";
  m.runtime.blake3 = hash_string("a runtime");
  m.runtime.winetricks = {"corefonts", "dsound"};
  m.recipe.method = "unzip";
  m.recipe.member = "Data2.zip";
  m.recipe.verify = {"MAIN.DAT", "ACTORS.DAT"};
  DiscFingerprint fp;
  fp.filename = "Classic Collection (USA).iso";
  fp.size = 3865470976ull;
  fp.blake3 = hash_string("the disc");
  fp.volume_id = "CLASSIC_COLLECTION";
  fp.anchors.push_back(Anchor{"Data2.zip", 654311424ull, hash_string("an anchor")});
  m.recipe.fingerprints.push_back(fp);
  m.tree = Tree::from_directory(root);

  // A recipe pack: no body, but a full tree and a real Merkle root.
  write_pack(out, set_of(m), WriteOptions{PackKind::Game, std::nullopt, false});
  Pack p = Pack::open(out);
  CHECK(!p.has_body());
  CHECK_EQ(p.header().kind, PackKind::Game);
  CHECK_EQ(p.header().blake3_root, set_of(m).root());
  CHECK_EQ(p.games()[0].id, std::string("classic2"));
  CHECK_EQ(p.games()[0].name, std::string("Classic 2"));
  CHECK_EQ(p.games()[0].year, 1998u);
  CHECK_EQ(p.games()[0].developer, std::string("Example Studios"));
  CHECK_EQ(p.games()[0].run.exe, std::string("CLASSIC2.EXE"));
  CHECK_EQ(p.games()[0].run.width, 640u);
  CHECK_EQ(p.games()[0].runtime.blake3, hash_string("a runtime"));
  CHECK_EQ(p.games()[0].runtime.winetricks.size(), 2u);
  CHECK_EQ(p.games()[0].recipe.member, std::string("Data2.zip"));
  CHECK_EQ(p.games()[0].recipe.fingerprints.size(), 1u);
  CHECK_EQ(p.games()[0].recipe.fingerprints[0].volume_id, std::string("CLASSIC_COLLECTION"));
  CHECK_EQ(p.games()[0].recipe.fingerprints[0].anchors.size(), 1u);
  CHECK_EQ(p.games()[0].recipe.fingerprints[0].anchors[0].hash, hash_string("an anchor"));
  CHECK_EQ(p.games()[0].tree.canonical(), m.tree.canonical());
  CHECK(p.verify().ok);

  // A capsule pack: same metadata, same root, plus a body.
  fs::path body = tmp / "body.bin";
  write_file(body, std::string(100000, 'x'));
  fs::path cap = tmp / "classic2.capsule.kgpack";
  write_pack(cap, set_of(m), WriteOptions{PackKind::Game, body, false});
  Pack c = Pack::open(cap);
  CHECK(c.has_body());
  CHECK_EQ(c.header().body_len, 100000u);
  CHECK_EQ(c.header().body_off % kBodyAlign, 0u);
  CHECK(c.verify().ok);

  // The property the whole format exists for: a recipe pack and a capsule pack
  // of the same install carry the same root, so a rebuild can be proven.
  CHECK_EQ(c.header().blake3_root, p.header().blake3_root);

  fs::path extracted = tmp / "body.out";
  c.extract_body(extracted);
  CHECK_EQ(fs::file_size(extracted), 100000u);
  CHECK_EQ(hash_file(extracted), hash_file(body));
}

// Revision 3 is the only one this build reads: a pack is a media set now, and
// a revision 1 or 2 pack is one game with its own copy of its discs. Those are
// installed again rather than read on a guess.
static void test_container_revision(const fs::path& tmp) {
  section("container revision");
  fs::path out = tmp / "rev.kgpack";
  Meta m;
  m.id = "rev";
  write_pack(out, set_of(m), WriteOptions{PackKind::Game, std::nullopt, false});
  CHECK_EQ(static_cast<int>(Pack::open(out).header().revision), 3);

  // Byte 7 is the revision, straight after the seven-byte magic.
  auto stamp = [&](const fs::path& p, unsigned char rev) {
    fs::copy_file(out, p, fs::copy_options::overwrite_existing);
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(7);
    f.write(reinterpret_cast<const char*>(&rev), 1);
  };
  for (unsigned char old : {1, 2}) {
    fs::path p = tmp / ("rev" + std::to_string(old) + ".kgpack");
    stamp(p, old);
    CHECK_THROWS_WITH(Pack::open(p), "made by an older kretro; install the game again");
  }
  fs::path four = tmp / "rev4.kgpack";
  stamp(four, 4);
  CHECK_THROWS(Pack::open(four));
  fs::path zero = tmp / "rev0.kgpack";
  stamp(zero, 0);
  CHECK_THROWS(Pack::open(zero));
}

static Meta set_game(const std::string& id, std::vector<Meta::Disc> discs) {
  Meta m;
  m.id = id;
  m.name = "The game " + id;
  m.run.exe = "game.exe";
  m.discs = std::move(discs);
  m.tree = Tree::from_canonical("000081a4 0000000000000004 " + to_hex(hash_string(id)) + " game.exe\n");
  return m;
}

static Meta::Disc set_disc(const std::string& key, const std::string& label) {
  Meta::Disc d;
  d.key = key;
  d.label = label;
  d.serial = 7;
  d.ref = "Example.zip#" + label;
  d.bytes = 1000;
  return d;
}

// Two games from one disc, one of them from a second disc too: the disc is
// listed once, each game names its own in drive order, and each game's view
// carries the discs and the body it plays from.
static void test_set_meta() {
  section("set metadata: one disc, two games");
  SetMeta s;
  s.set_id = "s-0123456789abcdef";
  s.discs = {set_disc("aaaaaaaaaaaaaaaa", "DEMO_DISC_1"), set_disc("bbbbbbbbbbbbbbbb", "DEMO_DISC_2")};
  s.games = {set_game("example-game-b", {s.discs[1], s.discs[0]}), set_game("example-game-a", {s.discs[0]})};
  s.body.length = 4096;
  s.body.blake3 = hash_string("body");
  s.body.packing = kBodyPacking;

  SetMeta back = SetMeta::decode(s.encode());
  CHECK_EQ(back.set_id, s.set_id);
  CHECK_EQ(back.discs.size(), size_t{2});
  CHECK_EQ(back.games.size(), size_t{2});
  // Sorted by id on the way out, so the bytes do not depend on install order.
  CHECK_EQ(back.games[0].id, std::string("example-game-a"));
  CHECK_EQ(back.games[1].id, std::string("example-game-b"));
  const Meta* b = back.find("example-game-b");
  CHECK(b != nullptr);
  CHECK_EQ(b->discs.size(), size_t{2});
  CHECK_EQ(b->discs[0].label, std::string("DEMO_DISC_2"));  // drive D:
  CHECK_EQ(b->discs[1].label, std::string("DEMO_DISC_1"));  // drive E:
  CHECK_EQ(b->discs[0].bytes, uint64_t{1000});
  CHECK_EQ(b->body.blake3, hash_string("body"));
  CHECK_EQ(b->body.packing, std::string(kBodyPacking));
  CHECK(back.find("example-game-c") == nullptr);

  // The root is over the games' roots in id order, and so does not depend on
  // the order they were given in.
  SetMeta swapped = s;
  std::swap(swapped.games[0], swapped.games[1]);
  CHECK_EQ(s.root(), swapped.root());
  CHECK(s.root() != s.games[0].tree.root());
}

static void test_set_meta_refusals() {
  section("set metadata: what is refused");
  auto refused = [](const SetMeta& s, const char* why) {
    bool threw = false;
    try {
      SetMeta::decode(s.encode());
    } catch (const std::exception& ex) {
      threw = std::string(ex.what()).find(why) != std::string::npos;
      if (!threw) std::fprintf(stderr, "  said: %s\n", ex.what());
    }
    CHECK(threw);
  };
  SetMeta ok;
  ok.set_id = "s-0123456789abcdef";
  ok.discs = {set_disc("aaaaaaaaaaaaaaaa", "DEMO_DISC")};
  ok.games = {set_game("example-game", {ok.discs[0]})};
  CHECK_EQ(SetMeta::decode(ok.encode()).games.size(), size_t{1});

  SetMeta no_games = ok;
  no_games.games.clear();
  refused(no_games, "no games");

  SetMeta twice = ok;
  twice.games.push_back(twice.games[0]);
  refused(twice, "twice");

  SetMeta unknown = ok;
  unknown.games[0].discs[0].key = "cccccccccccccccc";
  refused(unknown, "cccccccccccccccc");

  SetMeta dup_disc = ok;
  dup_disc.discs.push_back(dup_disc.discs[0]);
  refused(dup_disc, "aaaaaaaaaaaaaaaa");

  SetMeta bad_key = ok;
  bad_key.discs[0].key = "../x";
  bad_key.games[0].discs[0].key = "../x";
  refused(bad_key, "not a name");

  SetMeta bad_id = ok;
  bad_id.set_id = "../x";
  refused(bad_id, "not a name");

  SetMeta bad_game = ok;
  bad_game.games[0].id = "../../x";
  refused(bad_game, "not a name a game can have");
}

static void test_disc_key_and_set_id() {
  section("disc keys and set ids");
  Hash p = hash_string("the first 64 MiB");
  std::string k = disc_key(734003200, p);
  CHECK_EQ(k.size(), size_t{16});
  CHECK_EQ(k, disc_key(734003200, p));
  CHECK(k != disc_key(734003201, p));
  CHECK(k != disc_key(734003200, hash_string("another disc")));
  CHECK(id_is_safe(k));

  std::string a = set_id_for({"bbbbbbbbbbbbbbbb", "aaaaaaaaaaaaaaaa"}, "example-game");
  CHECK_EQ(a.rfind("s-", 0), size_t{0});
  CHECK_EQ(a.size(), size_t{18});
  // The discs decide, in any order, and the game does not.
  CHECK_EQ(a, set_id_for({"aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb"}, "another-game"));
  // With no disc at all, the game decides.
  CHECK(set_id_for({}, "example-game") != set_id_for({}, "another-game"));
  CHECK(id_is_safe(set_id_for({}, "example-game")));

  CHECK_EQ(body_game_dir("example-game").generic_string(), std::string("games/example-game"));
  CHECK_EQ(body_disc_dir("aaaaaaaaaaaaaaaa").generic_string(), std::string("discs/aaaaaaaaaaaaaaaa"));

  Meta m = set_game("example-game", {set_disc("aaaaaaaaaaaaaaaa", "DEMO_DISC")});
  SetMeta s = set_of(m);
  CHECK_EQ(s.set_id, set_id_for({"aaaaaaaaaaaaaaaa"}, "example-game"));
  CHECK_EQ(s.discs.size(), size_t{1});
  CHECK_EQ(s.games.size(), size_t{1});
}

// A pack is several games now: game() finds one by id and names the file when
// it is not there.
static void test_pack_games(const fs::path& tmp) {
  section("a pack of two games");
  SetMeta s;
  s.set_id = "s-0123456789abcdef";
  s.discs = {set_disc("aaaaaaaaaaaaaaaa", "DEMO_DISC")};
  s.games = {set_game("example-game-a", {s.discs[0]}), set_game("example-game-b", {s.discs[0]})};
  fs::path body = tmp / "set.body";
  write_file(body, std::string(9000, 's'));
  fs::path out = tmp / "set.kgpack";
  write_pack(out, s, WriteOptions{PackKind::Game, body, false});
  Pack p = Pack::open(out);
  CHECK_EQ(p.set().set_id, s.set_id);
  CHECK_EQ(p.games().size(), size_t{2});
  CHECK_EQ(p.game("example-game-b").name, std::string("The game example-game-b"));
  CHECK_EQ(p.game("example-game-b").body.length, uint64_t{9000});
  CHECK_THROWS_WITH(p.game("example-game-c"), "example-game-c");
  CHECK_EQ(p.header().blake3_root, s.root());
  CHECK(p.verify().ok);
}

// The two spellings of a kind: the word kgpack create takes after --kind, and
// the name kgpack info prints. They differ for a save export ("save" in, and
// "save-export" out), and that difference is kept, so each kind is checked
// against both words rather than round-tripped through one.
static void test_pack_kind_names() {
  section("pack kind names");
  struct Case {
    PackKind kind;
    const char* word;
    const char* name;
  };
  const Case cases[] = {
      {PackKind::Game, "game", "game"},
      {PackKind::Runtime, "runtime", "runtime"},
      {PackKind::SaveExport, "save", "save-export"},
  };
  for (const Case& c : cases) {
    CHECK(parse_pack_kind(c.word) == c.kind);
    CHECK_EQ(std::string(pack_kind_name(c.kind)), std::string(c.name));
  }
  CHECK(parse_pack_kind("game") == parse_pack_kind(pack_kind_name(PackKind::Game)));
  CHECK(parse_pack_kind("runtime") == parse_pack_kind(pack_kind_name(PackKind::Runtime)));
  CHECK(!parse_pack_kind("save-export").has_value());
  CHECK(!parse_pack_kind("nope").has_value());
  CHECK(!parse_pack_kind("").has_value());
}

// Peak resident memory, in bytes, from the kernel's own high-water mark. The
// point of this test is not that a big pack round-trips - it is that writing it
// never held it. VmHWM is the only measurement that can tell those apart.
//
// This reads /proc/self/status and is therefore Linux-only. So is everything
// else here - the runtime is a DwarFS image and a Wine prefix - but this is the
// one test that depends on the kernel's interface rather than on POSIX, so it
// is worth saying out loud. On a kernel without /proc the reads return 0, both
// samples are 0, and the headroom check passes vacuously rather than failing.
static uint64_t peak_rss() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("VmHWM:", 0) == 0) return std::strtoull(line.c_str() + 6, nullptr, 10) * 1024ull;
  }
  return 0;
}

// A body is a set: its games' trees plus their discs plus the discs' audio,
// and four gigabytes is ordinary;
// read_all appends 64 KiB at a time with no reserve, so its peak occupancy is
// worse than a single copy of it.
static void test_streamed_body(const fs::path& tmp) {
  section("streamed body");
  // 64 MiB. Large enough that holding the body would be unmissable - read_all
  // appends 64 KiB at a time with no reserve, so its peak while assembling 64
  // MiB is a couple of hundred - and small enough that the whole suite stays
  // the handful of seconds it is. Nothing else in make test writes to disk at
  // all; this test is the exception and does not get to be a slow one.
  constexpr uint64_t kBodyBytes = 64ull << 20;
  // The pack is a second copy of the body and the extraction a third, so three
  // times the body is what this needs on whatever /tmp turns out to be.
  // Skipping loudly beats failing on a small tmpfs, which is the convention
  // tests/integration/scan.sh already follows.
  std::error_code ec;
  fs::space_info sp = fs::space(tmp, ec);
  if (ec || sp.available < kBodyBytes * 3) {
    std::fprintf(stderr, "  skipped: %s has less than %llu MB free\n", tmp.c_str(),
                 static_cast<unsigned long long>(kBodyBytes * 3 >> 20));
    return;
  }

  fs::path body = tmp / "big.dwarfs";
  {
    std::ofstream f(body, std::ios::binary);
    std::string chunk(1u << 20, '\0');
    for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<char>(i * 7 + 3);
    for (uint64_t n = 0; n < kBodyBytes; n += chunk.size()) {
      f.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
  }
  CHECK_EQ(fs::file_size(body), kBodyBytes);

  Meta m;
  m.id = "big";
  fs::path out = tmp / "big.kgpack";

  // VmHWM is a high-water mark and never falls, so by the time this test runs
  // the tests before it have already set one and the difference measured here
  // would be zero no matter what write_pack does. Linux will reset the mark to
  // the current RSS on request, and that is the only thing that makes this
  // measurement about this function rather than about test ordering.
  {
    std::ofstream cr("/proc/self/clear_refs");
    if (cr) cr << "5";
  }
  uint64_t before = peak_rss();
  write_pack(out, set_of(m), WriteOptions{PackKind::Game, body, false});
  Pack p = Pack::open(out);
  CHECK(p.verify().ok);
  uint64_t after = peak_rss();

  CHECK_EQ(p.header().body_len, kBodyBytes);
  CHECK_EQ(p.games()[0].body.length, kBodyBytes);
  CHECK_EQ(p.games()[0].body.blake3, hash_file(body));
  CHECK_EQ(p.header().body_off % kBodyAlign, 0u);

  // Sixteen megabytes of growth, against a body of sixty-four. A streaming
  // writer comes nowhere near it - one chunk is a megabyte - and read_all
  // cannot come under it, because it holds the whole body at once. Measured,
  // rather than guessed: read_all grows the mark by 64.03 MiB here, so the
  // line goes at a quarter of the body and has four times the margin in both
  // directions. It was one whole body before, which sounds generous and is
  // not: glibc grows the string in place with mremap instead of doubling, so
  // read_all's peak lands 28 KB under a limit of 64 MiB and the test passed
  // against the very implementation it exists to reject.
  CHECK(after < before + (16ull << 20));

  fs::path back = tmp / "big.out";
  p.extract_body(back);
  CHECK_EQ(fs::file_size(back), kBodyBytes);
  CHECK_EQ(hash_file(back), hash_file(body));

  fs::remove(body, ec);
  fs::remove(out, ec);
  fs::remove(back, ec);
}

// The install and registry blocks must survive a CBOR round trip, or a capsule
// restores files without the registry keys that make the game start, and
// without the Windows path the game was installed to.
static void test_meta_install_blocks(const fs::path& tmp) {
  Meta m2;
  m2.id = "adventure2";
  m2.name = "Adventure II";
  m2.install.install_dir = "C:/Program Files/Adventure II";
  m2.registry.fragment = "REGEDIT4\n\n[HKEY_CURRENT_USER\\Software\\X]\n\"A\"=\"1\"\n";
  Meta::Disc install;
  install.key = "1111111111111111";
  install.label = "INSTALL";
  install.serial = 0x1234u;
  install.ref = "AdventureUSA.zip#INSTALL";
  install.source = "/home/someone/discs/AdventureUSA.zip";
  install.bytes = 650000000;
  Meta::Disc play = install;
  play.key = "2222222222222222";
  play.label = "DEMO_PLAY";
  play.serial = 0x5678u;
  play.ref = "AdventureUSA.zip#DEMO_PLAY";
  play.bytes = 0;
  m2.discs = {install, play};
  m2.recipe.setup_ref = "Setup.exe";
  // What the installer wrote outside the game folder. It sits between the
  // registry block and discs2, which is exactly where a miscounted map arity
  // would show up.
  m2.system.files = 37;
  m2.system.bytes = 6ull * 1024 * 1024;
  m2.input["a"] = "Return";
  m2.input["start"] = "F1";

  // Everything Meta::encode writes after the install map, which is exactly
  // where an arity that drifts stops the decoder dead. The map headers are
  // counted by hand, so a field removed without its count is not a lost field,
  // it is an unopenable pack - and these two are the canaries for it.
  fs::path t = tmp / "meta-tree";
  write_file(t / "GAME.EXE", "MZ");
  m2.body.length = 4096;
  m2.tree = Tree::from_directory(t);

  // Through a set, which is the only way a game is written now; the game's
  // view comes back with its discs and the set's body.
  Meta back = SetMeta::decode(set_of(m2).encode()).games[0];
  CHECK_EQ(back.install.install_dir, std::string("C:/Program Files/Adventure II"));
  CHECK_EQ(back.registry.fragment, m2.registry.fragment);
  CHECK_EQ(back.system.files, 37u);
  CHECK_EQ(back.system.bytes, 6ull * 1024 * 1024);
  CHECK_EQ(back.discs.size(), 2u);
  CHECK_EQ(back.discs[1].label, std::string("DEMO_PLAY"));
  CHECK_EQ(back.discs[1].serial, 0x5678u);
  CHECK_EQ(back.discs[0].ref, std::string("AdventureUSA.zip#INSTALL"));
  // The reference alone only resolves inside iso_dir(); the absolute path is
  // what makes a disc from anywhere else findable again.
  CHECK_EQ(back.discs[0].source, std::string("/home/someone/discs/AdventureUSA.zip"));
  CHECK_EQ(back.discs[1].source, std::string("/home/someone/discs/AdventureUSA.zip"));
  CHECK_EQ(back.discs[0].key, std::string("1111111111111111"));
  CHECK_EQ(back.discs[0].bytes, uint64_t{650000000});
  CHECK_EQ(back.recipe.setup_ref, std::string("Setup.exe"));
  CHECK_EQ(back.input.size(), 2u);
  CHECK_EQ(back.input.at("a"), std::string("Return"));
  CHECK_EQ(back.input.at("start"), std::string("F1"));
  CHECK_EQ(back.body.length, 4096u);
  CHECK_EQ(back.tree.canonical(), m2.tree.canonical());

  // A pack with none of these still decodes, with the defaults.
  Meta plain;
  plain.id = "old";
  Meta pback = SetMeta::decode(set_of(plain).encode()).games[0];
  CHECK_EQ(pback.install.install_dir, std::string(""));
  CHECK_EQ(pback.system.files, 0u);
  CHECK_EQ(pback.discs.size(), 0u);
  CHECK_EQ(pback.input.size(), 0u);

  // A disc whose source was never recorded reads back empty rather than
  // pointing at a path on somebody else's machine.
  Meta nosource;
  nosource.id = "demo-game";
  Meta::Disc d;
  d.key = "3333333333333333";
  d.label = "DEMO_GAME";
  nosource.discs = {d};
  CHECK_EQ(SetMeta::decode(set_of(nosource).encode()).games[0].discs[0].source, std::string(""));
}

// A pack is written over the top of the capsule the machine is already playing.
// Writing straight into that path empties it before the first byte of the
// replacement is down, so a reinstall that dies on the third disc, or a recipe
// import that dies in its rebuild, would take the working game with it.
static void test_write_pack_keeps_what_was_there(const fs::path& tmp) {
  section("write_pack keeps what was there");
  fs::path dest = tmp / "keeper.kgpack";
  fs::path body = tmp / "keeper.dwarfs";
  write_file(body, std::string(9000, 'k'));

  Meta m;
  m.id = "keeper";
  m.name = "The Installed Game";
  write_pack(dest, set_of(m), WriteOptions{PackKind::Game, body, false});
  const uint64_t was_size = fs::file_size(dest);
  const Hash was_hash = hash_file(dest);
  CHECK(Pack::open(dest).verify().ok);

  // Failure one: the body is gone by the time the write is asked for. Nothing
  // is written at all, and the pack that is already there is none of its
  // business.
  Meta m2;
  m2.id = "keeper";
  CHECK_THROWS(write_pack(dest, set_of(m2), WriteOptions{PackKind::Game, tmp / "no-such.dwarfs", false}));
  CHECK_EQ(fs::file_size(dest), was_size);
  CHECK_EQ(to_hex(hash_file(dest)), to_hex(was_hash));

  // Failure two, and the one that matters: a body that is measured at one size
  // and read at another, which is the throw at the very end of the copy. Every
  // byte of the new pack - header, metadata, padding, body - has been written
  // by the time it fires. /proc/self/cmdline is that body without a second
  // thread to shrink a real file underneath us: it stats as zero bytes and
  // reads back the argv this test was started with.
  fs::path shifty("/proc/self/cmdline");
  std::error_code sec;
  if (fs::exists(shifty, sec) && fs::file_size(shifty, sec) == 0 && !sec) {
    CHECK_THROWS(write_pack(dest, set_of(m2), WriteOptions{PackKind::Game, shifty, false}));
    CHECK_EQ(fs::file_size(dest), was_size);
    CHECK_EQ(to_hex(hash_file(dest)), to_hex(was_hash));
    CHECK(Pack::open(dest).verify().ok);
  }

  // And no half-written sibling is left lying beside it.
  int leftovers = 0;
  for (const fs::directory_entry& e : fs::directory_iterator(tmp)) {
    if (e.path().filename().string().find(".partial") != std::string::npos) ++leftovers;
  }
  CHECK_EQ(leftovers, 0);

  // The happy path still lands where it was asked to, over the top of what was
  // there, which is the whole reason the rename exists.
  Meta m3;
  m3.id = "keeper";
  m3.name = "The Reinstalled Game";
  write_pack(dest, set_of(m3), WriteOptions{PackKind::Game, body, false});
  CHECK_EQ(Pack::open(dest).games()[0].name, std::string("The Reinstalled Game"));

  fs::remove(dest, sec);
  fs::remove(body, sec);
}

// Packs are meant to be passed around, so the header is a stranger's arithmetic
// and the reader must not allocate on the strength of it. A meta_len that fits
// inside a large file is not thereby plausible.
static void test_absurd_meta_len_is_refused(const fs::path& tmp) {
  section("absurd meta_len");
  constexpr uint64_t kClaimed = 200ull << 20;

  Header h;
  h.kind = PackKind::Game;
  h.meta_off = kPackHeaderSize;
  h.meta_len = kClaimed;
  fs::path liar = tmp / "greedy.kgpack";
  write_file(liar, h.serialize());
  // Sparse, so the file genuinely is big enough to hold what its header claims
  // without this test writing a quarter of a gigabyte. Without the bound the
  // reader believes the claim, because the only gate it had was the file size.
  std::error_code ec;
  fs::resize_file(liar, kPackHeaderSize + kClaimed, ec);
  if (ec) {
    std::fprintf(stderr, "  skipped: cannot make a sparse file in %s\n", tmp.c_str());
    return;
  }

  {
    std::ofstream cr("/proc/self/clear_refs");
    if (cr) cr << "5";
  }
  uint64_t before = peak_rss();
  bool refused_before_allocating = false;
  try {
    Pack::open(liar);
  } catch (const std::exception& e) {
    refused_before_allocating =
        std::string(e.what()).find("implausible amount of metadata") != std::string::npos;
  }
  uint64_t after = peak_rss();
  CHECK(refused_before_allocating);
  // resize() value-initialises, so a reader that allocates first and checks the
  // 64 MiB limit afterwards commits all two hundred megabytes here.
  CHECK(after < before + (16ull << 20));

  fs::remove(liar, ec);
}

static void test_pack_rejects_damage(const fs::path& tmp) {
  section("kgpack rejects damage");
  fs::path good = tmp / "classic2.capsule.kgpack";

  // A single flipped byte inside the body must be caught.
  fs::path torn = tmp / "torn.kgpack";
  fs::copy_file(good, torn, fs::copy_options::overwrite_existing);
  {
    Pack p = Pack::open(torn);
    std::fstream f(torn, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(p.header().body_off + 5));
    char c = 'Z';
    f.write(&c, 1);
  }
  Pack::Verification v = Pack::open(torn).verify();
  CHECK(!v.ok);
  CHECK(!v.body_matches);
  CHECK(v.root_matches);  // the tree is untouched; only the body rotted

  // Bad magic is not a kgpack at all.
  fs::path junk = tmp / "junk.kgpack";
  write_file(junk, std::string(200, '\0'));
  CHECK_THROWS(Pack::open(junk));

  // A header that points past the end of the file must not be believed.
  fs::path liar = tmp / "liar.kgpack";
  fs::copy_file(good, liar, fs::copy_options::overwrite_existing);
  {
    std::fstream f(liar, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(24);  // meta_len
    uint64_t huge = 1ull << 40;
    f.write(reinterpret_cast<const char*>(&huge), 8);
  }
  CHECK_THROWS(Pack::open(liar));

  // Truncation.
  fs::path cut = tmp / "cut.kgpack";
  fs::copy_file(good, cut, fs::copy_options::overwrite_existing);
  fs::resize_file(cut, 40);
  CHECK_THROWS(Pack::open(cut));
}

// A .kgpack is the one file people are told to send each other, so everything
// in one is a stranger's writing. Two of its strings become paths on the
// machine that opens it, and until this test they were used as written.
static void test_a_pack_names_nothing_outside_its_own_places(const fs::path& tmp) {
  section("a pack from someone else names nothing outside");

  // The id is games/<id>.kgpack, saves/<id> and prefixes/<id>. Everything the
  // wizard's slug produces is a name; nothing with a separator in it is.
  CHECK(id_is_safe("classic2"));
  CHECK(id_is_safe("demo-game-side-story"));
  CHECK(id_is_safe("arena_3.arena"));
  CHECK(!id_is_safe(""));
  CHECK(!id_is_safe("."));
  CHECK(!id_is_safe(".."));
  CHECK(!id_is_safe("../../evil"));
  CHECK(!id_is_safe("games/../../evil"));
  CHECK(!id_is_safe("/etc/passwd"));
  CHECK(!id_is_safe("has space"));
  CHECK(!id_is_safe(".hidden"));
  CHECK(!id_is_safe(std::string(200, 'a')));

  // And the pack carrying one is refused whole, on open, before a directory is
  // named after it. "../../../.config/autostart/x" would have put a stranger's
  // file wherever he liked, on import and again on install and play.
  Meta hostile;
  hostile.id = "../../../.config/autostart/kretro";
  hostile.name = "Classic 2";
  fs::path bad_id = tmp / "hostile-id.kgpack";
  write_pack(bad_id, set_of(hostile), WriteOptions{PackKind::Game, std::nullopt, false});
  CHECK_THROWS(Pack::open(bad_id));
  CHECK_THROWS(SetMeta::decode(set_of(hostile).encode()));

  // install_dir is joined onto the prefix's drive_c, and what is there is
  // removed and replaced with a symlink the first time the game is played. A
  // relative path under C: is what it means; a walk out of C: is a pack that
  // deletes a file in the importer's home.
  CHECK(install_dir_is_safe(""));
  CHECK(install_dir_is_safe("Program Files/Adventure II"));
  CHECK(install_dir_is_safe("C:/Program Files/Adventure II"));
  CHECK(!install_dir_is_safe("/home/someone/.bashrc"));
  CHECK(!install_dir_is_safe("../../../.bashrc"));
  CHECK(!install_dir_is_safe("Games/../../../.bashrc"));
  CHECK(!install_dir_is_safe("..\\..\\..\\.bashrc"));
  CHECK(!install_dir_is_safe("."));

  Meta escaping;
  escaping.id = "adventure2";
  escaping.install.install_dir = "../../../../../../.bashrc";
  fs::path bad_dir = tmp / "hostile-install-dir.kgpack";
  write_pack(bad_dir, set_of(escaping), WriteOptions{PackKind::Game, std::nullopt, false});
  CHECK_THROWS(Pack::open(bad_dir));

  // The ordinary pack this one is a forgery of still opens, with both strings
  // intact: the check is a fence, not a rewrite.
  Meta ok;
  ok.id = "adventure2";
  ok.install.install_dir = "Program Files/Adventure II";
  fs::path fine = tmp / "ordinary.kgpack";
  write_pack(fine, set_of(ok), WriteOptions{PackKind::Game, std::nullopt, false});
  Pack p = Pack::open(fine);
  CHECK_EQ(p.games()[0].id, std::string("adventure2"));
  CHECK_EQ(p.games()[0].install.install_dir, std::string("Program Files/Adventure II"));

  std::error_code ec;
  fs::remove(bad_id, ec);
  fs::remove(bad_dir, ec);
  fs::remove(fine, ec);
}

// The count in front of a CBOR container is the writer's claim and nothing has
// been read to back it. sizeof(cbor::Value) is 112 bytes, so reserving on the
// claim is a hundredfold amplifier: a couple of megabytes of pack would ask
// for a couple of hundred, and get them, before the item budget refused it.
static void test_an_absurd_container_count_allocates_nothing() {
  section("absurd container count");

  auto big_array = [](uint64_t n, size_t filler) {
    std::string s;
    s.push_back(static_cast<char>(0x9a));  // array, uint32 count
    for (int k = 3; k >= 0; --k) s.push_back(static_cast<char>((n >> (8 * k)) & 0xff));
    s.append(filler, '\x01');              // as many one-byte items as it takes
    return s;
  };
  auto big_map = [](uint64_t n, size_t filler) {
    std::string s;
    s.push_back(static_cast<char>(0xba));  // map, uint32 count
    for (int k = 3; k >= 0; --k) s.push_back(static_cast<char>((n >> (8 * k)) & 0xff));
    s.append(filler, '\x01');
    return s;
  };

  // Two million elements declared, and two million bytes behind the claim so
  // that the older "longer than the buffer allows" gate is passed honestly.
  // The budget is a million, so this must be refused - and refused before the
  // 224 MB reserve, which is the whole of the fix.
  const std::string array_bomb = big_array(2000000, 2000000);
  const std::string map_bomb = big_map(700000, 1400000);

  {
    std::ofstream cr("/proc/self/clear_refs");
    if (cr) cr << "5";
  }
  uint64_t before = peak_rss();
  CHECK_THROWS(cbor::decode(array_bomb));
  CHECK_THROWS(cbor::decode(map_bomb));
  uint64_t after = peak_rss();
  // The two payloads are four megabytes between them and the test holds both.
  // A reserve on either count is two hundred megabytes and cannot hide here.
  CHECK(after < before + (32ull << 20));

  // A container whose count is honest still decodes, including one that is
  // larger than the up-front reserve now allows for.
  cbor::Encoder e;
  e.array(4000);
  for (int i = 0; i < 4000; ++i) e.uint_val(static_cast<uint64_t>(i));
  cbor::Value v = cbor::decode(e.data());
  CHECK(v.is_array());
  CHECK_EQ(v.arr.size(), 4000u);
  CHECK_EQ(v.arr[3999].uint_or(), 3999u);
}

// body_off and body_len are two more numbers a stranger wrote, and a check
// that they stay inside the file must not add them together in 64 bits. 4096
// plus 2^64-4096 is zero, and zero is inside every file there has ever been.
static void test_an_overflowing_body_length_is_refused(const fs::path& tmp) {
  section("overflowing body length");
  fs::path body = tmp / "overflow.dwarfs";
  write_file(body, std::string(20000, 'b'));
  fs::path good = tmp / "overflow.kgpack";
  Meta m;
  m.id = "dash3";
  write_pack(good, set_of(m), WriteOptions{PackKind::Game, body, false});
  CHECK(Pack::open(good).verify().ok);

  // Byte 32 is body_off and byte 40 is body_len, both little-endian.
  auto forge = [&](const fs::path& p, size_t off, uint64_t v) {
    fs::copy_file(good, p, fs::copy_options::overwrite_existing);
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(off));
    char raw[8];
    for (int i = 0; i < 8; ++i) raw[i] = static_cast<char>((v >> (8 * i)) & 0xff);
    f.write(raw, 8);
  };

  // A body that claims all of memory, starting where the real one does.
  fs::path wrapping = tmp / "wrapping.kgpack";
  forge(wrapping, 40, ~0ull - Pack::open(good).header().body_off + 1);
  CHECK_THROWS(Pack::open(wrapping));

  // The same trick from the other end: a plausible length at an offset that
  // wraps the sum back around to nothing.
  fs::path far_off = tmp / "faroff.kgpack";
  forge(far_off, 32, ~0ull - 1000ull);
  CHECK_THROWS(Pack::open(far_off));

  // And a length that is merely absurd rather than arithmetic: a terabyte is
  // past anything that will ever be a game, and the header says so on its own.
  fs::path absurd = tmp / "absurd-body.kgpack";
  forge(absurd, 40, 1ull << 50);
  CHECK_THROWS(Pack::open(absurd));

  std::error_code ec;
  fs::remove(wrapping, ec);
  fs::remove(far_off, ec);
  fs::remove(absurd, ec);
  fs::remove(good, ec);
  fs::remove(body, ec);
}

// ---- golden bytes -------------------------------------------------------------
//
// The exact bytes today's encoders write, pinned so that moving the code that
// writes them can be shown to change nothing on disk. Every input is fixed:
// no clock, no path of this machine's, no hash of anything that varies. A
// mismatch here is a format change, and a refactor must never make one.

// 32 consecutive byte values from `start`: fixed, and easy to find in a dump.
static Hash golden_hash(uint8_t start) {
  Hash h{};
  for (size_t i = 0; i < h.size(); ++i) h[i] = static_cast<uint8_t>(start + i);
  return h;
}

static void test_golden_header() {
  section("golden: the pack header");
  Header h;
  h.flags = kHasBody;
  h.kind = PackKind::Game;
  h.meta_off = 96;
  h.meta_len = 0x1234;
  h.body_off = 4096;
  h.body_len = 20000;
  h.blake3_root = golden_hash(0x20);
  const std::string want =
      "4b475041434b00030100010001000000600000000000000034120000000000000010000000000000204e000000000000"
      "00000000000000000000000000000000202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
  CHECK_EQ(kgtest::to_hex(h.serialize()), want);
}

// Every field set, and none to its default, so that a field that stops being
// written, or is written in another place, changes the bytes.
static Meta golden_meta() {
  Meta m;
  m.id = "example-game";
  m.name = "Example Game";
  m.developer = "Example Developer";
  m.publisher = "Example Publisher";
  m.year = 1999;
  m.recipe.method = "installer_exe";
  m.recipe.member = "setup/data.cab";
  m.recipe.subdir = "GAME";
  m.recipe.setup = "SETUP.EXE";
  m.recipe.setup_ref = "example.zip#DEMO_DISC";
  m.recipe.discs = {"example.zip#DEMO_DISC", "example.zip#DEMO_DISC2"};
  m.recipe.verify = {"GAME.EXE"};
  DiscFingerprint fp;
  fp.filename = "example.iso";
  fp.size = 734003200;
  fp.blake3 = golden_hash(0x01);
  fp.volume_id = "DEMO_DISC";
  fp.created = "1999-01-02 03:04:05";
  fp.anchors.push_back(Anchor{"SETUP.EXE", 12345, golden_hash(0x02)});
  m.recipe.fingerprints.push_back(fp);
  m.run.exe = "GAME.EXE";
  m.run.args = "-window";
  m.run.windows_version = "win98";
  m.run.width = 640;
  m.run.height = 480;
  m.runtime.id = "wine-10";
  m.runtime.blake3 = golden_hash(0x03);
  m.runtime.dlloverrides = "ddraw=n,b";
  m.runtime.winetricks = {"d3dx9", "vcrun6"};
  m.runtime.dgvoodoo = true;
  m.present.dar = "16:10";
  m.present.pause_on_blur = false;
  m.install.install_dir = "Program Files/Example Game";
  m.registry.fragment = "REGEDIT4\n\n[HKEY_LOCAL_MACHINE\\Software\\Example]\n\"Path\"=\"C:\\\\Game\"\n";
  m.system.files = 3;
  m.system.bytes = 5000000000ull;
  m.input = {{"start", "Escape"}, {"a", "Return"}};
  Meta::Disc d1;
  d1.key = "0001020304050607";
  d1.label = "DEMO_DISC";
  d1.serial = 0x12345678;
  d1.ref = "example.zip#DEMO_DISC";
  d1.source = "/discs/example.iso";
  d1.bytes = 734003200;
  Meta::Disc d2 = d1;
  d2.key = "0809101112131415";
  d2.label = "DEMO_DISC2";
  d2.serial = 7;
  d2.ref = "example.zip#DEMO_DISC2";
  d2.source = "";
  d2.bytes = 1;
  m.discs = {d1, d2};
  m.tree = Tree::from_canonical("000081a4 0000000000000010 " + to_hex(golden_hash(0x50)) + " GAME.EXE\n" +
                                "000041ed 0000000000000000 " + to_hex(golden_hash(0x60)) + " data\n" +
                                "000081a4 0000000000000400 " + to_hex(golden_hash(0x40)) + " data/level1.dat\n");
  return m;
}

// A set of one game, every field set: the set's own keys around the game's.
static SetMeta golden_set() {
  Meta m = golden_meta();
  SetMeta s;
  s.set_id = "s-0001020304050607";
  s.discs = m.discs;
  s.games = {m};
  s.body.length = 20000;
  s.body.blake3 = golden_hash(0x04);
  return s;
}

static void test_golden_meta() {
  section("golden: set metadata, with and without body.packing");
  SetMeta s = golden_set();
  const std::string plain =
      "a4667365745f696472732d303030313032303330343035303630376664697363733282a6636b65797030303031303230"
      "333034303530363037656c6162656c6944454d4f5f444953436673657269616c1a1234567863726566756578616d706c"
      "652e7a69702344454d4f5f4449534366736f75726365722f64697363732f6578616d706c652e69736f6562797465731a"
      "2bc00000a6636b65797030383039313031313132313331343135656c6162656c6a44454d4f5f44495343326673657269"
      "616c0763726566766578616d706c652e7a69702344454d4f5f444953433266736f757263656065627974657301656761"
      "6d657381ae6269646c6578616d706c652d67616d65646e616d656c4578616d706c652047616d6564796561721907cf63"
      "77686fa269646576656c6f706572714578616d706c6520446576656c6f706572697075626c6973686572714578616d70"
      "6c65205075626c697368657266726563697065a8666d6574686f646d696e7374616c6c65725f657865666d656d626572"
      "6e73657475702f646174612e636162667375626469726447414d456573657475706953455455502e4558456973657475"
      "705f726566756578616d706c652e7a69702344454d4f5f4449534365646973637382756578616d706c652e7a69702344"
      "454d4f5f44495343766578616d706c652e7a69702344454d4f5f444953433266766572696679816847414d452e455845"
      "6c66696e6765727072696e747381a66866696c656e616d656b6578616d706c652e69736f6473697a651a2bc000006662"
      "6c616b653358200102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2069766f6c756d655f69"
      "646944454d4f5f44495343676372656174656473313939392d30312d30322030333a30343a303567616e63686f727381"
      "a364706174686953455455502e4558456473697a6519303966626c616b6533582002030405060708090a0b0c0d0e0f10"
      "1112131415161718191a1b1c1d1e1f20216372756ea5636578656847414d452e4558456461726773672d77696e646f77"
      "6f77696e646f77735f76657273696f6e6577696e3938657769647468190280666865696768741901e06772756e74696d"
      "65a56269646777696e652d313066626c616b65335820030405060708090a0b0c0d0e0f101112131415161718191a1b1c"
      "1d1e1f2021226c646c6c6f76657272696465736964647261773d6e2c626a77696e65747269636b738265643364783966"
      "766372756e36686467766f6f646f6ff56770726573656e74a2636461726531363a31306d70617573655f6f6e5f626c75"
      "72f467696e7374616c6ca16b696e7374616c6c5f646972781a50726f6772616d2046696c65732f4578616d706c652047"
      "616d65687265676973747279a168667261676d656e74784252454745444954340a0a5b484b45595f4c4f43414c5f4d41"
      "4348494e455c536f6674776172655c4578616d706c655d0a2250617468223d22433a5c5c47616d65220a667379737465"
      "6da26566696c6573036562797465731b000000012a05f20065696e707574a261616652657475726e6573746172746645"
      "736361706565646973637382703030303130323033303430353036303770303830393130313131323133313431356474"
      "72656579012f303030303831613420303030303030303030303030303031302035303531353235333534353535363537"
      "353835393561356235633564356535663630363136323633363436353636363736383639366136623663366436653666"
      "2047414d452e4558450a3030303034316564203030303030303030303030303030303020363036313632363336343635"
      "363636373638363936613662366336643665366637303731373237333734373537363737373837393761376237633764"
      "3765376620646174610a3030303038316134203030303030303030303030303034303020343034313432343334343435"
      "343634373438343934613462346334643465346635303531353235333534353535363537353835393561356235633564"
      "3565356620646174612f6c6576656c312e6461740a64626f6479a2666c656e677468194e2066626c616b653358200405"
      "060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20212223";
  CHECK_EQ(kgtest::to_hex(s.encode()), plain);
  s.body.packing = kBodyPacking;
  const std::string packed =
      "a4667365745f696472732d303030313032303330343035303630376664697363733282a6636b65797030303031303230"
      "333034303530363037656c6162656c6944454d4f5f444953436673657269616c1a1234567863726566756578616d706c"
      "652e7a69702344454d4f5f4449534366736f75726365722f64697363732f6578616d706c652e69736f6562797465731a"
      "2bc00000a6636b65797030383039313031313132313331343135656c6162656c6a44454d4f5f44495343326673657269"
      "616c0763726566766578616d706c652e7a69702344454d4f5f444953433266736f757263656065627974657301656761"
      "6d657381ae6269646c6578616d706c652d67616d65646e616d656c4578616d706c652047616d6564796561721907cf63"
      "77686fa269646576656c6f706572714578616d706c6520446576656c6f706572697075626c6973686572714578616d70"
      "6c65205075626c697368657266726563697065a8666d6574686f646d696e7374616c6c65725f657865666d656d626572"
      "6e73657475702f646174612e636162667375626469726447414d456573657475706953455455502e4558456973657475"
      "705f726566756578616d706c652e7a69702344454d4f5f4449534365646973637382756578616d706c652e7a69702344"
      "454d4f5f44495343766578616d706c652e7a69702344454d4f5f444953433266766572696679816847414d452e455845"
      "6c66696e6765727072696e747381a66866696c656e616d656b6578616d706c652e69736f6473697a651a2bc000006662"
      "6c616b653358200102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2069766f6c756d655f69"
      "646944454d4f5f44495343676372656174656473313939392d30312d30322030333a30343a303567616e63686f727381"
      "a364706174686953455455502e4558456473697a6519303966626c616b6533582002030405060708090a0b0c0d0e0f10"
      "1112131415161718191a1b1c1d1e1f20216372756ea5636578656847414d452e4558456461726773672d77696e646f77"
      "6f77696e646f77735f76657273696f6e6577696e3938657769647468190280666865696768741901e06772756e74696d"
      "65a56269646777696e652d313066626c616b65335820030405060708090a0b0c0d0e0f101112131415161718191a1b1c"
      "1d1e1f2021226c646c6c6f76657272696465736964647261773d6e2c626a77696e65747269636b738265643364783966"
      "766372756e36686467766f6f646f6ff56770726573656e74a2636461726531363a31306d70617573655f6f6e5f626c75"
      "72f467696e7374616c6ca16b696e7374616c6c5f646972781a50726f6772616d2046696c65732f4578616d706c652047"
      "616d65687265676973747279a168667261676d656e74784252454745444954340a0a5b484b45595f4c4f43414c5f4d41"
      "4348494e455c536f6674776172655c4578616d706c655d0a2250617468223d22433a5c5c47616d65220a667379737465"
      "6da26566696c6573036562797465731b000000012a05f20065696e707574a261616652657475726e6573746172746645"
      "736361706565646973637382703030303130323033303430353036303770303830393130313131323133313431356474"
      "72656579012f303030303831613420303030303030303030303030303031302035303531353235333534353535363537"
      "353835393561356235633564356535663630363136323633363436353636363736383639366136623663366436653666"
      "2047414d452e4558450a3030303034316564203030303030303030303030303030303020363036313632363336343635"
      "363636373638363936613662366336643665366637303731373237333734373537363737373837393761376237633764"
      "3765376620646174610a3030303038316134203030303030303030303030303034303020343034313432343334343435"
      "343634373438343934613462346334643465346635303531353235333534353535363537353835393561356235633564"
      "3565356620646174612f6c6576656c312e6461740a64626f6479a3666c656e677468194e2066626c616b653358200405"
      "060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20212223677061636b696e676e63617465676f72697a"
      "652c533232";
  CHECK_EQ(kgtest::to_hex(s.encode()), packed);
}

// The input map's keys are read with text_or(), so a key that is not text
// becomes "" rather than refusing the pack or dropping the binding. Old packs
// may depend on that, so it has to survive the code being moved.
static void test_golden_meta_input_key() {
  section("golden: an input key that is not text decodes as \"\"");
  cbor::Encoder e;
  e.map(3);
  e.text("set_id"); e.text("s-0001020304050607");
  e.text("discs2"); e.array(0);
  e.text("games"); e.array(1);
  e.map(2);
  e.text("id"); e.text("example-game");
  e.text("input");
  e.map(2);
  e.uint_val(7); e.text("Return");
  e.text("a"); e.uint_val(5);
  Meta m = SetMeta::decode(e.data()).games[0];
  CHECK_EQ(m.input.size(), size_t{2});
  CHECK_EQ(m.input.count(""), size_t{1});
  CHECK_EQ(m.input.at(""), std::string("Return"));
  CHECK_EQ(m.input.at("a"), std::string(""));
}

static void test_little_endian() {
  section("little-endian and alignment");
  // Least significant byte first at every width, whatever the host's order:
  // the pack header, the table of contents and the trailer are written this way.
  std::string s;
  le::put_u16(s, 0xbeef);
  CHECK_EQ(s, std::string("\xef\xbe", 2));
  CHECK_EQ(le::get_u16(s, 0), uint16_t{0xbeef});
  s.clear();
  le::put_u32(s, 0x01020304u);
  CHECK_EQ(s, std::string("\x04\x03\x02\x01", 4));
  CHECK_EQ(le::get_u32(s, 0), uint32_t{0x01020304u});
  s.clear();
  le::put_u64(s, 0x0102030405060708ull);
  CHECK_EQ(s, std::string("\x08\x07\x06\x05\x04\x03\x02\x01", 8));
  CHECK_EQ(le::get_u64(s, 0), uint64_t{0x0102030405060708ull});
  // Read at an offset, and with every high bit set: a byte above 0x7f must not
  // sign-extend into the bytes above it.
  s = "xyz";
  le::put_u32(s, 0xfedcba98u);
  CHECK_EQ(le::get_u32(s, 3), uint32_t{0xfedcba98u});
  s.clear();
  le::put_u64(s, ~0ull);
  CHECK_EQ(le::get_u64(s, 0), ~0ull);

  static_assert(align_up(4097, 4096) == 8192);
  CHECK_EQ(align_up(0, 4096), uint64_t{0});
  CHECK_EQ(align_up(1, 4096), uint64_t{4096});
  CHECK_EQ(align_up(4096, 4096), uint64_t{4096});
  CHECK_EQ(align_up(4097, 4096), uint64_t{8192});
  CHECK_EQ(align_up(5, 1), uint64_t{5});
}

static void test_toml() {
  section("toml");
  Toml t = Toml::parse(R"(
# a game manifest, near enough
id   = "classic2"
name = "Classic 2"
year = 1998

[source]
iso    = "Classic Collection (USA).iso"
method = "unzip"
verify = ["MAIN.DAT", "ACTORS.DAT", "CLASSIC2.EXE"]
discs  = [
  "disc one.iso",
  "disc two.iso",
]

[run]
exe   = "CLASSIC2.EXE"
width = 640
scale = 2

[wine]
dgvoodoo = false
)");
  CHECK_EQ(t.str("id"), std::string("classic2"));
  CHECK_EQ(t.str("name"), std::string("Classic 2"));
  CHECK_EQ(t.integer("year"), 1998);
  CHECK_EQ(t.str("source.method"), std::string("unzip"));
  CHECK_EQ(t.array("source.verify").size(), 3u);
  CHECK_EQ(t.array("source.verify")[2], std::string("CLASSIC2.EXE"));
  // Multi-line arrays are legal TOML and manifests use them.
  CHECK_EQ(t.array("source.discs").size(), 2u);
  CHECK_EQ(t.array("source.discs")[1], std::string("disc two.iso"));
  CHECK_EQ(t.integer("run.width"), 640);
  CHECK_EQ(t.boolean("wine.dgvoodoo"), false);
  CHECK_EQ(t.str("nothing.here", "fallback"), std::string("fallback"));
  CHECK(!t.has("nothing.here"));

  // A '#' inside a value is part of the value, not a comment.
  Toml h = Toml::parse("name = \"Command # Conquer\"\n");
  CHECK_EQ(h.str("name"), std::string("Command # Conquer"));

  CHECK_THROWS(Toml::parse("this line has no equals sign\n"));
  CHECK_THROWS(Toml::parse("[unclosed\n"));
  CHECK_THROWS(Toml::parse("x = [1, 2\n"));
  CHECK_THROWS(Toml::parse("x = notanumber\n"));
}

int main() {
  fs::path tmp = fs::temp_directory_path() / "kretro-test-pack";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  try {
    test_hash();
    test_cbor();
    test_toml();
    test_little_endian();
    test_tree(tmp);
    test_pack_roundtrip(tmp);
    test_container_revision(tmp);
    test_pack_kind_names();
    test_set_meta();
    test_set_meta_refusals();
    test_disc_key_and_set_id();
    test_pack_games(tmp);
    test_streamed_body(tmp);
    test_meta_install_blocks(tmp);
    test_pack_rejects_damage(tmp);
    test_write_pack_keeps_what_was_there(tmp);
    test_absurd_meta_len_is_refused(tmp);
    test_a_pack_names_nothing_outside_its_own_places(tmp);
    test_an_absurd_container_count_allocates_nothing();
    test_an_overflowing_body_length_is_refused(tmp);
    test_golden_header();
    test_golden_meta();
    test_golden_meta_input_key();
  } catch (const std::exception& e) {
    kgtest::unexpected(e);
  }

  fs::remove_all(tmp);
  return kgtest::finish();
}
