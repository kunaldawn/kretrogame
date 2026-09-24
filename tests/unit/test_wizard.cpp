// Tier 1 unit tests for the wizard's arithmetic: no display, no Wine, no disc.
//
// These live against src/install/ rather than src/gui/wizard/ for
// one reason: a test binary links LIB_OBJ + B3_OBJ and cannot see a
// translation unit that includes SDL. Anything the wizard decides which could
// be quietly wrong belongs on this side of that line.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "install/build.h"
#include "install/draft.h"
#include "install/game_id.h"
#include "install/keys.h"
#include "install/manifest.h"
#include "install/preset.h"
#include "install/setup_ref.h"
#include "install/source.h"
#include "install/staging.h"
#include "install/survey.h"
#include "util/hash.h"
#include "util/paths.h"   // cache_dir(), games_dir(): where a test looks
#include "support/check.h"

namespace fs = std::filesystem;
using namespace kg;

static void write_file(const fs::path& p, size_t bytes) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f << std::string(bytes, 'x');
}

static void test_classify(const fs::path& tmp) {
  fs::path d = tmp / "classify";
  std::error_code ec;
  fs::create_directories(d / "Disc 1", ec);
  write_file(d / "game.iso", 3);
  write_file(d / "game.zip", 3);
  write_file(d / "setup.exe", 3);
  write_file(d / "readme.txt", 3);

  using K = install::Source::Kind;
  CHECK(install::classify_source(d / "Disc 1").kind == K::Directory);
  CHECK(install::classify_source(d / "game.iso").kind == K::DiscImage);
  CHECK(install::classify_source(d / "game.zip").kind == K::Archive);
  CHECK(install::classify_source(d / "setup.exe").kind == K::BareExe);

  install::Source bad = install::classify_source(d / "readme.txt");
  CHECK(bad.kind == K::Unreadable);
  CHECK(!bad.trouble.empty());
  CHECK(install::classify_source(d / "nothing-here.iso").kind == K::Unreadable);

  // Nothing was opened: three bytes is not an ISO 9660 volume descriptor, and
  // classify still answered DiscImage for it.
  CHECK_EQ(fs::file_size(d / "game.iso", ec), static_cast<uintmax_t>(3));
}

static void test_slug() {
  CHECK_EQ(install::slug("Demo-Game: Side Story"), std::string("demo-game-side-story"));
  CHECK_EQ(install::slug("  Adventure II  "), std::string("adventure-ii"));
  CHECK_EQ(install::slug("Example G.A.M.E."), std::string("example-g-a-m-e"));
  CHECK_EQ(install::slug(""), std::string(""));

  // The staging directory `kretro swap <id> <n>` asks install::staging_dir
  // for to find a running install it did not start, so it is a contract.
  CHECK_EQ(install::staging_dir("adventure-ii"), cache_dir() / "install-adventure-ii");
}

static void test_candidate_ranking() {
  // What detect_install_dir is handed: the diff of drive_c across the
  // installer run. Three directories come out of these four files.
  auto added = [](const char* p, uint64_t size) {
    TreeEntry te;
    te.path = p;
    te.mode = 0100644;
    te.size = size;
    return te;
  };
  Tree::Diff d;
  d.added.push_back(added("Program Files/Adventure II/Adventure II.exe", 1400000));
  d.added.push_back(added("Program Files/Adventure II/data.pak", 260000000));
  d.added.push_back(added("Program Files/Adventure II/Music.pak", 330000000));
  d.added.push_back(added("Temp/setup.log", 4096));

  std::vector<install::Candidate> c =
      install::rank_candidates("/nonexistent-drive-c", d);
  CHECK_EQ(c.size(), 3u);

  // Deepest first: a game's own directory is longer than the publisher's.
  CHECK_EQ(c[0].dir.generic_string(), std::string("Program Files/Adventure II"));
  // Then by file count, which is why the publisher directory beats Temp.
  CHECK_EQ(c[1].dir.generic_string(), std::string("Program Files"));
  CHECK_EQ(c[2].dir.generic_string(), std::string("Temp"));

  // Counts are cumulative over the subtree, so the game directory and its
  // parent show the same number - which is exactly what step 5's page shows.
  CHECK_EQ(c[0].files, 3u);
  CHECK_EQ(c[1].files, 3u);
  CHECK_EQ(c[2].files, 1u);
  CHECK_EQ(c[0].bytes, 591400000ull);
  CHECK_EQ(c[1].bytes, 591400000ull);

  // A drive_c that does not exist yields no executables rather than throwing:
  // ranking is done on a directory listing and a missing one is empty.
  CHECK_EQ(c[0].executables.size(), 0u);

  // Nothing added means no candidates, which is the "the installer wrote
  // nothing to C:" page rather than a crash.
  Tree::Diff none;
  CHECK_EQ(install::rank_candidates("/nonexistent-drive-c", none).size(), 0u);
}

static void test_executable_ranking(const fs::path& tmp) {
  // The install side: no setup preference, biggest first, uninstaller last -
  // and the uninstaller here is the biggest file in the directory, so size
  // alone would put it first.
  fs::path game = tmp / "ranking" / "game";
  write_file(game / "Adventure II.exe", 1400);
  write_file(game / "Adventure II Video Test.exe", 212);
  write_file(game / "unins000.exe", 9000);
  write_file(game / "data.pak", 50000);       // not an .exe, not a candidate

  std::vector<fs::path> g = install::rank_executables(game, false);
  CHECK_EQ(g.size(), 3u);
  CHECK_EQ(g[0].generic_string(), std::string("Adventure II.exe"));
  CHECK_EQ(g[1].generic_string(), std::string("Adventure II Video Test.exe"));
  CHECK_EQ(g[2].generic_string(), std::string("unins000.exe"));

  // The disc side: setup outranks autorun however small it is, and one
  // directory down is included - Arena III's real setup is not at the root.
  fs::path disc = tmp / "ranking" / "disc";
  write_file(disc / "AUTORUN.EXE", 9000);
  write_file(disc / "Setup.exe", 100);
  write_file(disc / "Support" / "dxsetup.exe", 4000);
  write_file(disc / "Support" / "Tools" / "buried.exe", 4000);   // two down: not offered

  std::vector<fs::path> s = install::rank_executables(disc, true);
  CHECK_EQ(s.size(), 3u);
  CHECK_EQ(s[0].generic_string(), std::string("Setup.exe"));
  CHECK_EQ(s[1].generic_string(), std::string("AUTORUN.EXE"));
  CHECK_EQ(s[2].generic_string(), std::string("Support/dxsetup.exe"));

  // Without the disc-side flag, nothing below the top level is offered at all.
  CHECK_EQ(install::rank_executables(disc, false).size(), 2u);
}

static void test_verify_list(const fs::path& tmp) {
  fs::path dir = tmp / "verify" / "Adventure II";
  write_file(dir / "Adventure II.exe", 1400);
  write_file(dir / "data.pak", 90000);
  write_file(dir / "expansion.pak", 80000);
  write_file(dir / "Music.pak", 70000);
  write_file(dir / "speech.pak", 60000);
  write_file(dir / "chars.pak", 50000);
  write_file(dir / "Patch.pak", 40000);
  write_file(dir / "Save" / "huge.sav", 999999);   // under a directory: skipped

  std::vector<std::string> v = install::verify_list(dir, "Adventure II.exe");

  // Every entry is a requirement detect_install_dir has to satisfy at once, so
  // the list is short on purpose.
  CHECK(v.size() <= 5u);
  CHECK_EQ(v.size(), 5u);
  CHECK_EQ(v[0], std::string("Adventure II.exe"));
  // Then the largest files at the top level, biggest first.
  CHECK_EQ(v[1], std::string("data.pak"));
  CHECK_EQ(v[4], std::string("speech.pak"));

  bool has_exe = false;
  for (const std::string& s : v) {
    CHECK(s.find('/') == std::string::npos);
    CHECK(s.find('\\') == std::string::npos);
    if (s == "Adventure II.exe") has_exe = true;
  }
  CHECK(has_exe);

  // An exe given as a path inside the directory still lands as a bare name.
  std::vector<std::string> w = install::verify_list(dir, fs::path("bin") / "Adventure II.exe");
  CHECK_EQ(w[0], std::string("Adventure II.exe"));

  // A directory with only the exe in it gives a one-entry list, not an error.
  fs::path thin = tmp / "verify" / "thin";
  write_file(thin / "game.exe", 10);
  CHECK_EQ(install::verify_list(thin, "game.exe").size(), 1u);
}

