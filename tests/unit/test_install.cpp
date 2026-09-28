// Tier 1 unit tests for the install engine: no Wine, no GPU, no disc, no game.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <type_traits>
#include <unistd.h>

#include "disc/drive.h"
#include "install/body.h"
#include "install/build.h"
#include "install/staging.h"
#include "install/collection.h"
#include "install/install.h"
#include "disc/members.h"
#include "pack/tree.h"
#include "rt/env.h"
#include "install/discs.h"
#include "install/keys.h"
#include "install/set_merge.h"
#include "install/share.h"
#include "session/compositor.h"
#include "session/journal.h"
#include "session/layers.h"
#include "session/lock.h"
#include "session/prefix.h"
#include "session/saves.h"
#include "session/unpack.h"
#include "util/hash.h"
#include "util/paths.h"
#include "wine/registry.h"
#include "wine/system_files.h"
#include "support/check.h"
#include "support/files.h"

namespace fs = std::filesystem;
using namespace kg;

static void test_reg_parse() {
  // Wine's user.reg format: a bracketed key line with a timestamp, then
  // name=value lines. Names are quoted; @ is the default value.
  const char* text =
      "WINE REGISTRY Version 2\n"
      "\n"
      "[Software\\\\Example Publisher\\\\Adventure II] 1234567890\n"
      "#time=1d9\n"
      "\"InstallPath\"=\"C:\\\\Program Files\\\\Adventure II\"\n"
      "\"Resolution\"=dword:00000001\n"
      "@=\"default\"\n"
      "\n"
      "[Software\\\\Wine] 111\n"
      "\"Version\"=\"win98\"\n";
  auto v = wine::parse_reg(text);
  CHECK_EQ(v.size(), 4u);
  CHECK_EQ(v[0].key, std::string("Software\\Example Publisher\\Adventure II"));
  CHECK_EQ(v[0].name, std::string("InstallPath"));
  CHECK_EQ(v[0].type, std::string("sz"));
  CHECK_EQ(v[0].data, std::string("C:\\Program Files\\Adventure II"));
  CHECK_EQ(v[1].name, std::string("Resolution"));
  CHECK_EQ(v[1].type, std::string("dword"));
  CHECK_EQ(v[1].data, std::string("00000001"));
  CHECK_EQ(v[2].name, std::string("@"));
  CHECK_EQ(v[3].key, std::string("Software\\Wine"));
}

static void test_reg_diff() {
  std::vector<wine::RegValue> before = {
      {"Software\\Wine", "Version", "sz", "win98"},
      {"Software\\X", "A", "sz", "1"},
  };
  std::vector<wine::RegValue> after = {
      {"Software\\Wine", "Version", "sz", "winxp"},      // changed
      {"Software\\X", "A", "sz", "1"},                   // unchanged
      {"Software\\Game", "InstallPath", "sz", "C:\\G"},  // added
  };
  auto d = wine::diff_reg(before, after);
  CHECK_EQ(d.size(), 2u);
  // Deterministic order, so a recipe's fragment is byte-stable.
  CHECK_EQ(d[0].key, std::string("Software\\Game"));
  CHECK_EQ(d[1].key, std::string("Software\\Wine"));
  CHECK_EQ(d[1].data, std::string("winxp"));

  // A value that disappeared is not carried into the fragment: an installer
  // deleting something is not state a capsule needs to recreate.
  auto none = wine::diff_reg(after, before);
  CHECK_EQ(none.size(), 1u);
  CHECK_EQ(none[0].data, std::string("win98"));
}

static void test_reg_fragment() {
  std::vector<wine::RegValue> v = {
      {"Software\\Game", "InstallPath", "sz", "C:\\G"},
      {"Software\\Game", "Res", "dword", "00000001"},
      {"Software\\Other", "@", "sz", "d"},
  };
  std::string f = wine::to_reg_fragment(v);
  CHECK(f.rfind("REGEDIT4", 0) == 0);
  CHECK(f.find("[HKEY_CURRENT_USER\\Software\\Game]") != std::string::npos);
  // Backslashes are doubled in .reg data, and each key appears once.
  CHECK(f.find("\"InstallPath\"=\"C:\\\\G\"") != std::string::npos);
  CHECK(f.find("\"Res\"=dword:00000001") != std::string::npos);
  CHECK(f.find("\"@\"") == std::string::npos);      // @ is written bare
  CHECK(f.find("@=\"d\"") != std::string::npos);
  size_t first = f.find("[HKEY_CURRENT_USER\\Software\\Game]");
  CHECK(f.find("[HKEY_CURRENT_USER\\Software\\Game]", first + 1) == std::string::npos);
}

// A prefix keeps its two hives in two files, and which file a value came out of
// is which root it has to go back under. Adventure II reads its InstallPath from
// HKEY_LOCAL_MACHINE; restoring that key to HKEY_CURRENT_USER puts it where the
// game never looks, which is indistinguishable from not restoring it at all.
// keys.h says, in two places, that a serial stays on this machine. The registry
// diff is where it stopped being true: an installer writes the key you typed
// straight into HKLM, and the unfiltered diff carried it into the fragment, the
// body's registry.reg and every recipe exported from the pack.
static void test_the_serial_stays_here() {
  const std::vector<wine::RegValue> before = {
      {"Software\\Wine", "Version", "sz", "winxp"},
  };
  std::vector<wine::RegValue> after = before;
  const char* key = "Software\\Example Publisher\\Adventure II";
  after.push_back({key, "InstallPath", "sz", "C:\\Program Files\\Adventure II"});
  after.push_back({key, "CDKey", "sz", "ABCD-1234-EFGH-5678"});
  after.push_back({key, "Serial Number", "sz", "9F3K2M8Q4T7X"});
  after.push_back({key, "RegistrationNumber", "hex", "00,01,02,03,04,05,06,07"});
  // Everything a narrow test must not take with it.
  after.push_back({key, "Resolution", "dword", "00000001"});
  after.push_back({key, "Key", "sz", "SPACE"});                       // a binding
  after.push_back({key, "KeyFile", "sz", "C:\\Windows\\game.key"});   // a path
  after.push_back({key, "Language", "sz", "English"});

  std::vector<std::string> dropped;
  std::vector<wine::RegValue> kept =
      wine::without_serials(wine::diff_reg(before, after), &dropped);

  CHECK_EQ(dropped.size(), 3u);
  std::string frag = wine::to_reg_fragment(kept);
  CHECK(frag.find("ABCD-1234-EFGH-5678") == std::string::npos);
  CHECK(frag.find("9F3K2M8Q4T7X") == std::string::npos);
  CHECK(frag.find("\"RegistrationNumber\"") == std::string::npos);

  // And what the game actually needs is all still there. Adventure II reads its
  // install directory back out of exactly this key.
  CHECK(frag.find("C:\\\\Program Files\\\\Adventure II") != std::string::npos);
  CHECK(frag.find("\"Resolution\"") != std::string::npos);
  CHECK(frag.find("SPACE") != std::string::npos);
  CHECK(frag.find("game.key") != std::string::npos);
  CHECK(frag.find("English") != std::string::npos);

  // The rule, value by value.
  CHECK(wine::is_serial_value({"K", "CDKey", "sz", "ABCD-1234-EFGH-5678"}));
  CHECK(wine::is_serial_value({"K", "cd key", "sz", "ABCD12345678"}));
  CHECK(wine::is_serial_value({"K", "ProductKey", "sz", "AAAAA BBBBB CCCCC"}));
  CHECK(wine::is_serial_value({"K", "CDKey", "hex", "00,01"}));
  CHECK(!wine::is_serial_value({"K", "CDKey", "sz", ""}));
  CHECK(!wine::is_serial_value({"K", "Serial", "dword", "1a2b3c4d"}));
  CHECK(!wine::is_serial_value({"K", "Publisher", "sz", "Publisher-1996"}));
  CHECK(!wine::is_serial_value({"K", "Key", "sz", "F1"}));
}

static void test_reg_hives(const fs::path& tmp) {
  fs::path prefix = tmp / "hives";
  fs::remove_all(prefix);
  fs::create_directories(prefix);
  std::ofstream(prefix / "system.reg")
      << "WINE REGISTRY Version 2\n\n"
      << "[Software\\\\Example Publisher\\\\Adventure II] 1\n"
      << "\"InstallPath\"=\"C:\\\\Program Files\\\\Adventure II\"\n";
  std::ofstream(prefix / "user.reg") << "WINE REGISTRY Version 2\n\n"
                                     << "[Software\\\\Example Publisher\\\\Adventure II] 1\n"
                                     << "\"Resolution\"=dword:00000001\n";

  std::vector<wine::RegValue> v = wine::snapshot_prefix(prefix);
  CHECK_EQ(v.size(), 2u);
  CHECK_EQ(v[0].name, std::string("InstallPath"));
  CHECK_EQ(v[0].hive, std::string("HKEY_LOCAL_MACHINE"));
  CHECK_EQ(v[1].name, std::string("Resolution"));
  CHECK_EQ(v[1].hive, std::string("HKEY_CURRENT_USER"));

  // The same key path under two hives is two sections, not one.
  std::string f = wine::to_reg_fragment(v);
  CHECK(f.find("[HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\Adventure II]") !=
        std::string::npos);
  CHECK(f.find("[HKEY_CURRENT_USER\\Software\\Example Publisher\\Adventure II]") !=
        std::string::npos);
  CHECK(f.find("HKEY_LOCAL_MACHINE") < f.find("\"InstallPath\""));
  CHECK(f.find("HKEY_CURRENT_USER") < f.find("\"Resolution\""));

  // And two values sharing a name in different hives are two slots, so a diff
  // does not silently cancel one against the other.
  std::vector<wine::RegValue> before = {
      {"Software\\Game", "Path", "sz", "C:\\G", "HKEY_CURRENT_USER"}};
  std::vector<wine::RegValue> after = {
      {"Software\\Game", "Path", "sz", "C:\\G", "HKEY_LOCAL_MACHINE"}};
  CHECK_EQ(wine::diff_reg(before, after).size(), 1u);
}

