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
#include "install/install.h"
#include "install/iso.h"
#include "pack/tree.h"
#include "install/discs.h"
#include "install/keys.h"
#include "install/registry.h"
#include "session/session.h"
#include "util/hash.h"
#include "util/paths.h"

namespace fs = std::filesystem;
using namespace kg;

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
  auto v = install::parse_reg(text);
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
  std::vector<install::RegValue> before = {
      {"Software\\Wine", "Version", "sz", "win98"},
      {"Software\\X", "A", "sz", "1"},
  };
  std::vector<install::RegValue> after = {
      {"Software\\Wine", "Version", "sz", "winxp"},      // changed
      {"Software\\X", "A", "sz", "1"},                   // unchanged
      {"Software\\Game", "InstallPath", "sz", "C:\\G"},  // added
  };
  auto d = install::diff_reg(before, after);
  CHECK_EQ(d.size(), 2u);
  // Deterministic order, so a recipe's fragment is byte-stable.
  CHECK_EQ(d[0].key, std::string("Software\\Game"));
  CHECK_EQ(d[1].key, std::string("Software\\Wine"));
  CHECK_EQ(d[1].data, std::string("winxp"));

  // A value that disappeared is not carried into the fragment: an installer
  // deleting something is not state a capsule needs to recreate.
  auto none = install::diff_reg(after, before);
  CHECK_EQ(none.size(), 1u);
  CHECK_EQ(none[0].data, std::string("win98"));
}