static void test_draft_to_meta() {
  std::vector<disc::Disc> discs(2);
  discs[0].source = "/home/someone/discs/AdventureUSA.zip";
  discs[0].label = "INSTALL";
  discs[0].serial = 0x11112222;
  discs[0].info.path = "/home/someone/discs/install.iso";
  discs[0].info.size = 640ull << 20;
  discs[0].info.volume_id = "INSTALL";
  discs[0].info.created = "1999-06-14";
  discs[0].info.prefix = hash_string("the first 64 MB of the install disc");
  discs[1].source = "/home/someone/discs/AdventureUSA.zip";
  discs[1].label = "DEMO_PLAY";
  discs[1].serial = 0x33334444;
  discs[1].info.path = "/home/someone/discs/play.iso";
  discs[1].info.size = 700ull << 20;
  discs[1].info.volume_id = "DEMO_PLAY";
  discs[1].info.prefix = hash_string("the first 64 MB of the play disc");

  install::Draft d;
  d.id = "adventure-ii";
  d.name = "Adventure II";
  d.year = 2000;
  d.serial = "ABCD-1234-EFGH-5678";
  d.method = install::Draft::Method::Installer;
  d.setup = "1/Setup.exe";
  d.windows_version = "winxp";
  d.install_dir = "Program Files/Adventure II";
  d.exe = "Adventure II.exe";
  d.args = "-w";
  d.width = 800;
  d.height = 600;
  d.dgvoodoo = true;
  d.embed_discs = true;

  Meta m = install::draft_to_meta(d, discs);

  CHECK_EQ(m.id, std::string("adventure-ii"));
  CHECK_EQ(m.name, std::string("Adventure II"));
  CHECK_EQ(m.year, 2000u);
  CHECK_EQ(m.recipe.method, std::string("wine_setup"));
  CHECK_EQ(m.run.exe, std::string("Adventure II.exe"));
  CHECK_EQ(m.run.args, std::string("-w"));
  CHECK_EQ(m.run.width, 800u);
  CHECK_EQ(m.run.height, 600u);
  CHECK_EQ(m.run.windows_version, std::string("winxp"));
  CHECK(m.runtime.dgvoodoo);
  CHECK_EQ(m.install.install_dir, std::string("Program Files/Adventure II"));
  CHECK_EQ(m.layout, std::string("rooted"));

  // "which exe on which disc" is split into the two fields the engine reads.
  CHECK_EQ(m.recipe.setup, std::string("Setup.exe"));
  CHECK_EQ(m.recipe.setup_ref, std::string("AdventureUSA.zip#INSTALL"));

  CHECK_EQ(m.discs.size(), 2u);
  CHECK_EQ(m.discs[0].ref, std::string("AdventureUSA.zip#INSTALL"));
  CHECK_EQ(m.discs[0].source, std::string("/home/someone/discs/AdventureUSA.zip"));
  CHECK_EQ(m.discs[1].ref, std::string("AdventureUSA.zip#DEMO_PLAY"));
  CHECK_EQ(m.discs[1].serial, 0x33334444u);
  CHECK(m.discs[1].embedded);
  CHECK_EQ(m.recipe.discs.size(), 2u);
  CHECK_EQ(m.recipe.discs[1], std::string("AdventureUSA.zip#DEMO_PLAY"));

  // What the disc is, not what it is called. Without these, export_recipe
  // refuses the pack outright - "no disc fingerprint recorded" - and a
  // recipient who has the same pressing filed under another name has nothing
  // to recognise it by.
  CHECK_EQ(m.recipe.fingerprints.size(), 2u);
  CHECK_EQ(m.recipe.fingerprints[0].volume_id, std::string("INSTALL"));
  CHECK_EQ(m.recipe.fingerprints[0].size, 640ull << 20);
  CHECK_EQ(m.recipe.fingerprints[0].created, std::string("1999-06-14"));
  CHECK_EQ(to_hex(m.recipe.fingerprints[0].blake3),
           to_hex(hash_string("the first 64 MB of the install disc")));
  CHECK_EQ(to_hex(m.recipe.fingerprints[1].blake3),
           to_hex(hash_string("the first 64 MB of the play disc")));
  // And they survive the trip into a pack, which is what export_recipe reads.
  CHECK_EQ(Meta::decode(m.encode()).recipe.fingerprints.size(), 2u);
  CHECK_EQ(to_hex(Meta::decode(m.encode()).recipe.fingerprints[1].blake3),
           to_hex(hash_string("the first 64 MB of the play disc")));

  // A pack with no disc behind it at all - a repacked installer somebody
  // downloaded - records none, and export_recipe is right to refuse that one.
  CHECK_EQ(install::draft_to_meta(d, {}).recipe.fingerprints.size(), 0u);

  // The serial stays on this machine. The comment at the top of keys.h is the
  // policy; this is it being kept.
  CHECK(m.encode().find("ABCD-1234-EFGH-5678") == std::string::npos);

  // Unchecking the discs marks them, so play can say "needs the original disc"
  // instead of looking for a discs/ that is not in the image.
  install::Draft bare = d;
  bare.embed_discs = false;
  Meta without = install::draft_to_meta(bare, discs);
  CHECK(!without.discs[0].embedded);
  CHECK(!without.discs[1].embedded);
  // And it is still a rooted body, because a body with the discs left out
  // still carries game/, system/ and registry.reg - open_layers only looks for
  // the game at image/game when the layout says so, and every one of those
  // three is there whether the gigabytes came along or not.
  CHECK_EQ(without.layout, std::string("rooted"));
  // The discs are still named, still findable, and still fingerprinted: a pack
  // built without them is exactly the pack you would export as a recipe.
  CHECK_EQ(without.recipe.fingerprints.size(), 2u);
  CHECK_EQ(without.discs[1].source, std::string("/home/someone/discs/AdventureUSA.zip"));

  // The other three methods map onto the other three strings the engine takes.
  install::Draft c = d;
  c.method = install::Draft::Method::Copy;
  c.subdir = "PC";
  CHECK_EQ(install::draft_to_meta(c, discs).recipe.method, std::string("copy"));
  CHECK_EQ(install::draft_to_meta(c, discs).recipe.subdir, std::string("PC"));
  c.method = install::Draft::Method::Unzip;
  c.member = "CLASSIC.ZIP";
  CHECK_EQ(install::draft_to_meta(c, discs).recipe.method, std::string("unzip"));
  CHECK_EQ(install::draft_to_meta(c, discs).recipe.member, std::string("CLASSIC.ZIP"));
  c.method = install::Draft::Method::InstallerExe;
  c.setup = "/home/someone/downloads/Racer.exe";
  Meta ie = install::draft_to_meta(c, discs);
  CHECK_EQ(ie.recipe.method, std::string("installer_exe"));
  CHECK_EQ(ie.recipe.setup, std::string("/home/someone/downloads/Racer.exe"));
}

static void test_staging_lifetime(const fs::path& tmp) {
  rt::Env e;   // no runtime: nothing here runs a program
  auto quiet = [](const std::string&) {};
  fs::path work = tmp / "staging" / "install-testgame";
  std::error_code ec;

  // A directory left by a previous run is cleared, not built on top of: a
  // half-written drive tree from a killed install would be mounted as a disc.
  fs::create_directories(work / "stale", ec);
  {
    install::Build b(e, work, quiet);
    CHECK(fs::exists(work));
    CHECK(!fs::exists(work / "stale"));
    // The layout `kretro swap <id> <n>` walks: install::staging_prefix is
    // work/prefix and install::staging_drive is work/drive-<letter>.
    CHECK(fs::exists(work / "prefix"));
    CHECK(fs::exists(work / "home"));

    // With no installer running, setup_pgid_ is zero and cancel is the flag
    // and nothing else - which is what makes it safe to call from the UI
    // thread at any moment, including this one.
    CHECK(!b.cancelled());
    b.cancel();
    CHECK(b.cancelled());

    CHECK_EQ(b.discs().size(), 0u);
    CHECK_EQ(b.sources().size(), 0u);
    CHECK_EQ(b.drives().size(), 0u);

    std::vector<disc::Disc> two(2);
    two[0].label = "ONE";
    two[1].label = "TWO";
    b.adopt_discs(two);
    CHECK_EQ(b.discs().size(), 2u);
    CHECK_EQ(b.discs()[1].label, std::string("TWO"));
  }
  // Gigabytes under cache_dir() must not survive a cancelled or crashed
  // install. Nothing else deletes this.
  CHECK(!fs::exists(work));

  {
    install::Build b(e, work, quiet);
    b.keep_tree = true;
    write_file(work / "prefix" / "drive_c" / "keepme.txt", 4);
  }
  CHECK(fs::exists(work / "prefix" / "drive_c" / "keepme.txt"));
  fs::remove_all(work, ec);
}