// Wine wraps a long binary value over several lines, ending each but the last
// with a backslash. Read one line at a time and the backslash becomes data and
// the rest of the bytes vanish - a CD key that is both truncated and, once
// written back out, not importable.
static void test_reg_continuation() {
  const char* text =
      "WINE REGISTRY Version 2\n"
      "\n"
      "[Software\\\\Example Publisher\\\\Adventure II] 1\n"
      "\"CDKey\"=hex:00,01,02,03,\\\n"
      "  04,05,06,07,\\\n"
      "  08,09\n"
      "\"After\"=\"still parsed\"\n";
  auto v = wine::parse_reg(text);
  CHECK_EQ(v.size(), 2u);
  CHECK_EQ(v[0].name, std::string("CDKey"));
  CHECK_EQ(v[0].type, std::string("hex"));
  CHECK_EQ(v[0].data, std::string("00,01,02,03,04,05,06,07,08,09"));
  CHECK_EQ(v[1].name, std::string("After"));

  std::string f = wine::to_reg_fragment(v);
  CHECK(f.find("\"CDKey\"=hex:00,01,02,03,04,05,06,07,08,09\n") != std::string::npos);
  CHECK(f.find("\\\n") == std::string::npos);  // nothing dangling
}

// str(2) and str(7) are Wine's private spelling of REG_EXPAND_SZ and
// REG_MULTI_SZ. regedit's importer has never heard of either, so copying the
// token through leaves the value silently unrestored - and an install path is
// very often an expand-string.
static void test_reg_expand_and_multi() {
  const char* text =
      "WINE REGISTRY Version 2\n"
      "\n"
      "[Software\\\\Game] 1\n"
      "\"Path\"=str(2):\"%ProgramFiles%\\\\Game\"\n"
      "\"Langs\"=str(7):\"en\\0fr\\0\"\n";
  auto v = wine::parse_reg(text);
  CHECK_EQ(v.size(), 2u);
  CHECK_EQ(v[0].name, std::string("Path"));
  CHECK_EQ(v[0].type, std::string("expand_sz"));
  CHECK_EQ(v[0].data, std::string("%ProgramFiles%\\Game"));
  CHECK_EQ(v[1].type, std::string("multi_sz"));
  CHECK_EQ(v[1].data, std::string("en\0fr\0", 6));

  std::string f = wine::to_reg_fragment(v);
  // UTF-16LE, comma-separated, with the terminating NUL Wine dropped put back.
  CHECK(f.find("\"Langs\"=hex(7):65,00,6e,00,00,00,66,00,72,00,00,00,00,00\n") !=
        std::string::npos);
  CHECK(f.find("\"Path\"=hex(2):25,00,50,00,72,00,6f,00,67,00,72,00,61,00,6d,00,46,00,69,00,"
               "6c,00,65,00,73,00,25,00,5c,00,47,00,61,00,6d,00,65,00,00,00\n") !=
        std::string::npos);
  CHECK(f.find("str(2)") == std::string::npos);
  CHECK(f.find("str(7)") == std::string::npos);
}

// attach_cdrom is where play meets a read-only disc tree, so its signature is
// the guarantee: no label, no serial, nothing that could be written. Those two
// files were put inside the pack at build time precisely so that this call has
// nothing to write. Registering the drive needs wine, so what tier one checks
// is that the split exists and that the writing half kept its own name.
static void test_cdrom_split() {
  static_assert(std::is_invocable_r_v<void, decltype(&disc::attach_cdrom), const rt::Env&,
                                      const fs::path&, char, const fs::path&>,
                "attach_cdrom(env, prefix, letter, tree) - and nothing writable");
  static_assert(std::is_invocable_r_v<void, decltype(&disc::write_drive_metadata), const fs::path&,
                                      const std::string&, uint32_t>,
                "write_drive_metadata(drive_dir, label, serial)");
  CHECK(true);
}

// The set layout, and the one property everything else rests on: putting the
// discs and the other games in the body does not move the game. Meta.tree
// covers games/<id>/game only, so the game's Merkle root is the same in every
// set it is packed into, and answers to the same recipe.
static void test_body_layout(const fs::path& tmp) {
  kgtest::section("a set body: two games, two discs, each once");
  fs::path src = tmp / "layout";
  fs::remove_all(src);
  fs::path a = src / "tree-a";
  fs::create_directories(a / "DATA");
  std::ofstream(a / "game.exe") << "MZ game a";
  std::ofstream(a / "DATA" / "data.pak") << "an archive";
  fs::path b = src / "tree-b";
  fs::create_directories(b);
  std::ofstream(b / "game.exe") << "MZ game b";
  fs::path sys_b = src / "system-b";
  fs::create_directories(sys_b / "windows" / "system32");
  std::ofstream(sys_b / "windows" / "system32" / "old.dll") << "a DLL";
  fs::path d1 = src / "drive-d";
  fs::create_directories(d1 / "install");
  std::ofstream(d1 / "install" / "data1.cab") << "a cabinet";
  fs::path d2 = src / "drive-e";
  fs::create_directories(d2);
  std::ofstream(d2 / "cinematics.bik") << "a movie";
  fs::path flac = src / "track02.flac";
  std::ofstream(flac) << "fLaC";
  Tree before_a = Tree::from_directory(a);

  fs::path root = src / "body";
  std::vector<uint64_t> sizes = install::lay_out_set_body(
      root,
      {install::BodyGame{"example-game-a", a, {}, "REGEDIT4\n\n[HKEY_LOCAL_MACHINE\\Software\\X]\n\"A\"=\"1\"\n"},
       install::BodyGame{"example-game-b", b, sys_b, ""}},
      {install::BodyDisc{"aaaaaaaaaaaaaaaa", d1, "DEMO_DISC_1", 0x1a2b3c4du, {}},
       install::BodyDisc{"bbbbbbbbbbbbbbbb", d2, "DEMO_DISC_2", 0x5u, {flac}}});

  CHECK(fs::exists(root / "games" / "example-game-a" / "game" / "game.exe"));
  CHECK(fs::exists(root / "games" / "example-game-a" / "game" / "DATA" / "data.pak"));
  CHECK(fs::exists(root / "games" / "example-game-a" / "registry.reg"));
  CHECK(!fs::exists(root / "games" / "example-game-a" / "system"));
  CHECK(fs::exists(root / "games" / "example-game-b" / "system" / "windows" / "system32" / "old.dll"));
  CHECK(!fs::exists(root / "games" / "example-game-b" / "registry.reg"));
  CHECK(fs::exists(root / "discs" / "aaaaaaaaaaaaaaaa" / "install" / "data1.cab"));
  CHECK(fs::exists(root / "discs" / "bbbbbbbbbbbbbbbb" / "cinematics.bik"));
  CHECK(fs::exists(root / "discs" / "bbbbbbbbbbbbbbbb" / "audio" / "track02.flac"));
  CHECK(!fs::exists(root / "game"));
  CHECK(!fs::exists(root / "registry.reg"));

  // The two files a play-time mount can never write, written here, once, while
  // the tree is still ours.
  std::ifstream lf(root / "discs" / "aaaaaaaaaaaaaaaa" / ".windows-label");
  std::string label;
  std::getline(lf, label);
  CHECK_EQ(label, std::string("DEMO_DISC_1"));
  std::ifstream sf(root / "discs" / "aaaaaaaaaaaaaaaa" / ".windows-serial");
  std::string serial;
  std::getline(sf, serial);
  CHECK_EQ(serial, std::string("1a2b3c4d"));

  // Each disc's size, for "how much room would unpacking take".
  CHECK_EQ(sizes.size(), size_t{2});
  CHECK(sizes[1] >= 7 + 4);  // the movie and the track, at least

  // The claim the whole design rests on: the root still means the installed
  // game and nothing else.
  CHECK_EQ(to_hex(Tree::from_directory(root / "games" / "example-game-a" / "game").root()),
           to_hex(before_a.root()));
  CHECK_EQ(Tree::from_directory(root / "games" / "example-game-a" / "game").size(), before_a.size());
}

// The shelf keeps a set per pack, and each game finds its set through a
// one-line index beside the other things kept per game.
static void test_shelf_index(const fs::path&) {
  kgtest::section("the shelf: a game is found through its index");
  ensure_state_dirs();
  CHECK(fs::is_directory(packs_dir()));
  CHECK(game_pack("example-game").empty());
  write_game_index("example-game", "s-0123456789abcdef");
  CHECK_EQ(game_set("example-game"), std::string("s-0123456789abcdef"));
  CHECK_EQ(game_pack("example-game"), set_pack("s-0123456789abcdef"));
  CHECK_EQ(set_pack("s-0123456789abcdef"), packs_dir() / "s-0123456789abcdef.kgpack");
  CHECK_EQ(game_index("example-game").filename().string(), std::string("example-game.set"));
  write_game_index("example-game-b", "s-0123456789abcdef");
  std::vector<std::string> ids = indexed_games();
  CHECK_EQ(ids.size(), size_t{2});
  CHECK_EQ(ids[0], std::string("example-game"));
  // A hand-edited index cannot point outside packs/.
  kgtest::write_file(game_index("example-game-c"), "../../etc\n");
  CHECK(game_pack("example-game-c").empty());
  fs::remove(game_index("example-game"));
  fs::remove(game_index("example-game-b"));
  fs::remove(game_index("example-game-c"));
}

// Review Focus 2 of the media-set work: the index names a set that does not
// hold the game - a crash between the set and its index, or a set deleted by
// hand. The game reads as not installed, and the shelf lists it once or not
// at all.
static void test_index_points_nowhere(const fs::path& tmp) {
  kgtest::section("an index that names the wrong set reads as not installed");
  Meta other;
  other.id = "example-game-b";
  other.tree = Tree::from_canonical("");
  SetMeta s = set_of(other);
  fs::path body = tmp / "nowhere.body";
  kgtest::write_file(body, std::string(5000, 'n'));
  write_pack(set_pack(s.set_id), s, WriteOptions{PackKind::Game, body, false});
  write_game_index("example-game", s.set_id);
  write_game_index("example-game-b", s.set_id);
  write_game_index("example-game-c", "s-00000000000000ff");  // no such pack
  std::vector<install::InstalledGame> got = install::installed_games();
  CHECK_EQ(got.size(), size_t{1});
  CHECK(!got.empty() && got[0].id == "example-game-b");
  CHECK(!got.empty() && got[0].pack == set_pack(s.set_id));
  fs::remove(game_index("example-game"));
  fs::remove(game_index("example-game-b"));
  fs::remove(game_index("example-game-c"));
  fs::remove(set_pack(s.set_id));
}