static void test_reg_fragment() {
  std::vector<install::RegValue> v = {
      {"Software\\Game", "InstallPath", "sz", "C:\\G"},
      {"Software\\Game", "Res", "dword", "00000001"},
      {"Software\\Other", "@", "sz", "d"},
  };
  std::string f = install::to_reg_fragment(v);
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
  const std::vector<install::RegValue> before = {
      {"Software\\Wine", "Version", "sz", "winxp"},
  };
  std::vector<install::RegValue> after = before;
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
  std::vector<install::RegValue> kept =
      install::without_serials(install::diff_reg(before, after), &dropped);

  CHECK_EQ(dropped.size(), 3u);
  std::string frag = install::to_reg_fragment(kept);
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
  CHECK(install::is_serial_value({"K", "CDKey", "sz", "ABCD-1234-EFGH-5678"}));
  CHECK(install::is_serial_value({"K", "cd key", "sz", "ABCD12345678"}));
  CHECK(install::is_serial_value({"K", "ProductKey", "sz", "AAAAA BBBBB CCCCC"}));
  CHECK(install::is_serial_value({"K", "CDKey", "hex", "00,01"}));
  CHECK(!install::is_serial_value({"K", "CDKey", "sz", ""}));
  CHECK(!install::is_serial_value({"K", "Serial", "dword", "1a2b3c4d"}));
  CHECK(!install::is_serial_value({"K", "Publisher", "sz", "Publisher-1996"}));
  CHECK(!install::is_serial_value({"K", "Key", "sz", "F1"}));
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

  std::vector<install::RegValue> v = install::snapshot_prefix(prefix);
  CHECK_EQ(v.size(), 2u);
  CHECK_EQ(v[0].name, std::string("InstallPath"));
  CHECK_EQ(v[0].hive, std::string("HKEY_LOCAL_MACHINE"));
  CHECK_EQ(v[1].name, std::string("Resolution"));
  CHECK_EQ(v[1].hive, std::string("HKEY_CURRENT_USER"));

  // The same key path under two hives is two sections, not one.
  std::string f = install::to_reg_fragment(v);
  CHECK(f.find("[HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\Adventure II]") !=
        std::string::npos);
  CHECK(f.find("[HKEY_CURRENT_USER\\Software\\Example Publisher\\Adventure II]") !=
        std::string::npos);
  CHECK(f.find("HKEY_LOCAL_MACHINE") < f.find("\"InstallPath\""));
  CHECK(f.find("HKEY_CURRENT_USER") < f.find("\"Resolution\""));

  // And two values sharing a name in different hives are two slots, so a diff
  // does not silently cancel one against the other.
  std::vector<install::RegValue> before = {
      {"Software\\Game", "Path", "sz", "C:\\G", "HKEY_CURRENT_USER"}};
  std::vector<install::RegValue> after = {
      {"Software\\Game", "Path", "sz", "C:\\G", "HKEY_LOCAL_MACHINE"}};
  CHECK_EQ(install::diff_reg(before, after).size(), 1u);
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
  auto v = install::parse_reg(text);
  CHECK_EQ(v.size(), 2u);
  CHECK_EQ(v[0].name, std::string("CDKey"));
  CHECK_EQ(v[0].type, std::string("hex"));
  CHECK_EQ(v[0].data, std::string("00,01,02,03,04,05,06,07,08,09"));
  CHECK_EQ(v[1].name, std::string("After"));

  std::string f = install::to_reg_fragment(v);
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
  auto v = install::parse_reg(text);
  CHECK_EQ(v.size(), 2u);
  CHECK_EQ(v[0].name, std::string("Path"));
  CHECK_EQ(v[0].type, std::string("expand_sz"));
  CHECK_EQ(v[0].data, std::string("%ProgramFiles%\\Game"));
  CHECK_EQ(v[1].type, std::string("multi_sz"));
  CHECK_EQ(v[1].data, std::string("en\0fr\0", 6));

  std::string f = install::to_reg_fragment(v);
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

// The rooted layout, and the one property everything else rests on: putting the
// discs in the body does not move the game. Meta.tree covers game/ only, so the
// Merkle root of a pack built with its discs is the root of the same pack built
// without them, and either answers to the same recipe.
static void test_body_layout(const fs::path& tmp) {
  fs::path src = tmp / "layout";
  fs::remove_all(src);
  fs::path game = src / "tree";
  fs::create_directories(game / "DATA");
  std::ofstream(game / "Adventure II.exe") << "MZ the game";
  std::ofstream(game / "DATA" / "data.pak") << "an archive";

  fs::path d1 = src / "drive-d";
  fs::create_directories(d1 / "install");
  std::ofstream(d1 / "install" / "data1.cab") << "a cabinet";
  fs::path d2 = src / "drive-e";
  fs::create_directories(d2);
  std::ofstream(d2 / "cinematics.bik") << "a movie";
  fs::path flac = src / "track02.flac";
  std::ofstream(flac) << "fLaC";

  Tree before = Tree::from_directory(game);

  std::vector<install::BodyDisc> discs = {
      install::BodyDisc{d1, "INSTALL", 0x1a2b3c4du, {}},
      install::BodyDisc{d2, "CINEMATICS", 0x5u, {flac}},
  };
  fs::path root = src / "body";
  install::lay_out_body(root, game, discs,
                        "REGEDIT4\n\n[HKEY_LOCAL_MACHINE\\Software\\X]\n\"A\"=\"1\"\n");

  CHECK(fs::exists(root / "game" / "Adventure II.exe"));
  CHECK(fs::exists(root / "game" / "DATA" / "data.pak"));
  CHECK(fs::exists(root / "discs" / "1" / "install" / "data1.cab"));
  CHECK(fs::exists(root / "discs" / "2" / "cinematics.bik"));
  CHECK(fs::exists(root / "discs" / "2" / "audio" / "track02.flac"));
  CHECK(fs::exists(root / "registry.reg"));

  // The two files a play-time mount can never write, written here, once, while
  // the tree is still ours.
  std::ifstream lf(root / "discs" / "1" / ".windows-label");
  std::string label;
  std::getline(lf, label);
  CHECK_EQ(label, std::string("INSTALL"));
  std::ifstream sf(root / "discs" / "1" / ".windows-serial");
  std::string serial;
  std::getline(sf, serial);
  CHECK_EQ(serial, std::string("1a2b3c4d"));

  // The claim the whole design rests on.
  CHECK_EQ(to_hex(Tree::from_directory(root / "game").root()), to_hex(before.root()));
  // And the discs are genuinely outside it: game/ never grew a discs entry.
  CHECK_EQ(Tree::from_directory(root / "game").size(), before.size());
}

// The whole of defect (1): everything the installer wrote, not just the game
// folder. A 1998 setup drops a runtime into C:\windows\system32, registers it,
// and the registry fragment naming it travels - so the file has to travel too
// or the recipient gets registrations pointing at nothing.
static void test_what_the_installer_wrote_outside_the_game(const fs::path& tmp) {
  // The rule, first, with no filesystem in it.
  CHECK(install::is_outside_the_game("windows/system32/msvcrt.dll", "Program Files/Adventure II"));
  CHECK(install::is_outside_the_game("windows/system32/MSVCRT.DLL", ""));
  CHECK(!install::is_outside_the_game("Program Files/Adventure II/Adventure II.exe",
                                      "Program Files/Adventure II"));
  // The same path as the installer spelled it, and as Windows spells it.
  CHECK(!install::is_outside_the_game("program files\\adventure ii\\data.pak",
                                      "Program Files/Adventure II"));
  CHECK(!install::is_outside_the_game("Program Files/Adventure II", "Program Files/Adventure II"));
  // A sibling whose name merely starts the same way is not inside it.
  CHECK(install::is_outside_the_game("Program Files/Adventure II Expansion/x.dll",
                                     "Program Files/Adventure II"));
  // Scratch never travels, whoever left it.
  CHECK(!install::is_outside_the_game("windows/Temp/_ins0432._mp", "Program Files/Game"));
  CHECK(!install::is_outside_the_game("users/kunal/Temp/ikernel.exe", "Program Files/Game"));
  CHECK(!install::is_outside_the_game("users/kunal/Local Settings/Temp/setup.inx",
                                      "Program Files/Game"));
  // But the user's own data directories do.
  CHECK(install::is_outside_the_game("users/kunal/Application Data/Game/config.ini",
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
    if (install::is_outside_the_game(w, "Program Files/Game")) outside.push_back(w);
  }
  // Three: the two runtime files and the one that was deleted between the diff
  // and now. The rule is about paths and knows nothing about the filesystem.
  CHECK_EQ(outside.size(), 3u);

  fs::path system = root / "system";
  install::SystemFiles got = install::gather_system_files(drive_c, outside, system);
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
  CHECK_EQ(install::restore_system_files(system, prefix_c), 2u);
  std::ifstream back(prefix_c / "windows" / "system32" / "msvcrt.dll");
  std::string line;
  std::getline(back, line);
  CHECK_EQ(line, std::string("a shared runtime"));
  CHECK(fs::exists(prefix_c / "windows" / "system32" / "comdlg32.ocx"));

  // Nothing to restore is not a failure: a copy install writes no system files
  // and a revision 1 pack has nowhere to keep them.
  CHECK_EQ(install::restore_system_files(root / "nosuchtree", prefix_c), 0u);
}

// Defect (4): a pack built with "include the discs" unchecked still has to be a
// pack - the game, the registry and now the system files - and still has to say
// so honestly, which is Meta::Disc::embedded and not the body.
static void test_a_body_without_discs_still_carries_everything_else(const fs::path& tmp) {
  fs::path src = tmp / "no-discs";
  fs::remove_all(src);
  fs::path game = src / "tree";
  fs::create_directories(game);
  std::ofstream(game / "game.exe") << "MZ the game";
  Tree before = Tree::from_directory(game);

  fs::path system = src / "system";
  fs::create_directories(system / "windows" / "system32");
  std::ofstream(system / "windows" / "system32" / "msvcrt.dll") << "a shared runtime";

  fs::path root = src / "body";
  install::lay_out_body(root, game, {}, "REGEDIT4\n", system);

  CHECK(fs::exists(root / "game" / "game.exe"));
  CHECK(fs::exists(root / "system" / "windows" / "system32" / "msvcrt.dll"));
  CHECK(fs::exists(root / "registry.reg"));
  CHECK(!fs::exists(root / "discs"));
  // system/ is beside game/, never inside it: the Merkle root still means the
  // identity of the installed game and nothing else.
  CHECK_EQ(to_hex(Tree::from_directory(root / "game").root()), to_hex(before.root()));
  CHECK_EQ(Tree::from_directory(root / "game").size(), before.size());
}

// A mounted CD is a directory, and so is a disc somebody already extracted -
// and "load game CD/DVD" is the headline path, not an edge case. Every reader
// below used to hand the path to 7z, which exits 2 on a directory: the wizard's
// third step took the throw out of the SDL loop and the program with it.
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
  std::vector<install::BodyDisc> discs = {install::BodyDisc{tree, "GAME CD", 0x1u, {}}};
  fs::path root = tmp / "linked-body";
  install::lay_out_body(root, game, discs, "");

  fs::path in_body = root / "discs" / "1" / "MOVIE.BIK";
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
  CHECK(!install::registry_marker_matches(prefix, frag));

  install::write_registry_marker(prefix, frag);
  CHECK(install::registry_marker_matches(prefix, frag));

  // A different fragment is a different pack, and gets applied.
  CHECK(!install::registry_marker_matches(prefix, frag + "\"B\"=\"2\"\n"));

  // The marker holds the hash and nothing else, so it can be read by a person
  // and cannot be confused with a prefix's own files.
  std::ifstream f(prefix / ".kretro-registry");
  std::string line;
  std::getline(f, line);
  CHECK_EQ(line, to_hex(hash_string(frag)));

  // A prefix that does not exist is not a match, and asking is not an error.
  CHECK(!install::registry_marker_matches(tmp / "no-such-prefix", frag));
}

// The unpacked-game cache, and why an id is not enough to key it on.
//
// Where FUSE is unavailable the body is unpacked once into state/extracted/<id>
// and played from there. The cache used to be gated on a marker file whose
// contents were the literal "1\n": it said that something had been unpacked,
// never what. So reinstalling a game - same id, new bytes - was played from the
// install before it, and a pack whose body is laid out the other way round was
// reached through a path that does not exist, which surfaces as "no such exe in
// the installed game" about a pack that is perfectly good.
//
// The stamp says the three things that make one unpacked tree a different tree.
// Unpacking itself needs the DwarFS tool and a real body, so what tier one
// proves is the decision: when the cache is reused and when it is thrown away.
static void test_extraction_stamp(const fs::path& tmp) {
  Header flat_h;
  flat_h.blake3_root = hash_string("the game tree");
  Meta flat;
  flat.layout = "flat";
  flat.body.blake3 = hash_string("the body bytes");

  // The same install, laid out the way this build lays bodies out. Nothing else
  // about it has changed - and it still may not be played from the flat tree,
  // because the game is a directory down from where it used to be.
  Meta rooted = flat;
  rooted.layout = "rooted";
  CHECK(session::extraction_stamp(flat, flat_h) != session::extraction_stamp(rooted, flat_h));

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
  CHECK(!session::extraction_stamp_matches(stamp, session::extraction_stamp(rooted, flat_h)));
  CHECK(!session::extraction_stamp_matches(stamp, session::extraction_stamp(rebuilt, flat_h)));

  // Readable by a person, and one line.
  std::ifstream f(stamp);
  std::string line;
  std::getline(f, line);
  CHECK_EQ(line, want);
  CHECK_EQ(line, "flat " + to_hex(flat.body.blake3) + " " + to_hex(flat_h.blake3_root));

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

  // The lock lives beside the layers it guards, so uninstalling the game takes
  // it away with everything else under saves/<id>.
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

  const std::vector<install::RegValue> installer_wrote = {
      {"Software\\Example Publisher\\Adventure II", "InstallPath", "sz",
       "C:\\Program Files\\Adventure II"},
      {"Software\\Example Publisher\\Adventure II", "Resolution", "dword", "00000001"},
  };
  const std::vector<install::RegValue> stock = {{"Software\\Wine", "Version", "sz", "winxp"}};

  // What install::run does today, with apply_registry false: the staging prefix
  // is stock before the installer runs and carries the game's keys after.
  std::vector<install::RegValue> after = stock;
  after.insert(after.end(), installer_wrote.begin(), installer_wrote.end());
  std::string rebuilt = install::to_reg_fragment(install::diff_reg(stock, after));
  CHECK(rebuilt.find("\"InstallPath\"") != std::string::npos);
  CHECK(rebuilt.find("\"Resolution\"") != std::string::npos);

  // What it would do if the recipe's fragment had been applied first: the keys
  // are on both sides, so the diff is empty and the fragment comes out blank.
  std::string cancelled = install::to_reg_fragment(install::diff_reg(after, after));
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
// open and run. `dir / name` throws `dir` away when `name` is absolute, so the
// collection directory used to vanish from under the join and a shared recipe
// could name any file on the importer's disk.
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

int main() {
  fs::path tmp = fs::temp_directory_path() / "kretro-test-install";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  // The game lock and the saves generations live under state_dir(), and
  // state_dir() caches its answer in a function-local static (paths.cpp:19),
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
    test_a_body_without_discs_still_carries_everything_else(tmp);
    test_a_directory_is_a_disc(tmp);
    test_a_directory_disc_travels_as_files(tmp);
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
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  FAIL unexpected exception: %s\n", e.what());
    ++failures;
  }

  fs::remove_all(tmp);
  std::fprintf(stderr, "\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