// The staging tree is named for the game, and step 1 has no game to name it
// after: the discs have to be opened before step 2 can offer a name for what
// came off them. Left at install-new, `kretro swap <id> <n>` - which hardcodes
// cache_dir()/install-<id> - could never address a wizard install, and the
// frames taken while the installer ran landed in saves/new/, a journal
// belonging to a game that does not exist.
static void test_rehome(const fs::path& tmp) {
  rt::Env e;
  auto quiet = [](const std::string&) {};
  std::error_code ec;
  fs::path first = tmp / "staging" / "install-new";

  install::Build b(e, first, quiet);
  CHECK_EQ(b.work_dir(), first);
  // What the name is read for, and both of them wrong while it is "new".
  CHECK_EQ(b.journal_dir(), saves_dir() / "new" / "journal");

  // A disc opened into the tree before there was an id: its normalised ISO and
  // its ripped audio live inside the directory that is about to move, and its
  // source file does not - that one is off in the user's own collection.
  write_file(first / "media" / "source-1-0" / "disc.iso", 64);
  write_file(first / "media" / "source-1-0" / "track02.flac", 32);
  std::vector<disc::Disc> one(1);
  one[0].source = "/home/someone/discs/AdventureUSA.zip";
  one[0].iso = first / "media" / "source-1-0" / "disc.iso";
  one[0].info.path = one[0].iso;
  one[0].audio.push_back(disc::AudioTrack{2, 100, first / "media" / "source-1-0" / "track02.flac"});
  b.adopt_discs(one);
  // A prefix booted before the id changed - which is what going back to step 2
  // after an install looks like. Its user directories are symlinks into a home
  // that is about to move, and the stamp is what would let the next prepare
  // skip the wineboot that repairs them.
  write_file(first / "prefix" / ".kretro-ready", 8);

  CHECK(b.rehome("adventure-ii"));

  // The layout the CLI addresses, spelled by the one function that spells it.
  fs::path want = install::staging_dir("adventure-ii");
  CHECK_EQ(b.work_dir(), want);
  CHECK(!fs::exists(first, ec));
  CHECK(fs::exists(want / "prefix"));
  CHECK(fs::exists(want / "media" / "source-1-0" / "disc.iso"));
  // And the journal is the game's own, which is the other half of the name.
  CHECK_EQ(b.journal_dir(), saves_dir() / "adventure-ii" / "journal");
  CHECK(!fs::exists(want / "prefix" / ".kretro-ready", ec));

  // The disc set moved with the tree. A path into a directory that is no
  // longer there is a disc that cannot be mounted three steps later.
  CHECK_EQ(b.discs()[0].iso, want / "media" / "source-1-0" / "disc.iso");
  CHECK_EQ(b.discs()[0].info.path, want / "media" / "source-1-0" / "disc.iso");
  CHECK_EQ(b.discs()[0].audio[0].file, want / "media" / "source-1-0" / "track02.flac");
  // Except the source, which was never inside it.
  CHECK_EQ(b.discs()[0].source, fs::path("/home/someone/discs/AdventureUSA.zip"));

  // Renaming to the name it already has, and renaming to nothing at all, are
  // both nothing happening: the id field is editable and this runs on every
  // pass through step 2.
  CHECK(b.rehome("adventure-ii"));
  CHECK(b.rehome(""));
  CHECK_EQ(b.work_dir(), want);
  CHECK(fs::exists(want / "media" / "source-1-0" / "disc.iso"));

  // A second thought about the id moves it again, over whatever a previous
  // install left under that name.
  fs::create_directories(install::staging_dir("adventure-ii-2") / "leftovers", ec);
  CHECK(b.rehome("adventure-ii-2"));
  CHECK_EQ(b.work_dir(), install::staging_dir("adventure-ii-2"));
  CHECK(!fs::exists(install::staging_dir("adventure-ii-2") / "leftovers", ec));
  CHECK(fs::exists(install::staging_dir("adventure-ii-2") / "media" / "source-1-0" / "disc.iso"));

  fs::remove_all(tmp / "staging", ec);
  fs::remove_all(install::staging_dir("adventure-ii"), ec);
  fs::remove_all(install::staging_dir("adventure-ii-2"), ec);
}

static void test_open_sources(const fs::path& tmp) {
  rt::Env e;   // no runtime: a directory source never reaches 7z
  auto quiet = [](const std::string&) {};
  std::error_code ec;

  fs::path src = tmp / "sources";
  write_file(src / "DISC1" / "SETUP.EXE", 100);
  write_file(src / "DISC1" / "DATA" / "game.dat", 5000);
  write_file(src / "DISC2" / "MOVIES" / "intro.smk", 7000);
  write_file(src / "notes.txt", 10);

  {
    install::Build b(e, tmp / "staging" / "install-multi", quiet);
    b.open_sources({src / "DISC1", src / "DISC2", src / "notes.txt"});

    // Three separate .bin/.cue pairs - or, here, three separate folders - are
    // the ordinary case for a multi-disc game, and no existing path assembled
    // a set from more than one file.
    CHECK_EQ(b.discs().size(), 2u);
    CHECK_EQ(b.discs()[0].label, std::string("DISC1"));
    CHECK_EQ(b.discs()[1].label, std::string("DISC2"));
    // A directory is an extracted disc: the tree itself is what a drive will
    // point at.
    CHECK_EQ(b.discs()[0].iso, src / "DISC1");
    CHECK(b.discs()[0].serial != 0u);

    // A source that could not be read is listed with a reason and does not
    // stop the others.
    CHECK_EQ(b.sources().size(), 3u);
    CHECK(b.sources()[2].kind == install::Source::Kind::Unreadable);
    CHECK(!b.sources()[2].trouble.empty());

    // One Source per file handed over, in the order they were handed over.
    // Step 1's rows are built from the classification made before anything was
    // opened, and it adopts these over the top of them by position: the reason
    // a file would not open is recorded here and nowhere else, and a list that
    // did not line up would put one file's trouble under another file's name.
    const std::vector<fs::path> given = {src / "DISC1", src / "DISC2", src / "notes.txt"};
    for (size_t i = 0; i < given.size(); ++i) {
      CHECK_EQ(b.sources()[i].path.string(), given[i].string());
    }
    // And what opened cleanly keeps the kind it was classified as, so adopting
    // the list wholesale cannot demote a good source.
    CHECK(b.sources()[0].kind == install::Source::Kind::Directory);
    CHECK(b.sources()[0].trouble.empty());
    CHECK(b.sources()[1].kind == install::Source::Kind::Directory);
  }

  // Two dumps of one disc are one disc and one alternate, and it takes the
  // single assemble over the whole concatenation to see that - assemble
  // traverses nothing, so nothing else could.
  fs::copy(src / "DISC1", src / "DISC1-again", fs::copy_options::recursive, ec);
  {
    install::Build b(e, tmp / "staging" / "install-dup", quiet);
    b.open_sources({src / "DISC1", src / "DISC1-again"});
    CHECK_EQ(b.discs().size(), 1u);
    CHECK_EQ(b.sources().size(), 2u);
  }

  // Nothing readable is a fact about the collection, not an error.
  {
    install::Build b(e, tmp / "staging" / "install-empty", quiet);
    b.open_sources({src / "notes.txt"});
    CHECK_EQ(b.discs().size(), 0u);
  }
  fs::remove_all(tmp / "staging", ec);
}

static void test_swap_disc(const fs::path& tmp) {
  rt::Env e;
  auto quiet = [](const std::string&) {};
  std::error_code ec;
  fs::path work = tmp / "staging" / "install-swapper";

  install::Build b(e, work, quiet);
  // After construction, because the constructor clears the staging tree.
  // These are the trees mount_discs writes and `kretro swap` looks for.
  fs::create_directories(work / "drive-d", ec);
  fs::create_directories(work / "drive-e", ec);
  std::ofstream(work / "drive-e" / ".windows-label") << "DEMO_PLAY\n";

  b.swap_disc('d', 1);
  fs::path link = work / "prefix" / "dosdevices" / "d:";
  CHECK(fs::is_symlink(link, ec));
  CHECK_EQ(fs::read_symlink(link, ec), work / "drive-e");

  b.swap_disc('d', 0);
  CHECK_EQ(fs::read_symlink(link, ec), work / "drive-d");

  // A disc that is not mounted is said so, not silently ignored: an installer
  // waiting on disc 3 would wait forever.
  CHECK_THROWS(b.swap_disc('d', 2));

  fs::remove_all(work, ec);
}

// The drive row the install page draws, and the one rule about it: it is a
// copy. The page starts drawing on the frame the worker starts, and the worker
// spends its first minutes inside mount_discs rebuilding the very vectors the
// row iterates - so a reference held across a frame is a use-after-free the
// moment one of them reallocates.
static void test_mounted_snapshot(const fs::path& tmp) {
  rt::Env e;
  auto quiet = [](const std::string&) {};
  std::error_code ec;
  fs::path work = tmp / "staging" / "install-mounted";

  install::Build b(e, work, quiet);
  // Nothing yet: no discs, no drives, and above all not ready. `ready` is what
  // makes the page say "putting the discs in their drives" instead of drawing
  // a row out of a vector that is still being filled.
  install::Build::Mounted m0 = b.mounted();
  CHECK(!m0.ready);
  CHECK(m0.drives.empty());
  CHECK(m0.discs.empty());

  std::vector<disc::Disc> two(2);
  two[0].label = "DISC1";
  two[1].label = "DISC2";
  b.adopt_discs(two);

  install::Build::Mounted m1 = b.mounted();
  CHECK(!m1.ready);            // adopted is not mounted
  CHECK_EQ(m1.discs.size(), 2u);
  CHECK_EQ(m1.discs[0], std::string("DISC1"));
  CHECK(m1.drives.empty());

  // The copy a frame is holding does not change under it when the disc set
  // does. This is the whole of the fix, stated as a test: what the UI thread
  // holds is its own, and Build may do as it likes with its containers.
  std::vector<disc::Disc> one(1);
  one[0].label = "SOMETHING ELSE";
  b.adopt_discs(one);
  CHECK_EQ(m1.discs.size(), 2u);
  CHECK_EQ(m1.discs[0], std::string("DISC1"));
  CHECK_EQ(b.mounted().discs.size(), 1u);

  // mount_discs registers a CD-ROM drive through wine, which a tier 1 test has
  // none of, so it throws part way. What matters here is that it leaves the
  // page gated rather than half a row: `ready` goes false on the way in and is
  // only set once every disc is in a drive.
  CHECK_THROWS(b.mount_discs());
  CHECK(!b.mounted().ready);

  fs::remove_all(work, ec);
}