// Review Focus 3: a set whose game is already on the shelf in another set is
// refused, naming the set it is in; the shelf is left as it was.
static void test_import_refuses_a_game_in_another_set(const fs::path& tmp) {
  kgtest::section("importing a set whose game is already in another one is refused");
  Meta a;
  a.id = "example-game";
  a.tree = Tree::from_canonical("");
  SetMeta mine = set_of(a);
  fs::path body = tmp / "mine.body";
  kgtest::write_file(body, std::string(5000, 'm'));
  write_pack(set_pack(mine.set_id), mine, WriteOptions{PackKind::Game, body, false});
  write_game_index("example-game", mine.set_id);

  SetMeta theirs = mine;
  theirs.set_id = "s-fedcba9876543210";
  fs::path in = tmp / "theirs.kgpack";
  write_pack(in, theirs, WriteOptions{PackKind::Game, body, false});
  CHECK_THROWS_WITH(install::import_pack(rt::Env{}, in, /*replace=*/true, [](const std::string&) {}),
                    mine.set_id);
  CHECK_EQ(game_set("example-game"), mine.set_id);
  CHECK(!fs::exists(set_pack(theirs.set_id)));

  // The same set again is an import of what is already there: refused unless
  // asked to replace it, and then it is the set that is replaced.
  fs::path again = tmp / "again.kgpack";
  write_pack(again, mine, WriteOptions{PackKind::Game, body, false});
  CHECK_THROWS_WITH(install::import_pack(rt::Env{}, again, /*replace=*/false, [](const std::string&) {}),
                    "already installed");
  install::ImportResult r = install::import_pack(rt::Env{}, again, /*replace=*/true, [](const std::string&) {});
  CHECK(r.had_body);
  CHECK_EQ(r.pack, set_pack(mine.set_id));
  CHECK_EQ(r.ids, std::vector<std::string>{"example-game"});

  // Review I1: two people install different games off one disc, so their sets
  // have one id. A capsule of the other's set would replace this one and take
  // example-game with it: refused, replace or not, naming what would be lost.
  Meta b;
  b.id = "example-game-b";
  b.tree = Tree::from_canonical("");
  SetMeta friends = mine;
  friends.games = {b};
  fs::path theirs_too = tmp / "friends.kgpack";
  write_pack(theirs_too, friends, WriteOptions{PackKind::Game, body, false});
  CHECK_THROWS_WITH(install::import_pack(rt::Env{}, theirs_too, /*replace=*/true, [](const std::string&) {}),
                    "example-game would");
  CHECK_THROWS_WITH(install::import_pack(rt::Env{}, theirs_too, /*replace=*/false, [](const std::string&) {}),
                    mine.set_id);
  CHECK(Pack::open(set_pack(mine.set_id)).set().find("example-game") != nullptr);
  CHECK(game_set("example-game-b").empty());

  // Review I3: the shelf is written by one install or import at a time.
  {
    session::GameLock held = session::lock_path(packs_dir() / ".lock");
    CHECK(!held.busy());
    CHECK_THROWS_WITH(install::import_pack(rt::Env{}, again, /*replace=*/true, [](const std::string&) {}),
                      "another install");
  }
  CHECK(install::import_pack(rt::Env{}, again, /*replace=*/true, [](const std::string&) {}).had_body);
  fs::remove(game_index("example-game"));
  fs::remove(set_pack(mine.set_id));
}

static Meta merge_game(const std::string& id, std::vector<std::string> keys) {
  Meta m;
  m.id = id;
  m.tree = Tree::from_canonical("");
  for (const std::string& k : keys) {
    Meta::Disc d;
    d.key = k;
    d.label = "DEMO_" + k.substr(0, 4);
    m.discs.push_back(d);
  }
  return m;
}

static install::ShelfSet shelf_set(const std::string& id, std::vector<Meta> games) {
  install::ShelfSet s;
  s.path = "/nowhere/" + id + ".kgpack";
  s.meta.set_id = id;
  for (const Meta& g : games) {
    for (const Meta::Disc& d : g.discs) {
      bool have = false;
      for (const Meta::Disc& x : s.meta.discs) have = have || x.key == d.key;
      if (!have) s.meta.discs.push_back(d);
    }
    s.meta.games.push_back(g);
  }
  return s;
}

// Where a new game goes: a set of its own, or the set its discs are already
// in, with any it bridges folded into one.
static void test_plan_merge() {
  kgtest::section("planning where a new game goes");
  const std::string k1 = "1111111111111111", k2 = "2222222222222222", k3 = "3333333333333333";

  // Nothing on the shelf: a set of its own.
  install::MergePlan p = install::plan_merge({}, merge_game("example-game-a", {k1}));
  CHECK(p.fold.empty());
  CHECK_EQ(p.set_id, set_id_for({k1}, "example-game-a"));
  CHECK_EQ(p.meta.set_id, p.set_id);
  CHECK_EQ(p.new_disc_keys, std::vector<std::string>{k1});

  // A second game from the same disc joins that set, and the disc is not read
  // again.
  std::vector<install::ShelfSet> shelf = {shelf_set("s-bbbb", {merge_game("example-game-a", {k1})})};
  p = install::plan_merge(shelf, merge_game("example-game-b", {k1}));
  CHECK_EQ(p.fold, std::vector<size_t>{0});
  CHECK_EQ(p.set_id, std::string("s-bbbb"));
  CHECK_EQ(p.meta.games.size(), size_t{2});
  CHECK_EQ(p.meta.discs.size(), size_t{1});
  CHECK(p.new_disc_keys.empty());

  // A game on that disc and another: the set grows by one disc.
  p = install::plan_merge(shelf, merge_game("example-game-c", {k2, k1}));
  CHECK_EQ(p.meta.discs.size(), size_t{2});
  CHECK_EQ(p.new_disc_keys, std::vector<std::string>{k2});

  // A game that bridges two sets folds both, and keeps the smaller id.
  shelf.push_back(shelf_set("s-aaaa", {merge_game("example-game-d", {k2})}));
  p = install::plan_merge(shelf, merge_game("example-game-e", {k1, k2}));
  CHECK_EQ(p.fold, (std::vector<size_t>{1, 0}));
  CHECK_EQ(p.set_id, std::string("s-aaaa"));
  CHECK_EQ(p.meta.games.size(), size_t{3});
  CHECK_EQ(p.meta.discs.size(), size_t{2});
  CHECK(p.new_disc_keys.empty());

  // A reinstall replaces the game rather than adding it twice.
  p = install::plan_merge(shelf, merge_game("example-game-a", {k1}));
  CHECK_EQ(p.meta.games.size(), size_t{1});
  CHECK_EQ(p.set_id, std::string("s-bbbb"));

  // Review Focus 1 of the media-set work: a game that moves. Reinstalled from
  // another disc, it leaves its old set, which keeps its other games and drops
  // the disc no game uses any more.
  std::vector<install::ShelfSet> two = {
      shelf_set("s-cccc", {merge_game("example-game-a", {k1}), merge_game("example-game-b", {k2})})};
  p = install::plan_merge(two, merge_game("example-game-a", {k3}));
  CHECK_EQ(p.fold, std::vector<size_t>{0});
  CHECK_EQ(p.meta.games.size(), size_t{2});
  bool has_k1 = false;
  for (const Meta::Disc& d : p.meta.discs) has_k1 = has_k1 || d.key == k1;
  CHECK(!has_k1);
  CHECK_EQ(p.meta.discs.size(), size_t{2});
  CHECK_EQ(p.new_disc_keys, std::vector<std::string>{k3});

  // Review C1: a set keeps its first id when a reinstall moves it off the
  // disc it was named after. A new set from that disc must not take the same
  // id - writing it would replace the set that already has it.
  std::vector<install::ShelfSet> renamed = {shelf_set(set_id_for({k1}, "x"), {merge_game("example-game-a", {k2})})};
  install::MergePlan fresh = install::plan_merge(renamed, merge_game("example-game-b", {k1}));
  CHECK(fresh.fold.empty());
  CHECK(fresh.set_id != set_id_for({k1}, "x"));
  CHECK_EQ(fresh.meta.set_id, fresh.set_id);
  CHECK(id_is_safe(fresh.set_id));

  // Review C2: a game left in two sets - a crash between a new set and the
  // removal of the one it folded - comes out once when both are folded again.
  std::vector<install::ShelfSet> twice = {shelf_set("s-dddd", {merge_game("example-game-b", {k1})}),
                                          shelf_set("s-eeee", {merge_game("example-game-b", {k2})})};
  install::MergePlan once = install::plan_merge(twice, merge_game("example-game-f", {k1, k2}));
  CHECK_EQ(once.fold.size(), size_t{2});
  CHECK_EQ(once.meta.games.size(), size_t{2});
  CHECK_EQ(SetMeta::decode(once.meta.encode()).games.size(), size_t{2});

  // Games come out sorted, as the encoder writes them.
  CHECK(std::is_sorted(p.meta.games.begin(), p.meta.games.end(),
                       [](const Meta& a, const Meta& b) { return a.id < b.id; }));
  // And the plan's game is the one given, with its discs as the set has them.
  const Meta* a = p.meta.find("example-game-a");
  CHECK(a != nullptr && a->discs.size() == 1 && a->discs[0].key == k3);
}

// A folded set's games and discs, unpacked, go into the new body as they are:
// the game being installed again is left behind, and so is a disc the set no
// longer needs.
static void test_collect_from_set(const fs::path& tmp) {
  kgtest::section("a folded set's games and discs are carried into the new body");
  fs::path from = tmp / "folded";
  fs::remove_all(from);
  kgtest::write_file(from / "games" / "example-game-a" / "game" / "game.exe", "MZ a");
  kgtest::write_file(from / "games" / "example-game-a" / "registry.reg", "REGEDIT4\n");
  kgtest::write_file(from / "games" / "example-game-b" / "game" / "game.exe", "MZ b");
  kgtest::write_file(from / "games" / "example-game-c" / "game" / "game.exe", "MZ c");
  kgtest::write_file(from / "games" / "example-game-c" / "system" / "windows" / "x.dll", "x");
  kgtest::write_file(from / "discs" / "1111111111111111" / "SETUP.EXE", "MZ");
  kgtest::write_file(from / "discs" / "2222222222222222" / "MOVIE.BIK", "m");
  SetMeta s;
  s.set_id = "s-aaaa";
  Meta::Disc d1, d2;
  d1.key = "1111111111111111";
  d1.label = "DEMO_DISC_1";
  d1.serial = 5;
  d2.key = "2222222222222222";
  s.discs = {d1, d2};
  for (const char* id : {"example-game-a", "example-game-b", "example-game-c"}) {
    Meta m;
    m.id = id;
    if (std::string(id) == "example-game-a") m.registry.fragment = "REGEDIT4\n";
    s.games.push_back(m);
  }
  std::vector<install::BodyGame> games = {install::BodyGame{"example-game-new", tmp / "x", {}, ""}};
  std::vector<install::BodyDisc> discs;
  install::collect_from_set(from, s, "example-game-b", {"1111111111111111"}, games, discs);
  CHECK_EQ(games.size(), size_t{3});
  if (games.size() == 3) {
    CHECK_EQ(games[1].id, std::string("example-game-a"));
    CHECK_EQ(games[1].tree, from / "games" / "example-game-a" / "game");
    CHECK(games[1].system.empty());
    CHECK_EQ(games[1].registry, std::string("REGEDIT4\n"));
    CHECK_EQ(games[2].id, std::string("example-game-c"));
    CHECK_EQ(games[2].system, from / "games" / "example-game-c" / "system");
  }
  CHECK_EQ(discs.size(), size_t{1});
  if (discs.size() == 1) {
    CHECK_EQ(discs[0].key, std::string("1111111111111111"));
    CHECK_EQ(discs[0].tree, from / "discs" / "1111111111111111");
    CHECK_EQ(discs[0].label, std::string("DEMO_DISC_1"));
    CHECK_EQ(discs[0].serial, 5u);
  }
  // A disc already on its way into the body is not taken twice, and neither is
  // a game (review C2: one left in two sets by a crash).
  install::collect_from_set(from, s, "example-game-b", {"1111111111111111"}, games, discs);
  CHECK_EQ(discs.size(), size_t{1});
  CHECK_EQ(games.size(), size_t{3});
}

