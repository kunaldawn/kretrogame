// Tier 1 unit tests: no GPU, no container, no game data.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "pack/kgpack.h"
#include "pack/tree.h"
#include "util/cbor.h"
#include "util/hash.h"
#include "util/toml.h"

namespace fs = std::filesystem;
using namespace kg;

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

#define CHECK_THROWS(expr)                                                \
  do {                                                                    \
    ++checks;                                                             \
    bool threw = false;                                                   \
    try { expr; } catch (const std::exception&) { threw = true; }         \
    if (!threw) {                                                         \
      ++failures;                                                         \
      std::fprintf(stderr, "  FAIL %s:%d  expected a throw from %s\n",    \
                   __FILE__, __LINE__, #expr);                            \
    }                                                                     \
  } while (0)

static void section(const char* name) { std::fprintf(stderr, "%s\n", name); }

static void write_file(const fs::path& p, std::string_view content) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

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
  write_pack(out, m, WriteOptions{Kind::Game, std::nullopt, false});
  Pack p = Pack::open(out);
  CHECK(!p.has_body());
  CHECK_EQ(p.header().kind, Kind::Game);
  CHECK_EQ(p.header().blake3_root, m.tree.root());
  CHECK_EQ(p.meta().id, std::string("classic2"));
  CHECK_EQ(p.meta().name, std::string("Classic 2"));
  CHECK_EQ(p.meta().year, 1998u);
  CHECK_EQ(p.meta().developer, std::string("Example Studios"));
  CHECK_EQ(p.meta().run.exe, std::string("CLASSIC2.EXE"));
  CHECK_EQ(p.meta().run.width, 640u);
  CHECK_EQ(p.meta().runtime.blake3, hash_string("a runtime"));
  CHECK_EQ(p.meta().runtime.winetricks.size(), 2u);
  CHECK_EQ(p.meta().recipe.member, std::string("Data2.zip"));
  CHECK_EQ(p.meta().recipe.fingerprints.size(), 1u);
  CHECK_EQ(p.meta().recipe.fingerprints[0].volume_id, std::string("CLASSIC_COLLECTION"));
  CHECK_EQ(p.meta().recipe.fingerprints[0].anchors.size(), 1u);
  CHECK_EQ(p.meta().recipe.fingerprints[0].anchors[0].hash, hash_string("an anchor"));
  CHECK_EQ(p.meta().tree.canonical(), m.tree.canonical());
  CHECK(p.verify().ok);

  // A capsule pack: same metadata, same root, plus a body.
  fs::path body = tmp / "body.bin";
  write_file(body, std::string(100000, 'x'));
  fs::path cap = tmp / "classic2.capsule.kgpack";
  write_pack(cap, m, WriteOptions{Kind::Game, body, false});
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

// The body layout changed, so the revision changed with it. Revision 1 packs
// were written by every build before this one and there is nothing wrong with
// them - they are flat, and they say so - so the check is a range and not an
// equality. Revision 3 does not exist yet and must still be refused: a reader
// that shrugs at a revision it does not know is a reader that will one day
// mount the wrong bytes.
static void test_container_revision(const fs::path& tmp) {
  section("container revision");
  fs::path out = tmp / "rev.kgpack";
  Meta m;
  m.id = "rev";
  write_pack(out, m, WriteOptions{Kind::Game, std::nullopt, false});

  CHECK_EQ(static_cast<int>(Pack::open(out).header().revision), 2);

  // Byte 7 is the revision, straight after the seven-byte magic.
  auto stamp = [&](const fs::path& p, unsigned char rev) {
    fs::copy_file(out, p, fs::copy_options::overwrite_existing);
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(7);
    f.write(reinterpret_cast<const char*>(&rev), 1);
  };

  fs::path one = tmp / "rev1.kgpack";
  stamp(one, 1);
  CHECK_EQ(static_cast<int>(Pack::open(one).header().revision), 1);

  fs::path three = tmp / "rev3.kgpack";
  stamp(three, 3);
  CHECK_THROWS(Pack::open(three));

  fs::path zero = tmp / "rev0.kgpack";
  stamp(zero, 0);
  CHECK_THROWS(Pack::open(zero));
}

// The layout is a field and not an inference from the revision, because the two
// answer different questions: the revision says what this build is allowed to
// read at all, and the layout says where in the body the game is. A pack
// written before either existed decodes as flat, which is exactly what it is.
static void test_layout() {
  section("body layout");
  Meta m;
  m.id = "adventure2";
  CHECK_EQ(m.layout, std::string("flat"));
  CHECK(!m.rooted());

  m.layout = "rooted";
  CHECK(m.rooted());
  Meta back = Meta::decode(m.encode());
  CHECK_EQ(back.layout, std::string("rooted"));
  CHECK(back.rooted());

  // A pack from before the key existed. Absent means flat, and flat is what
  // those packs are: the body root is the game tree.
  Meta old;
  old.id = "old";
  CHECK_EQ(Meta::decode(old.encode()).layout, std::string("flat"));
  CHECK(!Meta::decode(old.encode()).rooted());
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

// A body used to be a game tree and read_all could hold one. A rooted body is a
// game tree plus its discs plus their audio, and four gigabytes is ordinary;
// read_all appends 64 KiB at a time with no reserve, so peak occupancy was
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
  m.layout = "rooted";
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
  write_pack(out, m, WriteOptions{Kind::Game, body, false});
  Pack p = Pack::open(out);
  CHECK(p.verify().ok);
  uint64_t after = peak_rss();

  CHECK_EQ(p.header().body_len, kBodyBytes);
  CHECK_EQ(p.meta().body.length, kBodyBytes);
  CHECK_EQ(p.meta().body.blake3, hash_file(body));
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
  m2.discs.push_back(Meta::Disc{"INSTALL", 0x1234u, "AdventureUSA.zip#INSTALL",
                               "/home/someone/discs/AdventureUSA.zip", true});
  // A disc that was left out on purpose. Play must not go looking for a
  // discs/2 that nobody wrote, and the game's page must be able to say "needs
  // the original disc" rather than pretending.
  m2.discs.push_back(Meta::Disc{"DEMO_PLAY", 0x5678u, "AdventureUSA.zip#DEMO_PLAY",
                               "/home/someone/discs/AdventureUSA.zip", false});
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

  Meta back = Meta::decode(m2.encode());
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
  CHECK(back.discs[0].embedded);
  CHECK(!back.discs[1].embedded);
  CHECK_EQ(back.recipe.setup_ref, std::string("Setup.exe"));
  CHECK_EQ(back.input.size(), 2u);
  CHECK_EQ(back.input.at("a"), std::string("Return"));
  CHECK_EQ(back.input.at("start"), std::string("F1"));
  CHECK_EQ(back.body.length, 4096u);
  CHECK_EQ(back.tree.canonical(), m2.tree.canonical());

  // A pack with none of these still decodes, with the defaults.
  Meta plain;
  plain.id = "old";
  Meta pback = Meta::decode(plain.encode());
  CHECK_EQ(pback.install.install_dir, std::string(""));
  CHECK_EQ(pback.system.files, 0u);
  CHECK_EQ(pback.discs.size(), 0u);
  CHECK_EQ(pback.input.size(), 0u);

  // A revision 1 pack records discs and embeds none of them, because there was
  // nowhere in the body to put them.
  Meta rev1;
  rev1.id = "demo-game";
  rev1.discs.push_back(Meta::Disc{"DEMO_GAME", 0x99u, "demo.zip#DEMO_GAME"});
  CHECK(!Meta::decode(rev1.encode()).discs[0].embedded);
  // Nor did it record where the disc came from, so the field reads back empty
  // rather than pointing at a path on somebody else's machine.
  CHECK_EQ(Meta::decode(rev1.encode()).discs[0].source, std::string(""));
}

// A pack is written over the top of the capsule the machine is already playing.
// Writing straight into that path empties it before the first byte of the
// replacement is down, so a reinstall that dies on the third disc, or a recipe
// import that dies in its rebuild, used to take the working game with it.
static void test_write_pack_keeps_what_was_there(const fs::path& tmp) {
  section("write_pack keeps what was there");
  fs::path dest = tmp / "keeper.kgpack";
  fs::path body = tmp / "keeper.dwarfs";
  write_file(body, std::string(9000, 'k'));

  Meta m;
  m.id = "keeper";
  m.name = "The Installed Game";
  write_pack(dest, m, WriteOptions{Kind::Game, body, false});
  const uint64_t was_size = fs::file_size(dest);
  const Hash was_hash = hash_file(dest);
  CHECK(Pack::open(dest).verify().ok);

  // Failure one: the body is gone by the time the write is asked for. Nothing
  // is written at all, and the pack that is already there is none of its
  // business.
  Meta m2;
  m2.id = "keeper";
  CHECK_THROWS(write_pack(dest, m2, WriteOptions{Kind::Game, tmp / "no-such.dwarfs", false}));
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
    CHECK_THROWS(write_pack(dest, m2, WriteOptions{Kind::Game, shifty, false}));
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
  write_pack(dest, m3, WriteOptions{Kind::Game, body, false});
  CHECK_EQ(Pack::open(dest).meta().name, std::string("The Reinstalled Game"));

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
  h.kind = Kind::Game;
  h.meta_off = kHeaderSize;
  h.meta_len = kClaimed;
  fs::path liar = tmp / "greedy.kgpack";
  write_file(liar, h.serialize());
  // Sparse, so the file genuinely is big enough to hold what its header claims
  // without this test writing a quarter of a gigabyte. Without the bound the
  // reader believes the claim, because the only gate it had was the file size.
  std::error_code ec;
  fs::resize_file(liar, kHeaderSize + kClaimed, ec);
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
  // file wherever he liked, on import and again on install, play and uninstall.
  Meta hostile;
  hostile.id = "../../../.config/autostart/kretro";
  hostile.name = "Classic 2";
  fs::path bad_id = tmp / "hostile-id.kgpack";
  write_pack(bad_id, hostile, WriteOptions{Kind::Game, std::nullopt, false});
  CHECK_THROWS(Pack::open(bad_id));
  CHECK_THROWS(Meta::decode(hostile.encode()));

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
  write_pack(bad_dir, escaping, WriteOptions{Kind::Game, std::nullopt, false});
  CHECK_THROWS(Pack::open(bad_dir));

  // The ordinary pack this one is a forgery of still opens, with both strings
  // intact: the check is a fence, not a rewrite.
  Meta ok;
  ok.id = "adventure2";
  ok.install.install_dir = "Program Files/Adventure II";
  fs::path fine = tmp / "ordinary.kgpack";
  write_pack(fine, ok, WriteOptions{Kind::Game, std::nullopt, false});
  Pack p = Pack::open(fine);
  CHECK_EQ(p.meta().id, std::string("adventure2"));
  CHECK_EQ(p.meta().install.install_dir, std::string("Program Files/Adventure II"));

  std::error_code ec;
  fs::remove(bad_id, ec);
  fs::remove(bad_dir, ec);
  fs::remove(fine, ec);
}

// The count in front of a CBOR container is the writer's claim and nothing has
// been read to back it. sizeof(cbor::Value) is 112 bytes, so reserving on the
// claim is a hundredfold amplifier: a couple of megabytes of pack used to ask
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

// body_off and body_len are two more numbers a stranger wrote, and the check
// that they stay inside the file used to add them together in 64 bits. 4096
// plus 2^64-4096 is zero, and zero is inside every file there has ever been.
static void test_an_overflowing_body_length_is_refused(const fs::path& tmp) {
  section("overflowing body length");
  fs::path body = tmp / "overflow.dwarfs";
  write_file(body, std::string(20000, 'b'));
  fs::path good = tmp / "overflow.kgpack";
  Meta m;
  m.id = "dash3";
  write_pack(good, m, WriteOptions{Kind::Game, body, false});
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
    test_tree(tmp);
    test_pack_roundtrip(tmp);
    test_container_revision(tmp);
    test_layout();
    test_streamed_body(tmp);
    test_meta_install_blocks(tmp);
    test_pack_rejects_damage(tmp);
    test_write_pack_keeps_what_was_there(tmp);
    test_absurd_meta_len_is_refused(tmp);
    test_a_pack_names_nothing_outside_its_own_places(tmp);
    test_an_absurd_container_count_allocates_nothing();
    test_an_overflowing_body_length_is_refused(tmp);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  FAIL unexpected exception: %s\n", e.what());
    ++failures;
  }

  fs::remove_all(tmp);
  std::fprintf(stderr, "\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