static void test_file_counter(const fs::path& tmp) {
  rt::Env e;
  auto quiet = [](const std::string&) {};
  std::error_code ec;
  fs::path work = tmp / "staging" / "install-counter";

  install::Build b(e, work, quiet);
  fs::path c = work / "prefix" / "drive_c";
  write_file(c / "windows" / "system32" / "a.dll", 10);
  write_file(c / "windows" / "system32" / "b.dll", 10);

  // Directories count too: it is a count of entries under drive_c, which is
  // what a person watching an installer sees moving.
  CHECK_EQ(install::count_entries(c), 4u);   // windows, system32, a.dll, b.dll
  // A drive_c that does not exist yet is nothing written, not an error: the
  // page asks for this number before the prefix has been prepared.
  CHECK_EQ(install::count_entries(work / "no-such-drive"), 0u);

  // Nothing has counted yet. The page reads a published number and never walks
  // anything itself - on the UI thread that walk would happen in the middle
  // of a frame, once a second for the length of the install.
  CHECK_EQ(b.files_written_so_far(), 0u);

  b.start_counting();
  size_t n = 0;
  for (int i = 0; i < 400 && n != 4u; ++i) {
    n = b.files_written_so_far();
    if (n != 4u) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK_EQ(n, 4u);

  // Stopped means joined. Files appearing after that do not change the
  // published number, which is what proves no thread is still walking the tree
  // a ~Build is about to delete.
  b.stop_counting();
  write_file(c / "windows" / "system32" / "d.dll", 10);
  CHECK_EQ(b.files_written_so_far(), 4u);

  fs::remove_all(work, ec);
}

static void test_diff_after(const fs::path& tmp) {
  rt::Env e;
  auto quiet = [](const std::string&) {};
  std::error_code ec;
  fs::path work = tmp / "staging" / "install-diff";

  install::Build b(e, work, quiet);
  // Through the helper, not through a fourth hand-written copy of the layout:
  // if staging_drive_c named a directory the engine does not walk, everything
  // written below would be invisible to diff_after and this test would say so.
  fs::path c = install::staging_drive_c(work);
  fs::create_directories(c, ec);
  b.snapshot_before();          // an empty drive_c, no registry to read

  write_file(c / "Program Files" / "Adventure II" / "Adventure II.exe", 1400);
  write_file(c / "Program Files" / "Adventure II" / "data.pak", 90000);
  write_file(c / "Temp" / "setup.log", 40);
  b.diff_after();

  CHECK_EQ(b.candidates().size(), 3u);
  CHECK_EQ(b.candidates()[0].dir.generic_string(), std::string("Program Files/Adventure II"));
  // The executables of the chosen directory are ranked as part of the answer,
  // so step 6 has its list without walking the tree again.
  CHECK_EQ(b.candidates()[0].executables.size(), 1u);
  CHECK_EQ(b.candidates()[0].executables[0].generic_string(), std::string("Adventure II.exe"));

  // An installer that wrote nothing did not run, or was cancelled. This is the
  // page that offers step 3 again - usually a DemoShield front end was
  // launched instead of the setup - and it must be told apart from success.
  install::Build empty(e, tmp / "staging" / "install-nothing", quiet);
  fs::create_directories(install::staging_drive_c(tmp / "staging" / "install-nothing"), ec);
  empty.snapshot_before();
  CHECK_THROWS(empty.diff_after());
  // And it leaves nothing behind to choose from, which is the state step 5
  // draws that page out of. The wizard sends a failed install there now rather
  // than dropping the user back on step 3 with the screen as they left it: an
  // empty candidate list is the page, so it has to survive the throw rather
  // than be half a list nobody can use.
  CHECK(empty.candidates().empty());

  fs::remove_all(tmp / "staging", ec);
}

static void test_write_draft_verify(const fs::path& tmp) {
  rt::Env e;
  auto quiet = [](const std::string&) {};
  std::error_code ec;
  fs::path work = tmp / "staging" / "install-write";

  install::Build b(e, work, quiet);
  fs::path dir = install::staging_drive_c(work) / "Program Files" / "Adventure II";
  write_file(dir / "Adventure II.exe", 1400);
  write_file(dir / "data.pak", 90000);

  install::Draft d;
  d.id = "adventure-ii-write-test";
  d.name = "Adventure II";
  d.install_dir = "Program Files/Adventure II";
  d.exe = "Adventure II.exe";

  // No KRETRO_DWARFS in a tier 1 test, and there is no honest pack without a
  // body: the engine says so rather than writing an envelope around nothing.
  CHECK_THROWS(b.write(d));

  // The body was laid out before the packing that failed, and it is rooted:
  // the game is at body/game, which is where session::open_layers looks
  // for it and what the Merkle root is taken over. It is the confirmed
  // directory that travelled, not drive_c.
  CHECK(fs::exists(work / "body" / "game" / "Adventure II.exe"));
  CHECK(fs::exists(work / "body" / "game" / "data.pak"));
  CHECK(!fs::exists(work / "tree"));
  CHECK(!fs::exists(work / "prefix" / "drive_c" / "Program Files" / "Adventure II"));

  fs::remove_all(work, ec);
}

static void test_root_advisory(const fs::path& tmp) {
  rt::Env e;
  auto quiet = [](const std::string&) {};
  std::error_code ec;

  // A stand-in for the DwarFS tool. install::run's real one produces an image;
  // this one produces an empty file, which is all write() looks for, and it
  // means a tier 1 suite can reach the root check without a runtime.
  fs::path stub = tmp / "fake-dwarfs";
  {
    std::ofstream f(stub);
    f << "#!/bin/sh\n"
      << "while [ $# -gt 0 ]; do\n"
      << "  if [ \"$1\" = -o ]; then : > \"$2\"; exit 0; fi\n"
      << "  shift\n"
      << "done\n"
      << "exit 1\n";
  }
  fs::permissions(stub, fs::perms::owner_all, ec);
  ::setenv("KRETRO_DWARFS", stub.c_str(), 1);

  fs::path work = tmp / "staging" / "install-root";
  install::Build b(e, work, quiet);
  write_file(work / "prefix" / "drive_c" / "Program Files" / "Rooted" / "game.exe", 1200);

  install::Draft d;
  d.id = "root-advisory-test";
  d.name = "Rooted";
  d.install_dir = "Program Files/Rooted";
  d.exe = "game.exe";
  install::Result res = b.write(d);

  // The root is the root of game/, which is the value a recipe's expect_root
  // is written from and compared against.
  CHECK(fs::exists(res.pack, ec));
  CHECK(res.root == Tree::from_directory(work / "body" / "game").root());

  // The comparison install::run makes, both ways round. A mismatch is a fact
  // reported and not a refusal - two people clicking through InstallShield
  // need not produce the same bytes - so what has to hold is only that the
  // answer is false when the roots differ, rather than opt.expect_root_set
  // and therefore always true.
  Hash other = res.root;
  other[0] = static_cast<uint8_t>(other[0] ^ 0xff);
  CHECK(!(res.root == other));
  CHECK(res.root == res.root);

  ::unsetenv("KRETRO_DWARFS");
  fs::remove(games_dir() / "root-advisory-test.kgpack", ec);
  fs::remove_all(tmp / "staging", ec);
}

// An id names four things at once, and a check against one of them is not a
// check: a fresh install that reused an id would adopt somebody else's Wine
// prefix and somebody else's saves, and the saves are the half that cannot be
// reinstalled from a disc.
static void test_id_clash(const fs::path& tmp) {
  rt::Env e;   // no runtime: the manifest namespace is reached through
               // KRETRO_MANIFESTS, which manifest_dirs consults first.
  std::error_code ec;
  fs::create_directories(tmp / "manifests", ec);
  ::setenv("KRETRO_MANIFESTS", (tmp / "manifests").c_str(), 1);
  ensure_state_dirs();

  CHECK(!install::id_clash(e, "nothing-here").any());

  std::ofstream(games_dir() / "packed.kgpack") << "x";
  install::IdClash a = install::id_clash(e, "packed");
  CHECK(a.pack);
  CHECK(!a.saves);
  CHECK(a.any());

  fs::create_directories(saves_dir() / "saved");
  CHECK(install::id_clash(e, "saved").saves);
  fs::create_directories(prefixes_dir() / "prefixed");
  CHECK(install::id_clash(e, "prefixed").prefix);
  std::ofstream(tmp / "manifests" / "shipped.toml") << "id = \"shipped\"\n";
  CHECK(install::id_clash(e, "shipped").manifest);

  // The suggestion is an edit for the user to accept, so it has to be free.
  CHECK_EQ(install::next_free_id(e, "packed"), std::string("packed-2"));
  std::ofstream(games_dir() / "packed-2.kgpack") << "x";
  CHECK_EQ(install::next_free_id(e, "packed"), std::string("packed-3"));
  CHECK_EQ(install::next_free_id(e, "nothing-here"), std::string("nothing-here-2"));
}

// Opening the wizard from the game page or the Library's disc table means
// opening it on what that manifest already knows. A disc the manifest names
// but the collection does not hold is reported by name rather than resolving
// to an empty path that fails four steps later.
static void test_draft_from_meta(const fs::path& tmp) {
  fs::path iso = tmp / "iso";
  fs::create_directories(iso);
  std::ofstream(iso / "SideStory.zip") << "not really a zip";
  setenv("KRETRO_ISO_DIR", iso.c_str(), 1);

  Meta m;
  m.id = "demo-game-side-story";
  m.name = "Demo-Game: Side Story";
  m.year = 2001;
  m.recipe.method = "installer";
  m.recipe.setup = "setup.exe";
  m.recipe.discs = {"SideStory.zip#SIDESTORY", "Missing.zip#DISC2"};
  m.run.exe = "dg.exe";
  m.run.args = "-game sstory";
  m.run.width = 800;
  m.run.height = 600;
  m.run.windows_version = "win98";
  m.runtime.dgvoodoo = true;

  install::Prefill p = install::draft_from_meta(m);
  CHECK_EQ(p.draft.id, std::string("demo-game-side-story"));
  CHECK_EQ(p.draft.name, std::string("Demo-Game: Side Story"));
  CHECK_EQ(p.draft.year, 2001u);
  CHECK_EQ(p.draft.exe.string(), std::string("dg.exe"));
  CHECK_EQ(p.draft.args, std::string("-game sstory"));
  CHECK_EQ(p.draft.width, 800u);
  CHECK_EQ(p.draft.windows_version, std::string("win98"));
  CHECK(p.draft.dgvoodoo);
  CHECK_EQ(p.draft.sources.size(), 1u);
  CHECK_EQ(p.draft.sources[0], iso / "SideStory.zip");
  CHECK_EQ(p.missing.size(), 1u);
  CHECK_EQ(p.missing[0], std::string("Missing.zip#DISC2"));

  // One disc and no reference: the path alone is the whole answer, and a
  // number in front of it would be inventing one.
  CHECK_EQ(p.draft.setup.generic_string(), std::string("setup.exe"));
}

// Which disc the installer is on survives the round trip.
//
// draft_to_meta splits the wizard's "<n>/<path>" into recipe.setup_ref and
// recipe.setup; draft_from_meta read only the second half, so a recipe whose
// installer sits on disc 2 - an expansion on a compilation's second disc -
// came back naming disc 1's copy of that path, and step 4 ran it off the wrong
// mount.
static void test_setup_ref_round_trip() {
  Meta m;
  m.recipe.method = "wine_setup";
  m.recipe.discs = {"Arena3Gold.zip#ARENA3", "Arena3Gold.zip#EXPANSION"};
  m.recipe.setup_ref = "Arena3Gold.zip#EXPANSION";
  m.recipe.setup = "Setup/Setup.exe";
  CHECK_EQ(install::setup_from_recipe(m).generic_string(), std::string("2/Setup/Setup.exe"));

  // A disc spells its own separators and a manifest copies whatever it said;
  // what comes back is joined onto a mount point on this filesystem.
  m.recipe.setup = "Setup\\Setup.exe";
  CHECK_EQ(install::setup_from_recipe(m).generic_string(), std::string("2/Setup/Setup.exe"));

  // A pack carries its discs twice, and an older one may only have the bodies.
  Meta bodies;
  bodies.recipe.method = "wine_setup";
  bodies.recipe.setup_ref = "Gold.zip#DISC2";
  bodies.recipe.setup = "SETUP.EXE";
  bodies.discs.resize(2);
  bodies.discs[0].ref = "Gold.zip#DISC1";
  bodies.discs[1].ref = "Gold.zip#DISC2";
  CHECK_EQ(install::setup_from_recipe(bodies).generic_string(), std::string("2/SETUP.EXE"));

  // Nothing names a disc: every one-disc game, and the path stands as it is.
  Meta one;
  one.recipe.method = "wine_setup";
  one.recipe.discs = {"Blue.zip#SIDESTORY"};
  one.recipe.setup = "ssinstall.EXE";
  CHECK_EQ(install::setup_from_recipe(one).generic_string(), std::string("ssinstall.EXE"));

  // installer_exe's setup is an absolute path to a downloaded file and names
  // no disc, whatever else the recipe happens to carry.
  Meta bare;
  bare.recipe.method = "installer_exe";
  bare.recipe.discs = {"Gold.zip#DISC1", "Gold.zip#DISC2"};
  bare.recipe.setup_ref = "Gold.zip#DISC2";
  bare.recipe.setup = "/home/someone/downloads/Racer.exe";
  CHECK_EQ(install::setup_from_recipe(bare).generic_string(),
           std::string("/home/someone/downloads/Racer.exe"));

  // A reference to a disc the recipe does not list answers nothing, and
  // guessing a number would be worse than the path on its own.
  Meta odd;
  odd.recipe.method = "wine_setup";
  odd.recipe.discs = {"Gold.zip#DISC1"};
  odd.recipe.setup_ref = "Somewhere Else.zip#DISC9";
  odd.recipe.setup = "SETUP.EXE";
  CHECK_EQ(install::setup_from_recipe(odd).generic_string(), std::string("SETUP.EXE"));

  // And the whole of the point: what the wizard wrote is what it reads back.
  std::vector<disc::Disc> discs(2);
  discs[0].source = "/discs/Arena3Gold.zip";
  discs[0].label = "ARENA3";
  discs[1].source = "/discs/Arena3Gold.zip";
  discs[1].label = "EXPANSION";
  install::Draft d;
  d.id = "arena3-expansion";
  d.method = install::Draft::Method::Installer;
  d.setup = "2/Setup/Setup.exe";
  Meta packed = install::draft_to_meta(d, discs);
  CHECK_EQ(packed.recipe.setup_ref, std::string("Arena3Gold.zip#EXPANSION"));
  CHECK_EQ(install::setup_from_recipe(packed).generic_string(), std::string("2/Setup/Setup.exe"));

  // A preset taken on step 2 goes through the same door: apply_preset writing
  // the bare path would have step 3 open on disc 1's copy of it.
  install::Draft taken;
  install::apply_preset(packed, &taken);
  CHECK_EQ(taken.setup.generic_string(), std::string("2/Setup/Setup.exe"));
}

// The staging layout is shared with `kretro swap <id> <n>`, which finds it
// through install::staging_dir and install::staging_drive; if the command and
// the install ever disagree the command addresses nothing and says nothing.
static void test_staging_layout() {
  CHECK_EQ(install::staging_drive_c("/tmp/install-adventure2").string(),
           std::string("/tmp/install-adventure2/prefix/drive_c"));
  const fs::path w = "/tmp/install-adventure2";
  CHECK_EQ(install::staging_drive(w, 0), w / "drive-d");
  CHECK_EQ(install::staging_drive(w, 2), w / "drive-f");
  CHECK_EQ(install::staging_prefix(w), w / "prefix");
}

// draft_to_meta must carry the list into the pack, or the wizard computes it
// and throws it away. What goes *into* the list is Task 204's business, and
// test_verify_list above already covers it.
static void test_draft_verify(const fs::path& tmp) {
  fs::path dir = tmp / "carried";
  write_file(dir / "dg.exe", 2);

  install::Draft d;
  d.id = "side-story";
  d.name = "Demo-Game: Side Story";
  d.exe = "dg.exe";
  d.install_dir = "Program Files/Publisher/Demo-Game";
  d.verify = install::verify_list(dir, d.exe);

  Meta m = install::draft_to_meta(d, {});
  CHECK_EQ(m.recipe.verify.size(), 1u);
  CHECK_EQ(m.recipe.verify[0], std::string("dg.exe"));

  // And Build::write keeps what it was handed rather than working the same
  // question out again. Recomputing unconditionally would make the field
  // something the wizard fills, carries across four steps and has discarded
  // on arrival.
  fs::path root = tmp / "carried-root";
  write_file(root / "Program Files" / "Publisher" / "Demo-Game" / "dg.exe", 2);
  write_file(root / "Program Files" / "Publisher" / "Demo-Game" / "assets.pak", 90000);
  CHECK_EQ(install::verify_for(d, root).size(), 1u);
  CHECK_EQ(install::verify_for(d, root)[0], std::string("dg.exe"));

  // A draft that arrived without one - which is every draft the wizard has not
  // taken through step 6 - gets one read off the confirmed directory now.
  install::Draft bare = d;
  bare.verify.clear();
  std::vector<std::string> read_now = install::verify_for(bare, root);
  CHECK_EQ(read_now.size(), 2u);
  CHECK_EQ(read_now[0], std::string("dg.exe"));
  CHECK_EQ(read_now[1], std::string("assets.pak"));
}

// A manifest is offered when it recognises what is already on the table, and
// otherwise says nothing. It is never a gate: a disc no manifest names must
// install exactly as easily as one that four of them do.
static void test_preset_match(const fs::path& tmp) {
  fs::path mdir = tmp / "manifests";
  fs::create_directories(mdir);
  {
    std::ofstream f(mdir / "test-blue.toml");
    f << "id   = \"test-blue\"\n"
         "name = \"Test: Side Story\"\n"
         "year = 2001\n"
         "\n[source]\n"
         "method = \"wine_setup\"\n"
         "discs  = [\"Test Platinum.zip#TESTBLUE\"]\n"
         "setup  = \"ssinstall.EXE\"\n"
         "verify = [\"dg.exe\"]\n"
         "\n[run]\n"
         "exe             = \"dg.exe\"\n"
         "args            = \"-game sstory -window\"\n"
         "width           = 800\n"
         "height          = 600\n"
         "windows_version = \"winxp\"\n"
         "\n[wine]\ndgvoodoo = true\n";
  }
  {
    std::ofstream f(mdir / "test-nope.toml");
    f << "id   = \"test-nope\"\n"
         "name = \"Test: Something Else\"\n"
         "\n[source]\n"
         "method = \"wine_setup\"\n"
         "discs  = [\"Other.zip#NOTHERE\"]\n"
         "\n[run]\nexe = \"other.exe\"\n";
  }
  setenv("KRETRO_MANIFESTS", mdir.c_str(), 1);

  std::vector<disc::Disc> discs(1);
  discs[0].label = "testblue";              // labels are shouted on disc; case must not matter
  discs[0].source = "/somewhere/Test Platinum.zip";
  discs[0].info.size = 400000000;
  discs[0].info.volume_id = "testblue";

  std::vector<install::Preset> got = install::match_presets(rt::Env{}, discs);
  bool saw_blue = false, saw_nope = false;
  for (const install::Preset& p : got) {
    if (p.id == "test-blue") { saw_blue = true; CHECK_EQ(p.matched, 1u); CHECK_EQ(p.of, 1u); }
    if (p.id == "test-nope") saw_nope = true;
  }
  CHECK(saw_blue);
  CHECK(!saw_nope);

  // A whole-image hash, when we have one, outranks a label: two discs can share
  // a label and no two share a hash.
  Meta byhash;
  byhash.id = "test-hash";
  byhash.name = "Test: By Hash";
  byhash.recipe.discs.push_back("Renamed By Its Owner.zip#WHATEVER");
  DiscFingerprint fp;
  fp.size = 400000000;
  fp.blake3 = hash_string("this disc");
  fp.volume_id = "testblue";
  byhash.recipe.fingerprints.push_back(fp);

  std::vector<disc::Disc> hashed = discs;
  hashed[0].info.whole = hash_string("this disc");
  hashed[0].info.has_whole = true;
  install::Preset ph = install::score_preset(byhash, mdir / "test-hash.toml", hashed);
  CHECK_EQ(ph.matched, 1u);
  CHECK(ph.by_fingerprint);

  // The same manifest against a disc whose name, label and size all differ
  // matches nothing at all - and says so rather than guessing.
  std::vector<disc::Disc> wrong(1);
  wrong[0].label = "SOMETHINGELSE";
  wrong[0].source = "/somewhere/Not It.iso";
  wrong[0].info.size = 12345;
  install::Preset pw = install::score_preset(byhash, mdir / "test-hash.toml", wrong);
  CHECK_EQ(pw.matched, 0u);
  CHECK(!pw.by_fingerprint);

  // Accepting a preset fills the draft and leaves everything editable - which
  // is to say it writes plain fields and nothing else.
  install::Draft d;
  d.sources.push_back("/somewhere/Test Platinum.zip");
  install::apply_preset(install::load_manifest(mdir / "test-blue.toml"), &d);
  CHECK_EQ(d.id, std::string("test-blue"));
  CHECK_EQ(d.name, std::string("Test: Side Story"));
  CHECK_EQ(d.year, 2001u);
  CHECK_EQ(d.setup.string(), std::string("ssinstall.EXE"));
  CHECK_EQ(d.exe.string(), std::string("dg.exe"));
  CHECK_EQ(d.args, std::string("-game sstory -window"));
  CHECK_EQ(d.width, 800u);
  CHECK_EQ(d.height, 600u);
  CHECK_EQ(d.windows_version, std::string("winxp"));
  CHECK(d.dgvoodoo);
  // The sources are the user's, not the manifest's: a preset describes a game,
  // not where this person keeps their files.
  CHECK_EQ(d.sources.size(), 1u);
  // wine_setup is the Installer method, and this manifest names no member or
  // subdir, so both come back empty rather than stale.
  CHECK(d.method == install::Draft::Method::Installer);
  CHECK_EQ(d.member, std::string(""));
  CHECK_EQ(d.subdir, std::string(""));

  // An unzip preset carries the two fields that say which game on the disc it
  // is. Dropping them was not a weaker prefill, it was the wrong one: the
  // compilation disc holds several games as zips and the member is which.
  {
    std::ofstream f(mdir / "test-zip.toml");
    f << "id = \"test-zip\"\nname = \"Test Zip\"\nyear = 1998\n"
         "[source]\nmethod = \"unzip\"\nmember = \"Data2.zip\"\n"
         "subdir = \"Test 1.02.25\"\nverify = [\"MAIN.DAT\"]\n"
         "[run]\nexe = \"TEST.EXE\"\n";
  }
  install::Draft z;
  install::apply_preset(install::load_manifest(mdir / "test-zip.toml"), &z);
  CHECK(z.method == install::Draft::Method::Unzip);
  CHECK_EQ(z.member, std::string("Data2.zip"));
  CHECK_EQ(z.subdir, std::string("Test 1.02.25"));

  unsetenv("KRETRO_MANIFESTS");
}

// The fourth method was unreachable from the wizard: classify_source() calls any .exe
// a BareExe, open_sources deliberately opens no disc for one, and step 1 then
// refused to continue without a disc set. So "run the installer you dropped"
// was offered on step 3 and selectable from nowhere.
static void test_bare_exe_sources(const fs::path& tmp) {
  fs::path d = tmp / "bare";
  write_file(d / "game.iso", 3);
  write_file(d / "Setup Classic.exe", 3);

  std::vector<install::Source> disc = {install::classify_source(d / "game.iso")};
  std::vector<install::Source> exe = {install::classify_source(d / "Setup Classic.exe")};
  std::vector<install::Source> both = {disc[0], exe[0]};

  CHECK(install::bare_exe(disc).empty());
  CHECK_EQ(install::bare_exe(exe).filename().string(), std::string("Setup Classic.exe"));
  CHECK_EQ(install::bare_exe(both).filename().string(), std::string("Setup Classic.exe"));

  // A disc set is enough, and so is the .exe with no disc at all behind it.
  // Nothing readable is not.
  CHECK(install::sources_are_enough(disc, 1));
  CHECK(install::sources_are_enough(exe, 0));
  CHECK(install::sources_are_enough(both, 1));
  CHECK(!install::sources_are_enough(disc, 0));
  CHECK(!install::sources_are_enough({}, 0));

  using M = install::Draft::Method;
  std::vector<M> m = install::methods_for(disc, 1);
  CHECK_EQ(m.size(), 3u);
  CHECK(m[0] == M::Installer);
  CHECK(m[1] == M::Copy);
  CHECK(m[2] == M::Unzip);

  // Copy and unzip take files off a disc, and there is no disc here: the one
  // thing these sources can do is run the installer.
  std::vector<M> e = install::methods_for(exe, 0);
  CHECK_EQ(e.size(), 1u);
  CHECK(e[0] == M::InstallerExe);

  std::vector<M> b = install::methods_for(both, 1);
  CHECK_EQ(b.size(), 4u);
  CHECK(b[3] == M::InstallerExe);

  // A page still has to draw before anything has been read, and what it should
  // be asking for then is a disc.
  std::vector<M> none = install::methods_for({}, 0);
  CHECK_EQ(none.size(), 1u);
  CHECK(none[0] == M::Installer);
}

// The list step 3 shows is ranked - setup before install before autorun - so
// the entry a manifest names is rarely the entry that comes out on top, and
// the page wrote the top one into the draft on every frame it drew. Accepting
// a preset therefore filled in its setup and then threw it away: on a disc
// where INSTALL.EXE and SETUP.EXE both exist and only one of them installs the
// game, which is the entire reason such a manifest is written.
static void test_exe_preselection() {
  // Ranking answers "which is the game" by size, and for Classic that is
  // CLASSIC.EXE - the DOS build sitting beside the Windows one. A manifest
  // that says Classicw.exe is answering the question ranking gets wrong, so
  // the answer has to survive as far as the page that shows the list.
  std::vector<fs::path> exes = {"CLASSIC.EXE", "CLASSICW.EXE", "unins000.exe"};
  CHECK_EQ(install::exe_index(exes, "CLASSICW.EXE"), 1u);
  CHECK_EQ(install::exe_index(exes, "Classicw.exe"), 1u);   // the disc spells it how it likes
  CHECK_EQ(install::exe_index(exes, "CLASSIC.EXE"), 0u);
  // A manifest names the executable, not a path to it; either spelling of the
  // separator resolves to the same filename.
  CHECK_EQ(install::exe_index(exes, "Classic 2/CLASSICW.EXE"), 1u);
  CHECK_EQ(install::exe_index(exes, "Classic 2\\CLASSICW.EXE"), 1u);
  // No match and no name both mean "keep whatever the list chose".
  CHECK_EQ(install::exe_index(exes, "dg.exe"), exes.size());
  CHECK_EQ(install::exe_index(exes, ""), exes.size());
  CHECK_EQ(install::exe_index({}, "dg.exe"), 0u);
}

static void test_a_recipe_does_not_pick_your_files() {
  // A .kgpack is a thing people send each other, and for installer_exe the
  // path in it is executed under Wine. install::run already refused an
  // installer outside the collection - but the wizard's "rebuild it from your
  // disc" button never goes through install::run, so the same recipe that the
  // CLI rejected was one button away from running in the GUI.
  CHECK(install::setup_path_is_safe("Setup.exe"));
  CHECK(install::setup_path_is_safe("Arena3/Setup.exe"));
  CHECK(install::setup_path_is_safe("1/Arena3/Setup.exe"));
  CHECK(install::setup_path_is_safe("Setup\\Setup.exe"));
  CHECK(!install::setup_path_is_safe("/usr/bin/xterm"));
  CHECK(!install::setup_path_is_safe("/home/someone/Downloads/evil.exe"));
  CHECK(!install::setup_path_is_safe("../../../usr/bin/xterm"));
  CHECK(!install::setup_path_is_safe("Arena3/../../../etc/passwd"));
  CHECK(!install::setup_path_is_safe(""));

  // staged_setup is the wizard's road to a setup, which the GUI's rebuild
  // button also takes, so the refusal is there and not in either caller.
  install::Draft d;
  d.method = install::Draft::Method::Installer;
  d.setup = "/usr/bin/xterm";
  CHECK_THROWS(install::staged_setup("/w", d));
  d.setup = "1/Arena3/Setup.exe";
  CHECK_EQ(install::staged_setup("/w", d).string(),
           std::string("/w/drive-d/Arena3/Setup.exe"));

  // And a pack that names one is disarmed as it is read, keeping the rest of
  // the prefill, which is still worth having.
  Meta m;
  m.id = "hostile";
  m.name = "Hostile";
  m.recipe.method = "installer_exe";
  m.recipe.setup = "/usr/bin/xterm";
  install::Prefill p = install::draft_from_meta(m);
  CHECK_EQ(p.draft.setup.string(), std::string(""));
  CHECK_EQ(p.unsafe_setup, std::string("/usr/bin/xterm"));
  CHECK_EQ(p.draft.name, std::string("Hostile"));

  // A path on its own disc is left alone.
  Meta ok;
  ok.id = "fine";
  ok.recipe.method = "wine_setup";
  ok.recipe.setup = "Arena3/Setup.exe";
  install::Prefill q = install::draft_from_meta(ok);
  CHECK_EQ(q.draft.setup.string(), std::string("Arena3/Setup.exe"));
  CHECK_EQ(q.unsafe_setup, std::string(""));
}

// A directory whose name begins with digits is a directory, not a disc number.
static void test_a_digit_directory_is_not_a_disc() {
  install::Draft d;
  d.method = install::Draft::Method::Installer;
  d.setup = "3dfx/Setup.exe";
  Meta m = install::draft_to_meta(d, {});
  CHECK_EQ(m.recipe.setup, std::string("3dfx/Setup.exe"));
  CHECK_EQ(m.recipe.setup_ref, std::string(""));
}

static void test_setup_preselection() {
  std::vector<install::SetupChoice> setups = {
      {0, fs::path("SETUP.EXE")},
      {0, fs::path("INSTALL.EXE")},
      {1, fs::path("SETUP.EXE")},
      {1, fs::path("SETUP") / "SETUP.EXE"},
  };
  // A manifest names the file alone; which disc it is on is a field of its own.
  CHECK_EQ(install::setup_index(setups, "INSTALL.EXE"), 1u);
  // The wizard names both, and the number is which disc: the same filename on
  // disc 2 is a different answer from the one on disc 1.
  CHECK_EQ(install::setup_index(setups, "1/SETUP.EXE"), 0u);
  CHECK_EQ(install::setup_index(setups, "2/SETUP.EXE"), 2u);
  // A disc spells its own names however it likes, and so does a manifest.
  CHECK_EQ(install::setup_index(setups, "install.exe"), 1u);
  CHECK_EQ(install::setup_index(setups, "SETUP\\SETUP.EXE"), 3u);
  // Nothing named, or nothing that matches: the ranking stands, and the page
  // leaves its own choice alone rather than pointing at an entry at random.
  CHECK_EQ(install::setup_index(setups, ""), setups.size());
  CHECK_EQ(install::setup_index(setups, "DEMO.EXE"), setups.size());
  CHECK_EQ(install::setup_index(setups, "3/SETUP.EXE"), setups.size());
  // Both halves at once: disc two, and a setup a directory down on it. The
  // number is peeled off and the rest is left whole.
  CHECK_EQ(install::setup_index(setups, "2/SETUP/SETUP.EXE"), 3u);
}

// Arena III's real setup is Arena3/Setup.exe, and the disc it is on has no
// Setup.exe at its root at all.
//
// The wizard writes "<n>/<path on disc n>" and the install read everything
// before the first slash as n - so a setup one directory in lost that
// directory, and what ran was a path that is not on the disc. Only a leading
// run of digits is a number; the rest is a path, whatever it is spelled with.
static void test_setup_ref_split() {
  install::SetupRef r = install::split_setup_ref("2/Arena3/Setup.exe");
  CHECK_EQ(r.disc, 2u);
  CHECK_EQ(r.path, std::string("Arena3/Setup.exe"));

  // The case that was broken: no number at all, and a first component that
  // must survive.
  r = install::split_setup_ref("Arena3/Setup.exe");
  CHECK_EQ(r.disc, 0u);
  CHECK_EQ(r.path, std::string("Arena3/Setup.exe"));

  r = install::split_setup_ref("1/SETUP.EXE");
  CHECK_EQ(r.disc, 1u);
  CHECK_EQ(r.path, std::string("SETUP.EXE"));

  // A manifest names the path alone, a disc spells its separators how it
  // likes, and an installer_exe's setup is an absolute path on this machine.
  r = install::split_setup_ref("SETUP.EXE");
  CHECK_EQ(r.disc, 0u);
  CHECK_EQ(r.path, std::string("SETUP.EXE"));

  r = install::split_setup_ref("2\\Arena3\\Setup.exe");
  CHECK_EQ(r.disc, 2u);
  CHECK_EQ(r.path, std::string("Arena3/Setup.exe"));

  r = install::split_setup_ref("");
  CHECK_EQ(r.disc, 0u);
  CHECK_EQ(r.path, std::string(""));

  r = install::split_setup_ref("/home/me/Setup Classic.exe");
  CHECK_EQ(r.disc, 0u);
  CHECK_EQ(r.path, std::string("/home/me/Setup Classic.exe"));

  // And where that lands once the discs are mounted. mount_discs puts disc n
  // at work/drive-<'d'+n-1>, so disc 2 is drive-e and the path on it is joined
  // whole.
  install::Draft d;
  d.method = install::Draft::Method::Installer;
  d.setup = "2/Arena3/Setup.exe";
  CHECK_EQ(install::staged_setup("/w", d).generic_string(),
           std::string("/w/drive-e/Arena3/Setup.exe"));

  d.setup = "Arena3/Setup.exe";
  CHECK_EQ(install::staged_setup("/w", d).generic_string(),
           std::string("/w/drive-d/Arena3/Setup.exe"));

  d.setup = "1/SETUP.EXE";
  CHECK_EQ(install::staged_setup("/w", d).generic_string(),
           std::string("/w/drive-d/SETUP.EXE"));

  // Nothing is mounted for an installer somebody downloaded, and its setup is
  // already a path on this filesystem: it comes back untouched.
  d.method = install::Draft::Method::InstallerExe;
  d.setup = "/home/me/Setup Classic.exe";
  CHECK_EQ(install::staged_setup("/w", d).generic_string(),
           std::string("/home/me/Setup Classic.exe"));
}

// The combo shows one of the methods on offer; the page under it edits, and
// the Go button runs, draft_.method. A draft that arrives naming a method
// these sources cannot run - a manifest, a preset, or a source since removed -
// had the two disagree, silently, right up to the install.
static void test_method_clamp() {
  using M = install::Draft::Method;
  const std::vector<M> disc_only = {M::Installer, M::Copy, M::Unzip};

  // On offer: kept, whichever entry it is.
  CHECK(install::clamp_method(disc_only, M::Installer) == M::Installer);
  CHECK(install::clamp_method(disc_only, M::Unzip) == M::Unzip);

  // Not on offer: the first, which is what the combo would have shown anyway.
  CHECK(install::clamp_method(disc_only, M::InstallerExe) == M::Installer);

  // And the other way round. A .exe with no disc behind it can only be run,
  // so a draft that says "copy the files off the disc" is answered with the
  // one thing there is.
  const std::vector<M> exe_only = {M::InstallerExe};
  CHECK(install::clamp_method(exe_only, M::Copy) == M::InstallerExe);
  CHECK(install::clamp_method(exe_only, M::InstallerExe) == M::InstallerExe);

  // Nothing offered is not an answer, so nothing is changed.
  CHECK(install::clamp_method({}, M::Copy) == M::Copy);

  // What the page actually does, end to end: the sources are a disc set, the
  // draft came in naming the fourth method, and the entry the combo lands on
  // is the method that runs.
  std::vector<install::Source> disc(1);
  disc[0].kind = install::Source::Kind::DiscImage;
  disc[0].path = "arena3.iso";
  const std::vector<M> ways = install::methods_for(disc, 1);
  const M chosen = install::clamp_method(ways, M::InstallerExe);
  CHECK(std::find(ways.begin(), ways.end(), chosen) != ways.end());
}

// recipe.method is written into every pack, so its four strings are an on-disk
// format: pinned here, and each read back as the method it was written from.
// An empty method names nothing, and a string this build does not know is an
// installer disc, which is what draft_from_meta and apply_preset have always
// made of it.
static void test_method_strings() {
  using M = install::Draft::Method;
  CHECK_EQ(std::string(install::recipe_method(M::Installer)), std::string("wine_setup"));
  CHECK_EQ(std::string(install::recipe_method(M::InstallerExe)), std::string("installer_exe"));
  CHECK_EQ(std::string(install::recipe_method(M::Copy)), std::string("copy"));
  CHECK_EQ(std::string(install::recipe_method(M::Unzip)), std::string("unzip"));

  for (M m : {M::Installer, M::InstallerExe, M::Copy, M::Unzip}) {
    std::optional<M> back = install::method_from_recipe(install::recipe_method(m));
    CHECK(back.has_value() && *back == m);
  }

  CHECK(!install::method_from_recipe("").has_value());
  std::optional<M> bogus = install::method_from_recipe("bogus");
  CHECK(bogus.has_value() && *bogus == M::Installer);
}

// install::run's rule for where a recipe's game landed: the first candidate,
// in ranked order, holding every verify entry at once, found in any case.
static void test_find_install_dir(const fs::path& tmp) {
  fs::path c = tmp / "find-install-dir" / "drive_c";
  write_file(c / "Games" / "Arena3" / "Data" / "saves.dat", 3);
  write_file(c / "Games" / "Arena3" / "ARENA3.EXE", 3);
  write_file(c / "Games" / "Arena3" / "maps.pak", 3);

  std::vector<install::Candidate> cands(3);
  cands[0].dir = "Games/Arena3/Data";
  cands[1].dir = "Games/Arena3";
  cands[2].dir = "Games";

  // The deeper directory holds one entry and not the other, so it is passed
  // over for the one that holds both.
  CHECK_EQ(install::find_install_dir(cands, c, {"arena3.exe", "maps.pak"}).generic_string(),
           std::string("Games/Arena3"));
  // Nothing holds a file that is not there.
  CHECK(install::find_install_dir(cands, c, {"arena3.exe", "missing.dat"}).empty());
  // And with no list there is nothing to hold, so no answer.
  CHECK(install::find_install_dir(cands, c, {}).empty());
}

// A copy or an unzip runs no installer, writes nothing to C: and leaves no
// diff to rank, so "where did it land" is answered by the method rather than
// by looking at drive_c. The wizard's step 5 reads candidates() either way,
// which is why this has to produce one rather than nothing.
static void test_survey_extracted(const fs::path& tmp) {
  rt::Env e;
  auto quiet = [](const std::string&) {};
  std::error_code ec;
  fs::path work = tmp / "staging" / "install-copy";

  install::Build b(e, work, quiet);
  // Before anything is extracted, the game is wherever an installer would put
  // it: drive_c. This is the path step 6 measures an executable against.
  CHECK_EQ(b.installed_root().generic_string(),
           (work / "prefix" / "drive_c").generic_string());
  CHECK_THROWS(b.survey_extracted());

  fs::path tree = work / "tree";
  write_file(tree / "CLASSIC.EXE", 900);
  write_file(tree / "UNINST.EXE", 4000);
  write_file(tree / "MAIN.DAT", 70000);
  write_file(tree / "DATA" / "SOUND" / "intro.acm", 500);

  b.survey_extracted();
  CHECK_EQ(b.installed_root().generic_string(), tree.generic_string());
  // One candidate, and its directory is empty: the tree is the game, and
  // there is nothing under it to choose between.
  CHECK_EQ(b.candidates().size(), 1u);
  CHECK(b.candidates()[0].dir.empty());
  CHECK_EQ(b.candidates()[0].files, 4u);
  CHECK_EQ(b.candidates()[0].bytes, 75400u);
  // Ranked the same way an installed directory is, which is what makes step 6
  // work unchanged: an uninstaller is last whatever its size.
  CHECK_EQ(b.candidates()[0].executables.size(), 2u);
  CHECK_EQ(b.candidates()[0].executables[0].generic_string(), std::string("CLASSIC.EXE"));
  CHECK_EQ(b.candidates()[0].executables[1].generic_string(), std::string("UNINST.EXE"));

  // An empty tree is not an install. It is the copy that took nothing - a
  // subdirectory that matched no files - and it says so rather than offering
  // a candidate of nought files.
  install::Build empty(e, tmp / "staging" / "install-empty", quiet);
  fs::create_directories(tmp / "staging" / "install-empty" / "tree", ec);
  CHECK_THROWS(empty.survey_extracted());

  fs::remove_all(tmp / "staging", ec);
}

// The whole of a copy install, from the extracted tree to the pack: games that
// arrive this way - already installed on their disc, or zipped on it - could
// not be built at all while step 5 depended on a diff that never happened.
static void test_copy_install(const fs::path& tmp) {
  rt::Env e;
  auto quiet = [](const std::string&) {};
  std::error_code ec;

  fs::path stub = tmp / "fake-dwarfs-copy";
  {
    std::ofstream f(stub);
    f << "#!/bin/sh\n"
      << "while [ $# -gt 0 ]; do\n"
      << "  if [ \"$1\" = -o ]; then : > \"$2\"; exit 0; fi\n"
      << "  shift\n"
      << "done\n"
      << "exit 1\n";
  }
  fs::permissions(stub, fs::perms::owner_all, ec);
  ::setenv("KRETRO_DWARFS", stub.c_str(), 1);

  fs::path work = tmp / "staging" / "install-example";
  install::Build b(e, work, quiet);
  // copy_from_disc extracts the chosen directory *as* the staging tree, so
  // this is a disc whose PC directory has already come off it: the game, at
  // the top, with no disc structure above it.
  write_file(work / "tree" / "game.exe", 1200);
  write_file(work / "tree" / "graphics.res", 50000);

  b.survey_extracted();

  install::Draft d;
  d.id = "copy-install-test";
  d.name = "Example G.A.M.E.";
  d.method = install::Draft::Method::Copy;
  d.subdir = "pc";
  // What step 5 hands on: the candidate's directory, which for a copy is the
  // tree itself and therefore empty. Nothing is renamed out of drive_c.
  d.install_dir = b.candidates()[0].dir;
  d.exe = b.candidates()[0].executables[0];
  CHECK_EQ(d.exe.generic_string(), std::string("game.exe"));

  // The verify list is read off what came off the disc, not off a drive_c
  // this method never touched. Read here rather than after the write, because
  // lay_out_body consumes the tree: by the time there is a pack there is no
  // longer anything to read it from, which is exactly why write(Draft) takes
  // it before laying anything out.
  std::vector<std::string> v = install::verify_list(b.installed_root() / d.install_dir, d.exe);
  CHECK_EQ(v.size(), 2u);
  CHECK_EQ(v[0], std::string("game.exe"));
  CHECK_EQ(v[1], std::string("graphics.res"));

  install::Result res = b.write(d);
  CHECK(fs::exists(res.pack, ec));
  CHECK(fs::exists(work / "body" / "game" / "game.exe"));
  CHECK(res.root == Tree::from_directory(work / "body" / "game").root());

  // The recipe that went into it. The pack itself cannot be opened here - the
  // stub above writes an empty body and Pack::open refuses one - so this is
  // the same compilation write(Draft) does, checked directly.
  Meta m = install::draft_to_meta(d, {});
  CHECK_EQ(m.recipe.method, std::string("copy"));
  CHECK_EQ(m.recipe.subdir, std::string("pc"));
  CHECK(m.install.install_dir.empty());
  // And write() throws when a verify entry is not under the tree, so the
  // install above completing is the other half of that list.

  ::unsetenv("KRETRO_DWARFS");
  fs::remove(games_dir() / "copy-install-test.kgpack", ec);
  fs::remove_all(tmp / "staging", ec);
}

// The serial a rebuild shows beside the installer asking for it.
//
// A pack carries no serial - that is the policy test_draft_to_meta ends on -
// so a recipe arrives with an empty draft, and a rebuild goes straight to the
// install step without passing the page that would look one up. This is the
// only thing that puts it there.
static void test_prefill_serial(const fs::path& tmp) {
  fs::path vault = tmp / "vault" / "keys.txt";
  std::vector<install::StoredKey> keys;
  install::put_key(keys, "adventure-ii", "ABCD-1234-EFGH-5678", "typed into the wizard");
  install::save_keys(vault, keys);
  std::vector<install::StoredKey> stored = install::load_keys(vault);

  install::Draft d;
  d.id = "adventure-ii";
  CHECK(install::prefill_serial(d, stored));
  CHECK_EQ(d.serial, std::string("ABCD-1234-EFGH-5678"));
  // Twice is not a second answer.
  CHECK(!install::prefill_serial(d, stored));

  // A serial already in the draft is the one the person is looking at, and the
  // vault does not get to overrule it.
  install::Draft typed;
  typed.id = "adventure-ii";
  typed.serial = "WXYZ-0000";
  CHECK(!install::prefill_serial(typed, stored));
  CHECK_EQ(typed.serial, std::string("WXYZ-0000"));

  // A game nobody has stored a key for, and a draft with no id yet: neither
  // invents anything. There is no generator here and never will be.
  install::Draft unknown;
  unknown.id = "arena3";
  CHECK(!install::prefill_serial(unknown, stored));
  CHECK(unknown.serial.empty());
  install::Draft nameless;
  nameless.serial.clear();
  CHECK(!install::prefill_serial(nameless, stored));
  CHECK(nameless.serial.empty());

  // And through keys_file(), which is what the wizard calls: what `kretro key`
  // wrote is what a rebuild puts on screen.
  std::vector<install::StoredKey> real = install::load_keys(install::keys_file());
  install::put_key(real, "classic2", "0000-1111-2222", "kretro key");
  install::save_keys(install::keys_file(), real);
  install::Draft rebuild;
  rebuild.id = "classic2";
  CHECK(install::prefill_serial(rebuild, install::load_keys(install::keys_file())));
  CHECK_EQ(rebuild.serial, std::string("0000-1111-2222"));

  std::error_code ec;
  fs::remove(install::keys_file(), ec);
  fs::remove_all(tmp / "vault", ec);
}

int main() {
  fs::path tmp = fs::temp_directory_path() / "kretro-test-wizard";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  // state_dir() caches its answer in a function-local static (util/paths.cpp),
  // so this has to happen before anything at all asks where state lives.
  ::setenv("KRETRO_STATE", (tmp / "state").c_str(), 1);
  // Build::write puts the finished pack in games_dir(), and nothing below it
  // creates that directory: in the app it is gui::scan that does, and on
  // the CLI it is install::run. A test binary is neither, so it does it here.
  ensure_state_dirs();

  try {
    test_classify(tmp);
    test_slug();
    test_candidate_ranking();
    test_executable_ranking(tmp);
    test_verify_list(tmp);
    test_draft_to_meta();
    test_staging_layout();
    test_draft_verify(tmp);
    test_preset_match(tmp);
    test_staging_lifetime(tmp);
    test_rehome(tmp);
    test_open_sources(tmp);
    test_swap_disc(tmp);
    test_mounted_snapshot(tmp);
    test_file_counter(tmp);
    test_diff_after(tmp);
    test_write_draft_verify(tmp);
    test_root_advisory(tmp);
    test_id_clash(tmp);
    test_draft_from_meta(tmp);
    test_setup_ref_round_trip();
    test_bare_exe_sources(tmp);
    test_exe_preselection();
    test_a_recipe_does_not_pick_your_files();
    test_a_digit_directory_is_not_a_disc();
    test_setup_preselection();
    test_setup_ref_split();
    test_method_clamp();
    test_method_strings();
    test_find_install_dir(tmp);
    test_survey_extracted(tmp);
    test_copy_install(tmp);
    test_prefill_serial(tmp);
  } catch (const std::exception& e) {
    kgtest::unexpected(e);
  }

  fs::remove_all(tmp);
  return kgtest::finish();
}