static std::string test_dwarfs_tool() {
  if (const char* t = std::getenv("KRETRO_DWARFS"); t && *t) return t;
  if (fs::exists("build/dwarfs-universal")) return fs::absolute("build/dwarfs-universal").string();
  return "";
}

// A copy install of `subdir` off a directory disc, the way the headless path
// runs one, into the shelf under KRETRO_STATE.
static install::Result install_copy_from_dir(const fs::path& disc_dir, const std::string& id,
                                             const std::string& subdir) {
  install::Build b(rt::Env{}, install::staging_dir(id), [](const std::string&) {});
  b.adopt_discs({disc::open_directory(disc_dir)});
  b.copy_from_disc(subdir);
  Meta m;
  m.id = id;
  m.name = id;
  m.run.exe = "game.exe";
  m.recipe.method = "copy";
  m.recipe.subdir = subdir;
  m.discs = {install::disc_entry(b.discs()[0], "multi-disc")};
  return b.write(std::move(m));
}

static void test_install_into_a_set(const fs::path& tmp) {
  kgtest::section("a second game from the same disc joins its set, and the disc is stored once");
  const std::string tool = test_dwarfs_tool();
  if (tool.empty()) {
    std::fprintf(stderr, "  skip: no dwarfs tool (build/dwarfs-universal or KRETRO_DWARFS) to pack with\n");
    return;
  }
  const char* had = std::getenv("KRETRO_DWARFS");
  const std::string saved = had ? had : "";
  ::setenv("KRETRO_DWARFS", tool.c_str(), 1);

  fs::path disc_dir = tmp / "multi-disc";
  fs::remove_all(disc_dir);
  kgtest::write_file(disc_dir / "GAMEA" / "game.exe", "MZ a" + std::string(40000, 'a'));
  kgtest::write_file(disc_dir / "GAMEB" / "game.exe", "MZ b" + std::string(40000, 'b'));
  kgtest::write_file(disc_dir / "SHARED" / "movie.bik", std::string(200000, 'm'));
  install::Result ra = install_copy_from_dir(disc_dir, "example-game-a", "GAMEA");
  CHECK_EQ(ra.set_games, std::vector<std::string>{"example-game-a"});
  install::Result rb = install_copy_from_dir(disc_dir, "example-game-b", "GAMEB");
  CHECK_EQ(ra.set_id, rb.set_id);
  CHECK_EQ(rb.set_games, (std::vector<std::string>{"example-game-a", "example-game-b"}));
  CHECK_EQ(game_set("example-game-a"), game_set("example-game-b"));
  CHECK(rb.folded.empty());  // the set was rewritten in place, not folded away
  Pack p = Pack::open(game_pack("example-game-b"));
  CHECK_EQ(p.games().size(), size_t{2});
  CHECK_EQ(p.set().discs.size(), size_t{1});
  CHECK(p.verify().ok);
  CHECK_EQ(p.game("example-game-a").tree.root(), ra.root);
  CHECK_EQ(p.game("example-game-b").tree.root(), rb.root);
  CHECK(p.set().discs[0].bytes >= 280000);
  CHECK_EQ(install::installed_games().size(), size_t{2});

  // Reinstalling a replaces it and keeps b.
  install::Result ra2 = install_copy_from_dir(disc_dir, "example-game-a", "GAMEA");
  Pack again = Pack::open(game_pack("example-game-a"));
  CHECK_EQ(again.games().size(), size_t{2});
  CHECK_EQ(ra2.root, ra.root);
  CHECK(again.verify().ok);

  for (const char* id : {"example-game-a", "example-game-b"}) fs::remove(game_index(id));
  fs::remove(set_pack(ra.set_id));
  if (had) ::setenv("KRETRO_DWARFS", saved.c_str(), 1);
  else ::unsetenv("KRETRO_DWARFS");
}

// There is no pack without its discs: every disc entry is a disc the body
// carries, keyed by what the disc is.
static void test_every_disc_travels() {
  kgtest::section("a pack always carries its discs");
  disc::Disc d;
  d.label = "DEMO_DISC";
  d.source = "Example.zip";
  d.info.size = 1000;
  Meta::Disc e = install::disc_entry(d, "Example.zip#DEMO_DISC");
  CHECK_EQ(e.key, disc_key(1000, d.info.prefix));
  CHECK_EQ(e.label, std::string("DEMO_DISC"));
  CHECK_EQ(e.ref, std::string("Example.zip#DEMO_DISC"));
}

// Review Focus 5 of the media-set work: a mounted CD is opened as a directory,
// and a second game from it must land in the first one's set. That needs the
// same key from every open of the same directory.
static void test_disc_key_is_stable(const fs::path& tmp) {
  kgtest::section("a directory disc has the same key every time it is opened");
  fs::path dir = tmp / "keyed-disc";
  fs::remove_all(dir);
  kgtest::write_file(dir / "SETUP.EXE", "MZ");
  kgtest::write_file(dir / "DATA" / "a.cab", std::string(5000, 'c'));
  disc::Disc one = disc::open_directory(dir);
  disc::Disc two = disc::open_directory(dir);
  CHECK_EQ(disc_key(one.info.size, one.info.prefix), disc_key(two.info.size, two.info.prefix));
  CHECK_EQ(install::disc_entry(one, "x").key, disc_key(one.info.size, one.info.prefix));
  kgtest::write_file(dir / "DATA" / "b.cab", "another file");
  CHECK(install::disc_entry(disc::open_directory(dir), "x").key != install::disc_entry(one, "x").key);
}

// The whole of defect (1): everything the installer wrote, not just the game
// folder. A 1998 setup drops a runtime into C:\windows\system32, registers it,
// and the registry fragment naming it travels - so the file has to travel too
// or the recipient gets registrations pointing at nothing.
static void test_what_the_installer_wrote_outside_the_game(const fs::path& tmp) {
  // The rule, first, with no filesystem in it.
  CHECK(wine::is_outside_the_game("windows/system32/msvcrt.dll", "Program Files/Adventure II"));
  CHECK(wine::is_outside_the_game("windows/system32/MSVCRT.DLL", ""));
  CHECK(!wine::is_outside_the_game("Program Files/Adventure II/Adventure II.exe",
                                   "Program Files/Adventure II"));
  // The same path as the installer spelled it, and as Windows spells it.
  CHECK(!wine::is_outside_the_game("program files\\adventure ii\\data.pak",
                                   "Program Files/Adventure II"));
  CHECK(!wine::is_outside_the_game("Program Files/Adventure II", "Program Files/Adventure II"));
  // A sibling whose name merely starts the same way is not inside it.
  CHECK(wine::is_outside_the_game("Program Files/Adventure II Expansion/x.dll",
                                  "Program Files/Adventure II"));
  // Scratch never travels, whoever left it.
  CHECK(!wine::is_outside_the_game("windows/Temp/_ins0432._mp", "Program Files/Game"));
  CHECK(!wine::is_outside_the_game("users/kunal/Temp/ikernel.exe", "Program Files/Game"));
  CHECK(!wine::is_outside_the_game("users/kunal/Local Settings/Temp/setup.inx",
                                   "Program Files/Game"));
  // But the user's own data directories do.
  CHECK(wine::is_outside_the_game("users/kunal/Application Data/Game/config.ini",
                                  "Program Files/Game"));

  // Then the two ends of the round trip, over a real drive_c.
  fs::path root = tmp / "system-files";
  fs::remove_all(root);
  fs::path drive_c = root / "drive_c";
  fs::create_directories(drive_c / "windows" / "system32");
  fs::create_directories(drive_c / "windows" / "Temp");
  fs::create_directories(drive_c / "Program Files" / "Game");
  std::ofstream(drive_c / "windows" / "system32" / "msvcrt.dll") << "a shared runtime";
  std::ofstream(drive_c / "windows" / "system32" / "comdlg32.ocx") << "a control";
  std::ofstream(drive_c / "windows" / "Temp" / "_ins0432._mp") << "installer scratch";
  std::ofstream(drive_c / "Program Files" / "Game" / "game.exe") << "MZ the game";

  std::vector<std::string> written = {
      "windows/system32/msvcrt.dll", "windows/system32/comdlg32.ocx",
      "windows/Temp/_ins0432._mp",   "Program Files/Game/game.exe",
      "windows/system32/gone.dll",   // deleted between the diff and here
  };
  std::vector<std::string> outside;
  for (const std::string& w : written) {
    if (wine::is_outside_the_game(w, "Program Files/Game")) outside.push_back(w);
  }
  // Three: the two runtime files and the one that was deleted between the diff
  // and now. The rule is about paths and knows nothing about the filesystem.
  CHECK_EQ(outside.size(), 3u);

  fs::path system = root / "system";
  wine::SystemFiles got = wine::gather_system_files(drive_c, outside, system);
  CHECK_EQ(got.files, 2u);
  CHECK_EQ(got.bytes, fs::file_size(drive_c / "windows" / "system32" / "msvcrt.dll") +
                          fs::file_size(drive_c / "windows" / "system32" / "comdlg32.ocx"));
  CHECK(fs::exists(system / "windows" / "system32" / "msvcrt.dll"));
  CHECK(!fs::exists(system / "windows" / "Temp"));
  CHECK(!fs::exists(system / "Program Files"));

  // And the other end: a fresh prefix, the way wineboot leaves one, with its
  // own msvcrt.dll that the installer replaced.
  fs::path prefix_c = root / "prefix" / "drive_c";
  fs::create_directories(prefix_c / "windows" / "system32");
  std::ofstream(prefix_c / "windows" / "system32" / "msvcrt.dll") << "wine's own";
  CHECK_EQ(wine::restore_system_files(system, prefix_c), 2u);
  std::ifstream back(prefix_c / "windows" / "system32" / "msvcrt.dll");
  std::string line;
  std::getline(back, line);
  CHECK_EQ(line, std::string("a shared runtime"));
  CHECK(fs::exists(prefix_c / "windows" / "system32" / "comdlg32.ocx"));

  // Nothing to restore is not a failure: a copy install writes no system files
  // and a revision 1 pack has nowhere to keep them.
  CHECK_EQ(wine::restore_system_files(root / "nosuchtree", prefix_c), 0u);
}

// A game with no disc at all - an installer somebody downloaded - is still a
// whole game in its set: the tree, the registry and the system files.
static void test_a_game_with_no_disc_is_still_whole(const fs::path& tmp) {
  fs::path src = tmp / "no-disc";
  fs::remove_all(src);
  fs::path game = src / "tree";
  fs::create_directories(game);
  std::ofstream(game / "game.exe") << "MZ the game";
  Tree before = Tree::from_directory(game);

  fs::path system = src / "system";
  fs::create_directories(system / "windows" / "system32");
  std::ofstream(system / "windows" / "system32" / "msvcrt.dll") << "a shared runtime";

  fs::path root = src / "body";
  install::lay_out_set_body(root, {install::BodyGame{"example-game", game, system, "REGEDIT4\n"}}, {});

  const fs::path in = root / "games" / "example-game";
  CHECK(fs::exists(in / "game" / "game.exe"));
  CHECK(fs::exists(in / "system" / "windows" / "system32" / "msvcrt.dll"));
  CHECK(fs::exists(in / "registry.reg"));
  CHECK(!fs::exists(root / "discs"));
  // system/ is beside game/, never inside it: the Merkle root still means the
  // identity of the installed game and nothing else.
  CHECK_EQ(to_hex(Tree::from_directory(in / "game").root()), to_hex(before.root()));
  CHECK_EQ(Tree::from_directory(in / "game").size(), before.size());
}

// A mounted CD is a directory, and so is a disc somebody already extracted -
// and "load game CD/DVD" is the headline path, not an edge case. A reader
// below that handed the path to 7z would see it exit 2 on a directory, and the
// wizard's third step would take the throw out of the SDL loop and the program
// with it.
static void test_a_directory_is_a_disc(const fs::path& tmp) {
  rt::Env e;  // no runtime: a directory is read directly, and 7z is never asked
  fs::path disc = tmp / "mounted-cd";
  fs::remove_all(disc);
  fs::create_directories(disc / "pc");
  fs::create_directories(disc / "DATA");
  std::ofstream(disc / "SETUP.EXE") << "MZ the installer";
  std::ofstream(disc / "pc" / "GAME.EXE") << "MZ the game";
  std::ofstream(disc / "DATA" / "data1.cab") << "a cabinet";

  std::vector<std::string> listing = iso::list(e, disc);
  auto has = [&](const std::string& want) {
    return std::find(listing.begin(), listing.end(), want) != listing.end();
  };
  CHECK(has("SETUP.EXE"));
  CHECK(has("pc"));
  CHECK(has("pc/GAME.EXE"));
  CHECK(has("DATA/data1.cab"));
  // Spelled the way 7z spells one, so resolve() answers the same either way.
  CHECK_EQ(iso::resolve(listing, "pc/game.exe"), std::string("pc/GAME.EXE"));
  CHECK_EQ(iso::resolve(listing, "setup.exe"), std::string("SETUP.EXE"));

  // The whole disc, and then one directory of it flattened to the root.
  fs::path whole = tmp / "whole";
  std::string err;
  CHECK(iso::extract_subtree(e, disc, "", whole, &err));
  CHECK(fs::exists(whole / "SETUP.EXE"));
  CHECK(fs::exists(whole / "pc" / "GAME.EXE"));
  CHECK(fs::exists(whole / "DATA" / "data1.cab"));

  fs::path just_pc = tmp / "just-pc";
  CHECK(iso::extract_subtree(e, disc, "pc", just_pc, &err));
  CHECK(fs::exists(just_pc / "GAME.EXE"));
  CHECK(!fs::exists(just_pc / "SETUP.EXE"));

  // A second attempt replaces the first rather than landing on top of it.
  std::ofstream(just_pc / "LEFTOVER.DAT") << "from the wrong answer";
  CHECK(iso::extract_subtree(e, disc, "pc", just_pc, &err));
  CHECK(!fs::exists(just_pc / "LEFTOVER.DAT"));

  // A subtree the disc does not have is a sentence, not a crash.
  err.clear();
  CHECK(!iso::extract_subtree(e, disc, "cdrom", tmp / "nothing", &err));
  CHECK(!err.empty());

  // One member, named as 7z's `e` leaves it: the basename, without the
  // directories it sat in.
  fs::path member = tmp / "member";
  CHECK(iso::extract_member(e, disc, "DATA/data1.cab", member, &err));
  CHECK(fs::exists(member / "data1.cab"));
  err.clear();
  CHECK(!iso::extract_member(e, disc, "DATA/data2.cab", member, &err));
  CHECK(!err.empty());

  Hash h{};
  uint64_t size = 0;
  CHECK(iso::hash_member(e, disc, "DATA/data1.cab", &h, &size));
  CHECK_EQ(size, fs::file_size(disc / "DATA" / "data1.cab"));
  CHECK_EQ(to_hex(h), to_hex(hash_file(disc / "DATA" / "data1.cab")));
  CHECK(!iso::hash_member(e, disc, "DATA/data2.cab", &h, &size));

  // An empty directory is not a disc, and says so rather than reading as one.
  fs::path empty = tmp / "empty-mount";
  fs::create_directories(empty);
  bool threw = false;
  try {
    iso::list(e, empty);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

// The disc that travels in the pack has to be readable on a machine that has
// never seen this one. A directory disc was symlinked into its drive and that
// drive went into the body, so the recipient's CD-ROM was a farm of absolute
// names into the builder's own filesystem.
static void test_a_directory_disc_travels_as_files(const fs::path& tmp) {
  rt::Env e;
  fs::path outside = tmp / "outside";
  fs::create_directories(outside);
  std::ofstream(outside / "movie.bik") << "a movie that lives elsewhere";

  fs::path disc = tmp / "linked-cd";
  fs::remove_all(disc);
  fs::create_directories(disc);
  std::ofstream(disc / "AUTORUN.INF") << "[autorun]";
  std::error_code ec;
  fs::create_symlink(outside / "movie.bik", disc / "MOVIE.BIK", ec);
  CHECK(!ec);

  // Listed by the name the disc gives it, not by where the link points.
  std::vector<std::string> listing = iso::list(e, disc);
  CHECK(std::find(listing.begin(), listing.end(), "MOVIE.BIK") != listing.end());

  fs::path tree = tmp / "drive-d";
  std::string err;
  CHECK(iso::extract_subtree(e, disc, "", tree, &err));
  // Copied through the link, not copied as one.
  CHECK(!fs::is_symlink(tree / "MOVIE.BIK", ec));
  CHECK(fs::is_regular_file(tree / "MOVIE.BIK", ec));

  fs::path game = tmp / "linked-game";
  fs::create_directories(game);
  std::ofstream(game / "game.exe") << "MZ";
  fs::path root = tmp / "linked-body";
  install::lay_out_set_body(root, {install::BodyGame{"example-game", game, {}, ""}},
                            {install::BodyDisc{"1111111111111111", tree, "GAME CD", 0x1u, {}}});

  fs::path in_body = root / "discs" / "1111111111111111" / "MOVIE.BIK";
  CHECK(!fs::is_symlink(in_body, ec));
  // The builder's copy going away is exactly what shipping the pack does.
  fs::remove_all(outside, ec);
  std::ifstream f(in_body);
  std::string text;
  std::getline(f, text);
  CHECK_EQ(text, std::string("a movie that lives elsewhere"));
}

// A fragment is a snapshot of what the installer wrote, and a game writes to
// its own registry keys between launches - video settings are often kept
// there. Importing the fragment on every start would put the day the
// game was installed back over whatever the player has changed since. So it is
// applied once per prefix, and again only when the pack's fragment is genuinely
// a different one, which is what a rebuilt or reimported pack produces.
static void test_registry_marker(const fs::path& tmp) {
  fs::path prefix = tmp / "prefix-marker";
  fs::create_directories(prefix);
  const std::string frag = "REGEDIT4\n\n[HKEY_LOCAL_MACHINE\\Software\\Example]\n\"A\"=\"1\"\n";

  // Nothing applied yet.
  CHECK(!wine::registry_marker_matches(prefix, frag));

  wine::write_registry_marker(prefix, frag);
  CHECK(wine::registry_marker_matches(prefix, frag));

  // A different fragment is a different pack, and gets applied.
  CHECK(!wine::registry_marker_matches(prefix, frag + "\"B\"=\"2\"\n"));

  // The marker holds the hash and nothing else, so it can be read by a person
  // and cannot be confused with a prefix's own files.
  std::ifstream f(prefix / ".kretro-registry");
  std::string line;
  std::getline(f, line);
  CHECK_EQ(line, to_hex(hash_string(frag)));

  // A prefix that does not exist is not a match, and asking is not an error.
  CHECK(!wine::registry_marker_matches(tmp / "no-such-prefix", frag));
}

// The unpacked-game cache, and why an id is not enough to key it on.
//
// Where FUSE is unavailable the body is unpacked once into state/extracted/<id>
// and played from there. A cache gated on a marker file whose contents are the
// literal "1\n" says that something has been unpacked, never what. Then
// reinstalling a game - same id, new bytes - plays the install before it, and a
// pack whose body is laid out the other way round is reached through a path
// that does not exist, which surfaces as "no such exe in
// the installed game" about a pack that is perfectly good.
//
// The stamp says the three things that make one unpacked tree a different tree.
// Unpacking itself needs the DwarFS tool and a real body, so what tier one
// proves is the decision: when the cache is reused and when it is thrown away.
static void test_extraction_stamp(const fs::path& tmp) {
  Header flat_h;
  flat_h.blake3_root = hash_string("the game tree");
  Meta flat;
  flat.body.blake3 = hash_string("the body bytes");

  // A tree unpacked by a build from before media sets was laid out another
  // way, and its stamp said so; it may not be played from now.
  const std::string old_stamp = "rooted " + to_hex(flat.body.blake3) + " " + to_hex(flat_h.blake3_root);
  CHECK(session::extraction_stamp(flat, flat_h) != old_stamp);

  // A rebuild of the same game: same id, same layout, other bytes.
  Meta rebuilt = flat;
  rebuilt.body.blake3 = hash_string("the body bytes, built again");
  CHECK(session::extraction_stamp(flat, flat_h) != session::extraction_stamp(rebuilt, flat_h));

  // And a body that happens to hash the same over a different install.
  Header other_h;
  other_h.blake3_root = hash_string("another game tree");
  CHECK(session::extraction_stamp(flat, flat_h) != session::extraction_stamp(flat, other_h));

  fs::path cache = tmp / "extracted";
  fs::create_directories(cache);
  fs::path stamp = cache / "adventure2.stamp";
  const std::string want = session::extraction_stamp(flat, flat_h);

  // Nothing unpacked yet: unpack.
  CHECK(!session::extraction_stamp_matches(stamp, want));
  session::write_extraction_stamp(stamp, want);
  CHECK(session::extraction_stamp_matches(stamp, want));
  // The same tree is reused, and neither of the others is.
  session::write_extraction_stamp(stamp, old_stamp);
  CHECK(!session::extraction_stamp_matches(stamp, want));
  session::write_extraction_stamp(stamp, want);
  CHECK(!session::extraction_stamp_matches(stamp, session::extraction_stamp(rebuilt, flat_h)));

  // Readable by a person, and one line.
  std::ifstream f(stamp);
  std::string line;
  std::getline(f, line);
  CHECK_EQ(line, want);
  CHECK_EQ(line, "set " + to_hex(flat.body.blake3) + " " + to_hex(flat_h.blake3_root));

  // The stamp is a sibling of the unpacked tree, not a file inside it. For a
  // flat body that tree *is* the game directory, and the game directory is what
  // is diffed against meta.tree when the session ends: a stamp within it is a
  // file the game never wrote, reported as written by every session and given a
  // snapshot generation of its own.
  fs::path tree = cache / "adventure2";
  fs::create_directories(tree);
  std::ofstream(tree / "Adventure II.exe") << "MZ the game";
  Tree before = Tree::from_directory(tree);
  session::write_extraction_stamp(cache / "adventure2.stamp", want);
  CHECK_EQ(to_hex(Tree::from_directory(tree).root()), to_hex(before.root()));
  CHECK_EQ(Tree::from_directory(tree).size(), before.size());

  // Asking about a cache directory that does not exist is not an error.
  CHECK(!session::extraction_stamp_matches(tmp / "no-such-cache" / "x.stamp", want));

  // A set's own cache is kretro's to replace, whatever is in it and with no
  // stamp beside it; a directory of the same shape anywhere else is not.
  const fs::path own = set_extract_dir("s-0123456789abcdef");
  kgtest::write_file(own / "games" / "example-game" / "game" / "game.exe", "MZ");
  CHECK(session::may_unpack_into(own, "s-0123456789abcdef"));
  CHECK(!session::may_unpack_into(own, "s-fedcba9876543210"));
  fs::remove_all(own);
}

// The tile a game gets on the shelf, and who is allowed to write it.
//
// The first stable frame of a session becomes title.png and every frame after
// it leaves the tile alone - a tile that changed every session would not be a
// tile. But the frames an install takes are of InstallShield, not of the game,
// and write-once made one of those dialogs the tile for as long as the game
// stayed installed. So the install's tile is marked as a stand-in and the
// first frame of the first real play takes the tile back.
static void test_title_art_belongs_to_the_game(const fs::path& tmp) {
  fs::path frames = tmp / "journal" / "adventure-ii";
  fs::create_directories(frames);
  fs::path installer = tmp / "shots" / "installshield.png";
  fs::path first_play = tmp / "shots" / "the-game.png";
  fs::path later = tmp / "shots" / "act-two.png";
  fs::create_directories(tmp / "shots");
  std::ofstream(installer) << "a dialog with a Next button";
  std::ofstream(first_play) << "the title screen";
  std::ofstream(later) << "somewhere in act two";

  // The install takes the tile, because there is nothing there.
  CHECK(session::set_title_art(frames, installer, /*provisional=*/true));
  CHECK(fs::exists(frames / "title.png"));
  CHECK(fs::exists(frames / "title.provisional"));
  // And every frame after it in the same install leaves it alone: the tile is
  // the first stable frame, not the last one.
  CHECK(!session::set_title_art(frames, later, /*provisional=*/true));

  // The first real play replaces it and takes the mark away.
  CHECK(session::set_title_art(frames, first_play, /*provisional=*/false));
  CHECK(!fs::exists(frames / "title.provisional"));
  {
    std::ifstream f(frames / "title.png");
    std::string got;
    std::getline(f, got);
    CHECK_EQ(got, std::string("the title screen"));
  }

  // Every play after that leaves the tile where it is, including a second
  // install of the same game: only an absent tile or a stand-in is claimable.
  CHECK(!session::set_title_art(frames, later, /*provisional=*/false));
  CHECK(!session::set_title_art(frames, later, /*provisional=*/true));
  {
    std::ifstream f(frames / "title.png");
    std::string got;
    std::getline(f, got);
    CHECK_EQ(got, std::string("the title screen"));
  }

  // A game played before it was ever installed by this kretro - an imported
  // capsule - claims the tile outright and leaves no mark to be overwritten.
  fs::path fresh = tmp / "journal" / "another-game";
  CHECK(session::set_title_art(fresh, first_play, /*provisional=*/false));
  CHECK(fs::exists(fresh / "title.png"));
  CHECK(!fs::exists(fresh / "title.provisional"));

  // And a frame that is not there is not a tile.
  fs::path empty = tmp / "journal" / "nothing";
  CHECK(!session::set_title_art(empty, tmp / "shots" / "no-such-frame.png", false));
  CHECK(!fs::exists(empty / "title.png"));
}

// One kretro per game.
//
// open_layers starts by unmounting whatever is at this game's mountpoints and
// emptying its overlay workdir, because a session that was killed rather than
// closed leaves all of that behind. Run while a second kretro is playing the
// same game, that sweep pulls the filesystem out from under it. Nothing
// anywhere stopped the second one from starting.
static void test_one_kretro_per_game(const fs::path&) {
  // flock is held by the open file description, so two of these inside one
  // process contend exactly as two processes would.
  session::GameLock first = session::lock_game("adventure-ii");
  CHECK(!first.busy());
  CHECK(fs::exists(session::lock_file("adventure-ii")));

  session::GameLock second = session::lock_game("adventure-ii");
  CHECK(second.busy());

  // A different game shares nothing here: playing another game while Adventure II
  // is being played is not a conflict and must not be reported as one.
  session::GameLock other = session::lock_game("another-game");
  CHECK(!other.busy());

  // And the lock goes when the session does, rather than outliving it and
  // locking a person out of their own game until they reboot.
  first.release();
  session::GameLock again = session::lock_game("adventure-ii");
  CHECK(!again.busy());
  {
    session::GameLock scoped = std::move(again);
    CHECK(!scoped.busy());
    CHECK(session::lock_game("adventure-ii").busy());
  }
  CHECK(!session::lock_game("adventure-ii").busy());

  // The lock lives beside the layers it guards, under saves/<id> with
  // everything else the game's sessions keep.
  CHECK_EQ(session::lock_file("adventure-ii").string(),
           (saves_dir() / "adventure-ii" / "lock").string());
}

// A copy that says whether it copied, and a restore that does not delete the
// only other copy before it knows.
//
// link_tree discarded every error_code it was given and counted every file it
// had tried, so "the snapshot worked" and "the disk filled up halfway" were
// the same answer. restore then deleted the live saves and copied into the
// hole, which is the one caller for whom that difference is everything.
static void test_a_restore_does_not_delete_what_it_cannot_replace(const fs::path& tmp) {
  std::error_code ec;
  fs::path from = tmp / "linking" / "from";
  fs::create_directories(from / "Save" / "Slot1");
  std::ofstream(from / "config.cfg") << "gamma=1.4";
  std::ofstream(from / "Save" / "Slot1" / "player.sav") << "the sword";

  size_t files = 0;
  CHECK(session::link_tree(from, tmp / "linking" / "to", &files));
  CHECK_EQ(files, 2u);
  CHECK(fs::exists(tmp / "linking" / "to" / "config.cfg"));
  CHECK(fs::exists(tmp / "linking" / "to" / "Save" / "Slot1" / "player.sav"));

  // A source that is not there copies nothing and says so, rather than
  // reporting a complete copy of nothing.
  size_t none = 0;
  CHECK(!session::link_tree(tmp / "linking" / "no-such-tree", tmp / "linking" / "empty", &none));
  CHECK_EQ(none, 0u);

  // A destination that cannot exist - its parent is a file - is a failure too.
  std::ofstream(tmp / "linking" / "in-the-way") << "not a directory";
  size_t blocked = 0;
  CHECK(!session::link_tree(from, tmp / "linking" / "in-the-way" / "under", &blocked));
  CHECK_EQ(blocked, 0u);

  // And now the whole of restore, through state_dir().
  const std::string id = "another-game";
  fs::path live = saves_dir() / id / "live";
  fs::create_directories(live);
  std::ofstream(live / "SAVEGAME.0") << "in the cistern";

  // One snapshot of where the game is now, then the game moves on.
  fs::path gen = session::snapshot(id, live);
  CHECK(!gen.empty());
  CHECK_EQ(session::generations(id).size(), 1u);
  // Removed and written rather than truncated in place: a generation is a farm
  // of hardlinks, so writing through the live name would rewrite the very
  // bytes the snapshot is made of. Games that keep their saves write a new
  // file and rename it over the old one, which is this.
  fs::remove(live / "SAVEGAME.0", ec);
  std::ofstream(live / "SAVEGAME.0") << "in atlantis";
  std::ofstream(live / "SAVEGAME.1") << "a second slot";

  // Restoring puts the older state back and keeps the newer one as a
  // generation of its own, so the restore is itself undoable.
  session::restore(id, gen.filename().string());
  CHECK_EQ(session::generations(id).size(), 2u);
  {
    std::ifstream f(live / "SAVEGAME.0");
    std::string got;
    std::getline(f, got);
    CHECK_EQ(got, std::string("in the cistern"));
  }
  // And what the older state did not have is gone from the live layer rather
  // than left lying in it: a restore is a state, not a merge.
  CHECK(!fs::exists(live / "SAVEGAME.1"));
  // Neither the staging directory nor the one it displaces is left behind.
  CHECK(!fs::exists(saves_dir() / id / "restoring"));
  CHECK(!fs::exists(saves_dir() / id / "restoring.old"));

  // A generation that does not exist is refused before anything is touched.
  CHECK_THROWS(session::restore(id, "9999"));
  CHECK(fs::exists(live / "SAVEGAME.0"));

  // The state the game is in when a restore fails is the state it was in. A
  // directory the walk cannot enter is the reachable way to make link_tree
  // fail on purpose; root can enter anything, so it is not asked to.
  if (::geteuid() != 0) {
    fs::path lost = saves_dir() / id / "gen" / "0002" / "unreadable";
    fs::create_directories(lost);
    std::ofstream(lost / "deep.sav") << "a file behind a closed door";
    fs::permissions(lost, fs::perms::none, ec);
    const size_t before = session::generations(id).size();
    CHECK_THROWS(session::restore(id, "0002"));
    fs::permissions(lost, fs::perms::owner_all, ec);
    {
      std::ifstream f(live / "SAVEGAME.0");
      std::string got;
      std::getline(f, got);
      CHECK_EQ(got, std::string("in the cistern"));
    }
    // The safety snapshot it took on the way in is a real generation; what it
    // could not lay out left nothing behind.
    CHECK(session::generations(id).size() >= before);
    CHECK(!fs::exists(saves_dir() / id / "restoring"));
  }
}

// Why prepare_prefix has to be able to say no.
//
// import_pack hands a recipe's whole Meta - fragment included - to install::run,
// which prepares a staging prefix and then diffs that prefix's registry across
// the installer to work out what the installer wrote. If the fragment had been
// imported into the staging prefix first, its keys would be in the *before*
// snapshot as well as the after; diff_reg would find them equal on both sides
// and drop every one of them, and the recipient's rebuilt pack would come out
// with an empty registry. That is the "capsule restores files that will not
// start" bug reinstated, for exactly the person the feature exists for.
//
// A real rebuild needs Wine, so what tier one proves is the mechanism: the
// signature that carries the answer, and the diff that eats the fragment when
// the answer is wrong.
static void test_registry_survives_a_rebuild() {
  // The seventh parameter is the pack's system/ - what the installer wrote to
  // C: outside the game folder - and it is here rather than at the call site
  // for the same reason the flag above is: the only correct moment to put those
  // files back is after wineboot and before the fragment that names them.
  static_assert(
      std::is_invocable_r_v<void, decltype(&session::prepare_prefix), const rt::Env&,
                            const fs::path&, const fs::path&, const Meta&, bool,
                            const std::function<void(const std::string&)>&, const fs::path&>,
      "prepare_prefix(env, prefix, home, meta, apply_registry, say, system_tree)");

  const std::vector<wine::RegValue> installer_wrote = {
      {"Software\\Example Publisher\\Adventure II", "InstallPath", "sz",
       "C:\\Program Files\\Adventure II"},
      {"Software\\Example Publisher\\Adventure II", "Resolution", "dword", "00000001"},
  };
  const std::vector<wine::RegValue> stock = {{"Software\\Wine", "Version", "sz", "winxp"}};

  // What install::run does today, with apply_registry false: the staging prefix
  // is stock before the installer runs and carries the game's keys after.
  std::vector<wine::RegValue> after = stock;
  after.insert(after.end(), installer_wrote.begin(), installer_wrote.end());
  std::string rebuilt = wine::to_reg_fragment(wine::diff_reg(stock, after));
  CHECK(rebuilt.find("\"InstallPath\"") != std::string::npos);
  CHECK(rebuilt.find("\"Resolution\"") != std::string::npos);

  // What it would do if the recipe's fragment had been applied first: the keys
  // are on both sides, so the diff is empty and the fragment comes out blank.
  std::string cancelled = wine::to_reg_fragment(wine::diff_reg(after, after));
  CHECK(cancelled.find("InstallPath") == std::string::npos);
}

static void test_key_vault(const fs::path& tmp) {
  fs::path f = tmp / "keys.txt";
  std::vector<install::StoredKey> v;
  install::put_key(v, "adventure2", "ABCDE-FGHIJ-KLMNO-PQRST", "from the jewel case");
  install::put_key(v, "dash3", "1111-2222-3333", "");
  // Storing the same game twice replaces rather than duplicates.
  install::put_key(v, "adventure2", "ZZZZZ-YYYYY-XXXXX-WWWWW", "the second copy");
  CHECK_EQ(v.size(), 2u);
  CHECK_EQ(install::key_for(v, "adventure2"), std::string("ZZZZZ-YYYYY-XXXXX-WWWWW"));
  CHECK_EQ(install::key_for(v, "nosuchgame"), std::string(""));

  install::save_keys(f, v);
  auto back = install::load_keys(f);
  CHECK_EQ(back.size(), 2u);
  CHECK_EQ(install::key_for(back, "dash3"), std::string("1111-2222-3333"));
  CHECK_EQ(install::key_for(back, "adventure2"), std::string("ZZZZZ-YYYYY-XXXXX-WWWWW"));

  // A note with a tab in it must not corrupt the next field.
  install::put_key(v, "q3", "AAAA", "a\tnote\twith\ttabs");
  install::save_keys(f, v);
  auto back2 = install::load_keys(f);
  CHECK_EQ(install::key_for(back2, "q3"), std::string("AAAA"));

  // Missing file is empty, not an error.
  CHECK_EQ(install::load_keys(tmp / "nope.txt").size(), 0u);
}

static void test_disc_ref_parsing() {
  // "archive#LABEL" names a disc by its volume label - the only stable handle,
  // because listing order is not disc order.
  install::DiscRef a = install::parse_disc_ref("AdventureUSA.zip#DEMO_PLAY");
  CHECK_EQ(a.archive, std::string("AdventureUSA.zip"));
  CHECK_EQ(a.label, std::string("DEMO_PLAY"));
  CHECK_EQ(a.index, 0);

  // "archive#2" names it by position, for sets whose labels are unhelpful.
  install::DiscRef b = install::parse_disc_ref("RacerUSA.zip#2");
  CHECK_EQ(b.index, 2);
  CHECK_EQ(b.label, std::string(""));

  // A bare name is the whole archive, first disc.
  install::DiscRef c = install::parse_disc_ref("Classic Collection (USA).iso");
  CHECK_EQ(c.archive, std::string("Classic Collection (USA).iso"));
  CHECK_EQ(c.index, 0);
  CHECK_EQ(c.label, std::string(""));
}

static void test_disc_pick() {
  std::vector<disc::Disc> discs(3);
  discs[0].label = "INSTALL";
  discs[1].label = "CINEMATICS";
  discs[2].label = "DEMO_PLAY";

  install::DiscRef by_label;
  by_label.label = "demo_play";   // case-insensitive: labels are shouted on disc
  CHECK_EQ(install::pick_disc(discs, by_label), 2);

  install::DiscRef by_index;
  by_index.index = 2;
  CHECK_EQ(install::pick_disc(discs, by_index), 1);   // 1-based

  install::DiscRef none;
  CHECK_EQ(install::pick_disc(discs, none), 0);       // the first disc

  install::DiscRef missing;
  missing.label = "NOSUCH";
  CHECK_EQ(install::pick_disc(discs, missing), -1);

  install::DiscRef past_end;
  past_end.index = 9;
  CHECK_EQ(install::pick_disc(discs, past_end), -1);
}

// A recipe is a file people are told to send each other, and it names the disc
// - and for installer_exe the executable - that the machine opening it will
// open and run. `dir / name` throws `dir` away when `name` is absolute, so
// unchecked, the collection directory would vanish from under the join and a
// shared recipe could name any file on the importer's disk.
static void test_a_recipe_names_no_file_outside_the_collection(const fs::path& tmp) {
  fs::path collection = tmp / "collection";
  fs::create_directories(collection);
  { std::ofstream f(collection / "Classic Collection (USA).iso"); f << "not really an iso"; }
  fs::path outside = tmp / "elsewhere" / "Setup.exe";
  fs::create_directories(outside.parent_path());
  { std::ofstream f(outside); f << "MZ"; }

  setenv("KRETRO_ISO_DIR", collection.c_str(), 1);

  // What the collection holds is still found, by its name and by any case of
  // it: that is what a manifest records and it has not changed.
  CHECK_EQ(install::find_iso("Classic Collection (USA).iso"),
           collection / "Classic Collection (USA).iso");
  CHECK_EQ(install::find_iso("classic collection (usa).iso"),
           collection / "Classic Collection (USA).iso");

  // What it does not hold is not found somewhere else.
  CHECK(install::find_iso(outside.string()).empty());
  CHECK(install::find_iso("/etc/passwd").empty());
  CHECK(install::find_iso("../elsewhere/Setup.exe").empty());
  CHECK(install::find_iso("").empty());

  CHECK(install::is_collection_name("Classic Collection (USA).iso"));
  CHECK(install::is_collection_name("boxed/AdventureUSA.zip"));
  CHECK(!install::is_collection_name("/usr/bin/env"));
  CHECK(!install::is_collection_name("../../usr/bin/env"));
  CHECK(!install::is_collection_name("discs/../../usr/bin/env"));
  CHECK(!install::is_collection_name(""));

  unsetenv("KRETRO_ISO_DIR");
}

// A writable layer from inside a classic snap.
//
// Ubuntu 25.04 and later confine fusermount3 with an AppArmor profile whose
// only unix-socket rules are for its own label and for unconfined peers. When
// kretro is started from inside a snap with classic confinement - VS Code's
// terminal, the case this was found in - every process of ours carries the
// snap's label, so does the socket fuse-overlayfs makes for fusermount3, and
// the kernel closes it on the way into fusermount3's profile: "fusermount3:
// file descriptor 6 is not a socket", no overlay, and every game unpacked.
// The same program run from an anonymous memfd lands in a learning profile of
// the snap's own, where fusermount3 keeps what it is handed.
//
// The stand-in below is fuse-overlayfs under that confinement: it mounts, by
// leaving a file in `merged` as a mount would, only when it runs with no name.
static void test_a_writable_layer_from_inside_a_snap(const fs::path& tmp) {
  fs::path root = tmp / "confined-rt";
  fs::create_directories(root / "lib");
  fs::create_directories(root / "bin");
  fs::create_directories(root / "usr/bin");
  std::error_code ec;
  fs::copy_file("/lib64/ld-linux-x86-64.so.2", root / "lib/ld-linux-x86-64.so.2", ec);
  CHECK(!ec);
  fs::create_symlink("/bin/sh", root / "bin/sh", ec);
  {
    std::ofstream f(root / "usr/bin/fuse-overlayfs");
    f << "#!/bin/sh\n"
         "for a; do m=$a; done\n"
         "case \"$(readlink /proc/$$/exe)\" in\n"
         "  /memfd:*) : > \"$m/mounted\"; exit 0 ;;\n"
         "esac\n"
         "echo \"fusermount3: file descriptor 6 is not a socket, can't send fuse fd\" >&2\n"
         "echo \"fuse-overlayfs: cannot mount: Operation not permitted\" >&2\n"
         "exit 1\n";
  }
  fs::permissions(root / "usr/bin/fuse-overlayfs", fs::perms::owner_all, fs::perm_options::add);

  rt::Env e;
  e.root = root;
  e.library_path = (root / "lib").string();
  fs::path lower = tmp / "confined" / "lower", upper = tmp / "confined" / "upper",
           work = tmp / "confined" / "work", merged = tmp / "confined" / "merged";
  for (const fs::path& p : {lower, upper, work, merged}) fs::create_directories(p);

  CHECK_EQ(session::mount_overlay(e, lower, upper, work, merged), std::string());
  CHECK(fs::exists(merged / "mounted"));

  // And where it cannot mount at all, what fuse-overlayfs said is still what
  // a person is told.
  {
    std::ofstream f(root / "usr/bin/fuse-overlayfs");
    f << "#!/bin/sh\necho 'fuse: device not found' >&2\nexit 1\n";
  }
  fs::remove(merged / "mounted");
  std::string why = session::mount_overlay(e, lower, upper, work, merged);
  CHECK(why.find("fuse: device not found") != std::string::npos);
  CHECK(fs::is_empty(merged));

  // A fuse-overlayfs that says it mounted, and did, but shows nothing in
  // merged, was not refused by fusermount3's profile - that fails with an
  // exit status. Mounting it again from memory would put a second overlay on
  // top of the first, and close_layers unmounts one. So it is run once.
  fs::path calls = tmp / "confined" / "calls";
  fs::remove(calls);
  {
    std::ofstream f(root / "usr/bin/fuse-overlayfs");
    f << "#!/bin/sh\necho run >> '" << calls.string() << "'\nexit 0\n";
  }
  why = session::mount_overlay(e, lower, upper, work, merged);
  CHECK(!why.empty());
  std::ifstream cf(calls);
  int runs = 0;
  for (std::string line; std::getline(cf, line);) ++runs;
  CHECK_EQ(runs, 1);
}

// ---- golden bytes -------------------------------------------------------------
//
// The exact text today's writers put on disk, pinned so that moving the code
// that writes it can be shown to change nothing a machine already holds.
// Every input is fixed: no clock, no hash of anything that varies.

// 32 consecutive byte values from `start`: fixed, and easy to find in a dump.
static Hash golden_hash(uint8_t start) {
  Hash h{};
  for (size_t i = 0; i < h.size(); ++i) h[i] = static_cast<uint8_t>(start + i);
  return h;
}

// Both hives, a default value, an escaped string, a dword, raw hex, and the
// two types that are written as hex(2) and hex(7) UTF-16LE.
static void test_golden_reg_fragment() {
  const std::string hklm(wine::kHiveLocalMachine);
  const std::string hkcu(wine::kHiveCurrentUser);
  const std::string key = "Software\\Example Publisher\\Example Game";
  const std::vector<wine::RegValue> v = {
      {key, "InstallPath", "sz", "C:\\Games\\Example \"Game\"", hklm},
      {key, "@", "sz", "default", hklm},
      {key, "Path", "expand_sz", "%ProgramFiles%\\Example", hklm},
      {key, "Discs", "multi_sz", std::string("D:\0E:", 5), hklm},
      {key, "Resolution", "dword", "00000001", hkcu},
      {key, "Blob", "hex", "01,02,ff", hkcu},
  };
  const std::string want =
      "REGEDIT4\n"
      "\n"
      "[HKEY_CURRENT_USER\\Software\\Example Publisher\\Example Game]\n"
      "\"Blob\"=hex:01,02,ff\n"
      "\"Resolution\"=dword:00000001\n"
      "\n"
      "[HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\Example Game]\n"
      "@=\"default\"\n"
      "\"Discs\"=hex(7):44,00,3a,00,00,00,45,00,3a,00,00,00\n"
      "\"InstallPath\"=\"C:\\\\Games\\\\Example \\\"Game\\\"\"\n"
      "\"Path\"=hex(2):25,00,50,00,72,00,6f,00,67,00,72,00,61,00,6d,00,46,00,69,00,6c,00,65,00,73,00,25,00,"
      "5c,00,45,00,78,00,61,00,6d,00,70,00,6c,00,65,00,00,00\n";
  CHECK_EQ(wine::to_reg_fragment(v), want);
}

static void test_golden_journal() {
  session::Record r;
  r.started = 1700000000;
  r.ended = 1700003600;
  r.runtime_id = "wine-10";
  r.note = "said \"hi\"\n\tand left\x01";
  r.screenshot = "shots/one.png";
  r.files_written = 12;
  r.status = 3;
  r.generation = "gen-0001";
  session::write_record("journal-golden", r);
  const std::string want =
      "{\n"
      "  \"started\": 1700000000,\n"
      "  \"ended\": 1700003600,\n"
      "  \"seconds\": 3600,\n"
      "  \"runtime\": \"wine-10\",\n"
      "  \"files_written\": 12,\n"
      "  \"status\": 3,\n"
      "  \"generation\": \"gen-0001\",\n"
      "  \"screenshot\": \"shots/one.png\",\n"
      "  \"note\": \"said \\\"hi\\\"\\n\\tand left\\u0001\"\n"
      "}\n";
  CHECK_EQ(kgtest::slurp(game_saves_dir("journal-golden") / "journal" / "1700000000.json"), want);
}

static void test_golden_extraction_stamp() {
  Meta m;
  m.body.blake3 = golden_hash(0x11);
  Header h;
  h.blake3_root = golden_hash(0x22);
  CHECK_EQ(session::extraction_stamp(m, h),
           std::string("set 1112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30 "
                       "22232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f4041"));
}

static void test_golden_key_vault(const fs::path& tmp) {
  // A tab or a newline in a field is flattened to a space, or it would be a
  // field or a line of its own.
  const std::vector<install::StoredKey> keys = {{"example-game", "ABCD-EFGH", "from the box"},
                                                {"example-game-2", "1111\t2222", "a\nnote"}};
  const fs::path f = tmp / "golden" / "keys.txt";
  install::save_keys(f, keys);
  const std::string want =
      "# Serials you have entered. This file stays on this machine: a key is\n"
      "# yours, not the game's, and it goes into no pack and over no network.\n"
      "#\n"
      "# game\tkey\tnote\n"
      "example-game\tABCD-EFGH\tfrom the box\n"
      "example-game-2\t1111 2222\ta note\n";
  CHECK_EQ(kgtest::slurp(f), want);
}

static void test_env_get_and_append() {
  // How WINEDLLOVERRIDES grows: the pack's, then a player's own, then the
  // backend's, each after a ';'. A value that is unset or empty is simply set.
  rt::Env e;
  CHECK_EQ(e.get("WINEDLLOVERRIDES"), std::string());
  e.append("WINEDLLOVERRIDES", "ddraw=n", ';');
  CHECK_EQ(e.get("WINEDLLOVERRIDES"), std::string("ddraw=n"));
  e.append("WINEDLLOVERRIDES", "d3d9=n,b", ';');
  CHECK_EQ(e.get("WINEDLLOVERRIDES"), std::string("ddraw=n;d3d9=n,b"));
  CHECK_EQ(e.vars.size(), 1u);
  e.set("PATH", "");
  e.append("PATH", "/bin", ':');
  CHECK_EQ(e.get("PATH"), std::string("/bin"));
  CHECK_EQ(e.vars.size(), 2u);
}

int main() {
  fs::path tmp = fs::temp_directory_path() / "kretro-test-install";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  // The game lock and the saves generations live under state_dir(), and
  // state_dir() caches its answer in a function-local static (util/paths.cpp),
  // so this has to happen before anything at all asks where state lives.
  ::setenv("KRETRO_STATE", (tmp / "state").c_str(), 1);
  ensure_state_dirs();

  try {
    test_reg_parse();
    test_reg_diff();
    test_reg_fragment();
    test_the_serial_stays_here();
    test_reg_hives(tmp);
    test_reg_continuation();
    test_reg_expand_and_multi();
    test_cdrom_split();
    test_body_layout(tmp);
    test_what_the_installer_wrote_outside_the_game(tmp);
    test_a_game_with_no_disc_is_still_whole(tmp);
    test_a_directory_is_a_disc(tmp);
    test_a_directory_disc_travels_as_files(tmp);
    test_disc_key_is_stable(tmp);
    test_every_disc_travels();
    test_shelf_index(tmp);
    test_index_points_nowhere(tmp);
    test_import_refuses_a_game_in_another_set(tmp);
    test_plan_merge();
    test_collect_from_set(tmp);
    test_install_into_a_set(tmp);
    test_registry_marker(tmp);
    test_extraction_stamp(tmp);
    test_title_art_belongs_to_the_game(tmp);
    test_one_kretro_per_game(tmp);
    test_a_restore_does_not_delete_what_it_cannot_replace(tmp);
    test_registry_survives_a_rebuild();
    test_key_vault(tmp);
    test_disc_ref_parsing();
    test_disc_pick();
    test_a_recipe_names_no_file_outside_the_collection(tmp);
    test_a_writable_layer_from_inside_a_snap(tmp);
    test_golden_reg_fragment();
    test_golden_journal();
    test_golden_extraction_stamp();
    test_golden_key_vault(tmp);
    test_env_get_and_append();
  } catch (const std::exception& e) {
    kgtest::unexpected(e);
  }

  fs::remove_all(tmp);
  return kgtest::finish();
}
