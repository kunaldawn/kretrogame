// Tier 1 unit tests for the player: where it keeps its state, what it
// remembers, how a game's prefix is made and upgraded, the desktop entry, the
// command line, an author's key in the registry - and a real player file,
// built from small fake packs, opened and checked the way a launch checks it.
//
// No Wine, no display, no FUSE. Every decision that would start one is a pure
// function and tested as such; the files it makes are made under a temporary
// directory and read back.
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bundle/build.h"
#include "rt/env.h"
#include "player/cli.h"
#include "player/desktop.h"
#include "player/player.h"
#include "player/prefix.h"
#include "player/state.h"
#include "util/paths.h"
#include "util/proc.h"

namespace fs = std::filesystem;
using namespace kg;
using player::Command;
using player::PrefixAction;

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

#define CHECK_THROWS(expr)                                                             \
  do {                                                                                 \
    ++checks;                                                                          \
    bool threw_ = false;                                                               \
    try {                                                                              \
      (void)(expr);                                                                    \
    } catch (const std::exception&) {                                                  \
      threw_ = true;                                                                   \
    }                                                                                  \
    if (!threw_) {                                                                     \
      ++failures;                                                                      \
      std::fprintf(stderr, "  FAIL %s:%d  did not throw: %s\n", __FILE__, __LINE__, #expr); \
    }                                                                                  \
  } while (0)

static bool contains(const std::string& s, const std::string& what) {
  return s.find(what) != std::string::npos;
}

static void section(const char* name) { std::fprintf(stderr, "%s\n", name); }

static void write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << s;
}

static std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---- state ------------------------------------------------------------------

static void test_state_choice(const fs::path& tmp) {
  section("state: portable beside the file, else XDG, and a read-only portable falls back");
  fs::path dir = tmp / "stick";
  fs::path exe = dir / "classics.run";
  write(exe, "ELF");

  // No portable directory: XDG_DATA_HOME, keyed by the bundle id.
  player::StateChoice c = player::choose_state(exe, "retro-shelf", "/xdg/data", "/home/kim");
  CHECK_EQ(c.dir, fs::path("/xdg/data/retro-shelf"));
  CHECK(!c.portable);
  CHECK(c.refused_portable.empty());
  CHECK(c.private_root.empty());

  // No XDG_DATA_HOME: ~/.local/share. A relative one is invalid by the spec
  // and ignored the same way.
  CHECK_EQ(player::choose_state(exe, "retro-shelf", "", "/home/kim").dir,
           fs::path("/home/kim/.local/share/retro-shelf"));
  CHECK_EQ(player::choose_state(exe, "retro-shelf", "rel/data", "/home/kim").dir,
           fs::path("/home/kim/.local/share/retro-shelf"));

  // The portable directory is named for the file, not the bundle.
  CHECK_EQ(player::portable_dir(exe), dir / "classics.run-data");
  fs::create_directories(dir / "classics.run-data");
  c = player::choose_state(exe, "retro-shelf", "/xdg/data", "/home/kim");
  CHECK_EQ(c.dir, dir / "classics.run-data");
  CHECK(c.portable);

  // A file of that name is not a directory, and is not taken for one.
  fs::path exe2 = dir / "other.run";
  write(dir / "other.run-data", "not a directory");
  CHECK_EQ(player::choose_state(exe2, "b", "/x", "/h").dir, fs::path("/x/b"));

  // Read-only: XDG, and the refused directory is named so it can be said.
  fs::permissions(dir / "classics.run-data", fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace);
  if (geteuid() == 0) {
    std::fprintf(stderr, "  (running as root: a read-only directory is still writable; skipped)\n");
  } else {
    c = player::choose_state(exe, "retro-shelf", "/xdg/data", "/home/kim");
    CHECK_EQ(c.dir, fs::path("/xdg/data/retro-shelf"));
    CHECK(!c.portable);
    CHECK_EQ(c.refused_portable, dir / "classics.run-data");
    CHECK_EQ(c.refused_why, std::string("cannot be written"));
    CHECK(!player::writable_dir(dir / "classics.run-data"));
  }
  fs::permissions(dir / "classics.run-data", fs::perms::owner_all, fs::perm_options::replace);
  CHECK(player::writable_dir(dir / "classics.run-data"));
  // A prefix is made of symbolic links, so the portable directory has to
  // hold them; FAT and exFAT cannot, and that is asked the same way, by
  // making one. (Mounting a FAT image to see the refusal needs root.)
  CHECK(player::holds_links(dir / "classics.run-data"));
  CHECK(!player::holds_links(dir / "no-such-directory"));
  // The probes leave nothing behind.
  CHECK(fs::is_empty(dir / "classics.run-data"));

  // Somebody else's: a player run from /tmp takes a -data beside it that
  // another user could have made first, with a prefix and a HOME for the game
  // in it. Asked as another user, since this one cannot make a directory that
  // is not its own.
  c = player::choose_state(exe, "retro-shelf", "/xdg/data", "/home/kim", getuid() + 1);
  CHECK_EQ(c.dir, fs::path("/xdg/data/retro-shelf"));
  CHECK(!c.portable);
  CHECK_EQ(c.refused_portable, dir / "classics.run-data");
  CHECK_EQ(c.refused_why, std::string("belongs to another user"));

  // No home at all: under /tmp, in a directory that has to be made ours.
  c = player::choose_state(exe2, "retro-shelf", "", "");
  CHECK_EQ(c.private_root, fs::path("/tmp") / ("kretro-" + std::to_string(getuid())));
  CHECK_EQ(c.dir, c.private_root / "retro-shelf");
  fs::path shared = tmp / "shared-tmp" / "kretro-me";
  fs::create_directories(shared);
  fs::permissions(shared, fs::perms::all, fs::perm_options::replace);
  player::claim_private_dir(shared);
  CHECK((fs::status(shared).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
  CHECK_THROWS(player::claim_private_dir(shared, getuid() + 1));
  fs::path made = tmp / "shared-tmp" / "kretro-new";
  player::claim_private_dir(made);
  CHECK(fs::is_directory(made));
  // A link there is somebody pointing the saves elsewhere, whatever it points at.
  fs::create_directory_symlink(made, tmp / "shared-tmp" / "kretro-link");
  CHECK_THROWS(player::claim_private_dir(tmp / "shared-tmp" / "kretro-link"));
}

static void test_bundle_layout() {
  section("paths: a player keeps each game in a directory of its own");
  // KRETRO_STATE was set by main before anything asked.
  const fs::path st = state_dir();
  CHECK(!bundle_layout());
  CHECK_EQ(game_saves_dir("example"), st / "saves" / "example");
  CHECK_EQ(game_prefix_dir("example"), st / "prefixes" / "example");
  CHECK_EQ(game_mount_dir("example"), st / "saves" / "example");
  CHECK_EQ(game_extract_dir("example"), st / "extracted" / "example");

  use_bundle_layout("classics");
  CHECK(bundle_layout());
  CHECK_EQ(game_saves_dir("example"), st / "example" / "saves");
  CHECK_EQ(game_prefix_dir("example"), st / "example" / "prefix");
  CHECK_EQ(game_home_dir("example"), st / "example" / "home");
  // Mount points in the session's runtime directory, never in the state -
  // which may be on a stick fusermount3 is not allowed to mount on.
  // And keyed by the state as well as the bundle: two copies of one bundle
  // with states of their own must not share mount points, or each one's
  // start-up sweep unmounts the other's running game.
  CHECK_EQ(game_mount_dir("example"),
           fs::path(std::getenv("XDG_RUNTIME_DIR")) / "kretro" / ("classics-" + state_key(st)) / "example");
  CHECK_EQ(state_key(st).size(), size_t{8});
  CHECK_EQ(state_key("/media/kim/STICK/classics.run-data"), state_key("/media/kim/STICK/classics.run-data/"));
  CHECK(state_key("/media/kim/STICK/classics.run-data") != state_key("/home/kim/.local/share/classics"));
  // A fixed answer, so a build that changed how it is worked out would fail
  // here rather than orphan the mounts an older build left.
  CHECK_EQ(state_key("/home/kim/.local/share/classics"), std::string("f1ed8400"));
  CHECK_EQ(game_extract_dir("example"),
           fs::path(std::getenv("XDG_CACHE_HOME")) / "kretro" / ("classics-" + state_key(st)) / "example");
  CHECK_EQ(session::extraction_stamp_file(game_extract_dir("example")),
           fs::path(std::getenv("XDG_CACHE_HOME")) / "kretro" / ("classics-" + state_key(st)) / "example.stamp");
  CHECK_EQ(session::lock_file("example"), st / "example" / "saves" / "lock");
  use_bundle_layout("");
  CHECK(!bundle_layout());
}

static void test_launcher_and_settings(const fs::path& tmp) {
  section("state: launcher.toml and settings.toml say what they were told");
  fs::path f = tmp / "launcher" / "launcher.toml";
  player::LauncherState s = player::load_launcher(f);  // nothing there: defaults
  CHECK(!s.desktop_offered);
  CHECK(s.warnings_seen.empty());
  s.window_w = 1600;
  s.window_h = 900;
  s.desktop_offered = true;
  s.portable_fallback_said = true;
  // A warning with every character the file format cares about in it.
  const std::string line = "No \"hidraw\" access, [see] /dev/hidraw0 # really, it's fine\\";
  s.warnings_seen.insert(player::warning_key(line));
  player::save_launcher(f, s);
  player::LauncherState back = player::load_launcher(f);
  CHECK_EQ(back.window_w, 1600u);
  CHECK_EQ(back.window_h, 900u);
  CHECK(back.desktop_offered);
  CHECK(!back.desktop_installed);
  CHECK(back.portable_fallback_said);
  CHECK(back.warnings_seen.count(player::warning_key(line)) == 1);
  CHECK(back.warnings_seen.count(player::warning_key(line + " ")) == 0);
  CHECK(!fs::exists(f.string() + ".tmp"));

  // A file somebody broke is a fresh start, not a player that will not open.
  write(f, "[window\nwidth = = =\n");
  CHECK(!player::load_launcher(f).desktop_offered);
  write(f, "[window]\nwidth = 5\n");  // absurd: the default, not a 5-pixel window
  CHECK_EQ(player::load_launcher(f).window_w, 1280u);

  // Settings: the author's defaults until the person changes one.
  player::GameSettings d = player::default_settings("fit", true);
  CHECK(d.display.mode == config::ScaleMode::Fit);
  CHECK(d.fullscreen);
  CHECK_EQ(d.gamepad, std::string("author"));
  fs::path sf = tmp / "g" / "settings.toml";
  CHECK(player::load_game_settings(sf, d).display.mode == config::ScaleMode::Fit);
  player::GameSettings mine = d;
  mine.display.mode = config::ScaleMode::Native;
  mine.display.scale = 3;
  mine.fullscreen = false;
  mine.gamepad = "kretro";
  player::save_game_settings(sf, mine);
  player::GameSettings got = player::load_game_settings(sf, d);
  CHECK(got.display.mode == config::ScaleMode::Native);
  CHECK_EQ(got.display.scale, 3u);
  CHECK(!got.fullscreen);
  CHECK_EQ(got.gamepad, std::string("kretro"));
  write(sf, "[controls]\ngamepad = \"joystick-of-legend\"\n");
  CHECK_EQ(player::load_game_settings(sf, d).gamepad, std::string("author"));

  // Two copies of a player on one state write the same files - two
  // launchers, a launcher and a terminal - and each has to leave a whole
  // file and not be told it could not write. They shared one <file>.tmp.
  fs::path shared = tmp / "together" / "launcher.toml";
  std::atomic<int> refused{0};
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back([&, w] {
      player::LauncherState ls;
      ls.window_w = 1000 + static_cast<uint32_t>(w);
      for (int i = 0; i < 300; ++i) {
        try {
          player::save_launcher(shared, ls);
        } catch (const std::exception&) {
          ++refused;
        }
      }
    });
  }
  for (std::thread& t : writers) t.join();
  CHECK_EQ(refused.load(), 0);
  const uint32_t w = player::load_launcher(shared).window_w;
  CHECK(w >= 1000 && w < 1004);
  // Nothing but the file itself is left.
  size_t left = 0;
  for (const fs::directory_entry& de : fs::directory_iterator(shared.parent_path())) {
    (void)de;
    ++left;
  }
  CHECK_EQ(left, size_t{1});
}

static void test_warnings_once(const fs::path& tmp) {
  section("state: a warning of the silent check is said once, by the launcher or by play");
  player::doctor::Report rep;
  using S = gpu::Problem::Severity;
  rep.problems.push_back({S::Blocking, "No Wayland or X11 display", "Run this from a desktop session"});
  rep.problems.push_back({S::Warning, "No PulseAudio or PipeWire sound server is running for you", "Start one"});
  rep.problems.push_back({S::Warning, "NVIDIA 590.48 libraries, 595.84 kernel module", "Reinstall the driver"});
  fs::path f = tmp / "warned" / "launcher.toml";
  player::LauncherState ls = player::load_launcher(f);
  std::vector<std::string> said = player::unseen_warnings(rep, ls);
  CHECK_EQ(said.size(), size_t{2});
  CHECK(!said.empty() && contains(said[0], "sound server"));
  // A blocking problem is not a warning: it is said every time, as it stops.
  for (const std::string& s : said) CHECK(!contains(s, "No Wayland"));
  player::save_launcher(f, ls);
  // The next start - from either - says neither again.
  player::LauncherState again = player::load_launcher(f);
  CHECK(player::unseen_warnings(rep, again).empty());
  rep.problems.push_back({S::Warning, "/dev/hidraw0 cannot be read", "Add a udev rule"});
  CHECK_EQ(player::unseen_warnings(rep, again).size(), size_t{1});
}

static void test_memo(const fs::path& tmp) {
  section("memo: a pack is checked once per bundle version, and a changed pack again");
  fs::path memo = tmp / "memo" / "verified";
  Hash a = hash_string("pack a"), b = hash_string("pack b");
  CHECK(!player::pack_verified(memo, "example", "1.0", a));
  player::remember_verified(memo, "example", "1.0", a);
  CHECK(player::pack_verified(memo, "example", "1.0", a));
  CHECK(!player::pack_verified(memo, "example", "1.1", a));      // a new version checks again
  CHECK(!player::pack_verified(memo, "example", "1.0", b));      // so does a new pack
  CHECK(!player::pack_verified(memo, "classic2", "1.0", a)); // and another game
  // A version with a space in it is still one field.
  player::remember_verified(memo, "example", "1.0 beta", b);
  CHECK(player::pack_verified(memo, "example", "1.0 beta", b));
  CHECK(!player::pack_verified(memo, "example", "1.0", b));
  CHECK(player::pack_verified(memo, "example", "1.0", a));  // older lines kept
  // Remembering twice is one line.
  player::remember_verified(memo, "example", "1.0", a);
  std::string text = slurp(memo);
  CHECK_EQ(static_cast<int>(std::count(text.begin(), text.end(), '\n')), 2);
  // Bounded.
  for (int i = 0; i < 300; ++i) player::remember_verified(memo, "g", std::to_string(i), a);
  text = slurp(memo);
  CHECK(std::count(text.begin(), text.end(), '\n') <= 256);
  CHECK(player::pack_verified(memo, "g", "299", a));
}

// ---- the prefix ---------------------------------------------------------------

static void test_prefix_decision() {
  section("prefix: seed, upgrade or leave, from the two stamps");
  using O = std::optional<std::string>;
  CHECK(player::prefix_action(O("wine-11.18"), O(), false) == PrefixAction::Seed);
  CHECK(player::prefix_action(O("wine-11.18"), O("wine-11.18"), true) == PrefixAction::Ready);
  CHECK(player::prefix_action(O("wine-11.19"), O("wine-11.18"), true) == PrefixAction::Upgrade);
  // Never re-seeded: a newer Wine upgrades, an older one too - the prefix is
  // the game's, the template is only where it started.
  CHECK(player::prefix_action(O("wine-11.17"), O("wine-11.18"), true) == PrefixAction::Upgrade);
  // A prefix nobody stamped was made some other way, and is left alone.
  CHECK(player::prefix_action(O("wine-11.18"), O(), true) == PrefixAction::Ready);
  // No template to seed from or compare with.
  CHECK(player::prefix_action(O(), O(), false) == PrefixAction::Ready);
  CHECK(player::prefix_action(O(), O("wine-11.18"), true) == PrefixAction::Ready);
}

static void test_prefix_seed(const fs::path& tmp) {
  section("prefix: a seed is a writable copy with c: and no z:");
  fs::path t = tmp / "template";
  write(t / ".kretro-wine-version", "wine-11.18 (Staging)\n");
  write(t / "system.reg", "WINE REGISTRY Version 2\n[Software] 1\n");
  write(t / "user.reg", "WINE REGISTRY Version 2\n");
  write(t / "drive_c" / "windows" / "system32" / "kernel32.dll", std::string(5000, 'k'));
  fs::create_directories(t / "dosdevices");
  fs::create_directory_symlink("../drive_c", t / "dosdevices" / "c:");
  fs::create_directory_symlink("/", t / "dosdevices" / "z:");
  // The runtime's copy is read-only; its seed must not be.
  fs::permissions(t / "system.reg", fs::perms::owner_read, fs::perm_options::replace);
  fs::permissions(t / "drive_c" / "windows" / "system32", fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace);

  CHECK_EQ(player::read_stamp(t).value_or(""), std::string("wine-11.18 (Staging)"));
  CHECK(!player::read_stamp(tmp / "nowhere"));

  fs::path p = tmp / "state" / "example" / "prefix";
  player::seed_prefix(t, p);
  CHECK_EQ(player::read_stamp(p).value_or(""), std::string("wine-11.18 (Staging)"));
  CHECK_EQ(slurp(p / "drive_c" / "windows" / "system32" / "kernel32.dll"), std::string(5000, 'k'));
  CHECK(fs::is_symlink(p / "dosdevices" / "c:"));
  CHECK_EQ(fs::read_symlink(p / "dosdevices" / "c:"), fs::path("../drive_c"));
  CHECK(!fs::exists(fs::symlink_status(p / "dosdevices" / "z:")));
  CHECK(player::writable_dir(p / "drive_c" / "windows" / "system32"));
  CHECK((fs::status(p / "system.reg").permissions() & fs::perms::owner_write) != fs::perms::none);
  CHECK(!fs::exists(p.string() + ".seeding"));
  fs::permissions(t / "drive_c" / "windows" / "system32", fs::perms::owner_all, fs::perm_options::replace);

  // What the game made there, then an upgrade's snapshot of it.
  write(p / "drive_c" / "users" / "player" / "Documents" / "save1.dat", "level 7");
  write(p / "user.reg", "WINE REGISTRY Version 2\n[Software\\\\Example Publisher] 2\n\"Volume\"=\"9\"\n");
  fs::path snap = player::snapshot_prefix(p, tmp / "state" / "example" / "prefix-before");
  CHECK(contains(snap.filename().string(), "wine-11.18"));
  CHECK_EQ(slurp(snap / "drive_c" / "users" / "player" / "Documents" / "save1.dat"), std::string("level 7"));
  CHECK(contains(slurp(snap / "user.reg"), "Volume"));
  CHECK(fs::exists(snap / ".kretro-wine-version"));
  // The whole of drive_c/windows is Wine's to rewrite and is not copied.
  CHECK(!fs::exists(snap / "drive_c" / "windows"));
  // A copy, not a link: an upgrade rewriting the prefix leaves it as it was.
  write(p / "user.reg", "rewritten");
  CHECK(contains(slurp(snap / "user.reg"), "Volume"));
}

static void test_key_registry() {
  section("key: an author's key becomes one REGEDIT4 value where the game reads it");
  bundle::GameMeta::Key k;
  k.value = "1234-5678-9ABC";
  k.registry_path = "HKLM\\Software\\Example Publisher\\EXAMPLE";
  k.registry_value = "CDKey";
  std::string r = player::key_registry(k);
  CHECK_EQ(r, std::string("REGEDIT4\n\n[HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\EXAMPLE]\n\"CDKey\"=\"1234-5678-9ABC\"\n"));

  k.registry_path = "HKEY_CURRENT_USER/Software/Example/";
  CHECK(contains(player::key_registry(k), "[HKEY_CURRENT_USER\\Software\\Example]\n"));
  k.registry_path = "hkcu\\Software\\X";
  CHECK(contains(player::key_registry(k), "[HKEY_CURRENT_USER\\Software\\X]"));
  // No hive named: where the installers of the era kept their keys.
  k.registry_path = "Software\\Example Publisher\\Adventure II";
  CHECK(contains(player::key_registry(k), "[HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\Adventure II]"));
  // Quotes and backslashes in the value are escaped, not left to end it.
  k.value = "AB\"CD\\EF";
  CHECK(contains(player::key_registry(k), "\"CDKey\"=\"AB\\\"CD\\\\EF\"\n"));
  // The default value.
  k.registry_value = "@";
  CHECK(contains(player::key_registry(k), "\n@=\""));

  // The 32-bit view: HKLM\Software goes through Wow6432Node, since that is
  // where a 32-bit game in a 64-bit prefix reads it.
  bundle::GameMeta::Key v = k;
  v.registry_value = "CDKey";
  v.view = "32";
  v.registry_path = "HKLM\\Software\\Example Publisher\\EXAMPLE";
  CHECK(contains(player::key_registry(v), "[HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Example Publisher\\EXAMPLE]\n"));
  v.registry_path = "Software\\Example Publisher\\EXAMPLE";  // no hive: HKLM, the same
  CHECK(contains(player::key_registry(v), "[HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Example Publisher\\EXAMPLE]\n"));
  v.registry_path = "HKLM\\SOFTWARE\\Wow6432Node\\Example Publisher\\EXAMPLE";  // already there
  CHECK(contains(player::key_registry(v), "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Example Publisher\\EXAMPLE]\n"));
  v.registry_path = "HKCU\\Software\\Example Publisher\\EXAMPLE";  // shared by both views
  CHECK(contains(player::key_registry(v), "[HKEY_CURRENT_USER\\Software\\Example Publisher\\EXAMPLE]\n"));
  v.registry_path = "HKLM\\Software\\Classes\\EXAMPLE";
  CHECK(contains(player::key_registry(v), "[HKEY_LOCAL_MACHINE\\Software\\Classes\\EXAMPLE]\n"));
  v.view = "64";
  v.registry_path = "HKLM\\Software\\Example Publisher\\EXAMPLE";
  CHECK(contains(player::key_registry(v), "[HKEY_LOCAL_MACHINE\\Software\\Example Publisher\\EXAMPLE]\n"));

  bundle::GameMeta::Key bad = k;
  bad.registry_path = "";
  CHECK_THROWS(player::key_registry(bad));
  bad = k;
  bad.registry_path = "HKLM";
  CHECK_THROWS(player::key_registry(bad));
  bad = k;
  bad.registry_value = "";
  CHECK_THROWS(player::key_registry(bad));
  bad = k;
  bad.value = "12\n[HKEY_LOCAL_MACHINE\\Evil]";
  CHECK_THROWS(player::key_registry(bad));
}

static void test_extra_files(const fs::path& tmp) {
  section("extra files: beside the game, once, with overrides for the DLLs");
  fs::path game = tmp / "game-dir";
  fs::create_directories(game);
  std::vector<bundle::GameMeta::Dll> files = {{"DDraw.dll", "MZ ddraw"}, {"D3D8.dll", "MZ d3d8"},
                                              {"dgVoodoo.conf", "[General]"}};
  CHECK_EQ(player::place_extra_files(files, game), std::string("ddraw=n,b;d3d8=n,b"));
  CHECK_EQ(slurp(game / "DDraw.dll"), std::string("MZ ddraw"));
  CHECK_EQ(slurp(game / "dgVoodoo.conf"), std::string("[General]"));
  // Placed again unchanged: not rewritten, so an overlay does not see a write.
  auto before = fs::last_write_time(game / "DDraw.dll");
  ::usleep(20000);
  player::place_extra_files(files, game);
  CHECK(fs::last_write_time(game / "DDraw.dll") == before);
  // Changed: rewritten.
  files[0].data = "MZ ddraw v2";
  player::place_extra_files(files, game);
  CHECK_EQ(slurp(game / "DDraw.dll"), std::string("MZ ddraw v2"));
  // A name that is a path is not followed.
  CHECK_EQ(player::place_extra_files({{"../escape.dll", "x"}}, game), std::string());
  CHECK(!fs::exists(tmp / "escape.dll"));

  // Where the executable is, as the tree spells it, whatever case run.exe was
  // typed in.
  Meta m;
  m.run.exe = "bin\\game.exe";
  fs::path tree = tmp / "exe-tree";
  write(tree / "Bin" / "GAME.EXE", "MZ");
  write(tree / "readme.txt", "x");
  m.tree = Tree::from_directory(tree);
  CHECK_EQ(player::exe_dir_in_tree(m), std::string("Bin"));
  m.run.exe = "readme.txt";
  CHECK_EQ(player::exe_dir_in_tree(m), std::string());
  m.run.exe = "../../x.exe";
  CHECK_EQ(player::exe_dir_in_tree(m), std::string());

  // The layer: beside the executable, and exactly the bundle's files - what a
  // newer build dropped goes, and so does a copy where the exe used to be.
  fs::path layer = tmp / "extra-layer";
  write(layer / "old.dll", "gone");
  CHECK_EQ(player::stage_extra_layer(files, "Bin", layer), std::string("ddraw=n,b;d3d8=n,b"));
  CHECK_EQ(slurp(layer / "Bin" / "DDraw.dll"), std::string("MZ ddraw v2"));
  CHECK(!fs::exists(layer / "old.dll"));
  CHECK_EQ(player::stage_extra_layer(files, "", layer), std::string("ddraw=n,b;d3d8=n,b"));
  CHECK(fs::exists(layer / "DDraw.dll"));
  CHECK(!fs::exists(layer / "Bin" / "DDraw.dll"));
  // None: no layer at all.
  CHECK_EQ(player::stage_extra_layer({}, "Bin", layer), std::string());
  CHECK(!fs::exists(layer));
}

static void test_gamepad_bindings() {
  section("gamepad: the pack's own map, and the author's over it when the controls say so");
  bundle::GameMeta g;
  g.gamepad = "a=space\nstart=p\n";
  std::map<std::string, std::string> own = {{"a", "Return"}, {"b", "Escape"}};
  player::GameSettings s;
  CHECK_EQ(s.gamepad, std::string("author"));
  auto got = player::gamepad_bindings(own, g, s);
  CHECK_EQ(got["a"], std::string("space"));
  CHECK_EQ(got["b"], std::string("Escape"));
  CHECK_EQ(got["start"], std::string("p"));
  s.gamepad = "kretro";
  CHECK((player::gamepad_bindings(own, g, s) == own));
  g.gamepad.clear();
  s.gamepad = "author";
  CHECK((player::gamepad_bindings(own, g, s) == own));
}

static void test_profile_adoption(const fs::path& tmp) {
  section("profile: an old kretro prefix's C:\\users\\<login> becomes C:\\users\\player");
  fs::path p = tmp / "old-prefix";
  write(p / "drive_c" / "users" / "kim" / "Documents" / "save.dat", "level 7");
  fs::create_directories(p / "drive_c" / "users" / "Public");
  std::string said = session::adopt_player_profile(p);
  CHECK(contains(said, "C:\\users\\kim"));
  CHECK_EQ(slurp(p / "drive_c" / "users" / "player" / "Documents" / "save.dat"), std::string("level 7"));
  CHECK(fs::is_symlink(p / "drive_c" / "users" / "kim"));
  // The old name still leads there, for a path recorded in full.
  CHECK_EQ(slurp(p / "drive_c" / "users" / "kim" / "Documents" / "save.dat"), std::string("level 7"));
  CHECK(fs::is_directory(p / "drive_c" / "users" / "Public"));
  // Done once: a second look changes nothing.
  CHECK_EQ(session::adopt_player_profile(p), std::string());

  // Two old profiles: which holds the saves is not a guess to make.
  fs::path two = tmp / "two-profiles";
  write(two / "drive_c" / "users" / "kim" / "a", "1");
  write(two / "drive_c" / "users" / "root" / "b", "2");
  CHECK_EQ(session::adopt_player_profile(two), std::string());
  CHECK(!fs::exists(two / "drive_c" / "users" / "player"));

  // A player's own prefix, and one that already has both: left alone.
  fs::path both = tmp / "both-profiles";
  write(both / "drive_c" / "users" / "kim" / "a", "1");
  write(both / "drive_c" / "users" / "player" / "b", "2");
  CHECK_EQ(session::adopt_player_profile(both), std::string());
  CHECK(fs::is_directory(both / "drive_c" / "users" / "kim"));
  CHECK(!fs::is_symlink(both / "drive_c" / "users" / "kim"));
  CHECK_EQ(session::adopt_player_profile(tmp / "no-prefix-here"), std::string());
}

// ---- the desktop entry -----------------------------------------------------------

static void test_desktop(const fs::path& tmp) {
  section("desktop: one entry and one icon, named for the bundle, pointing at the file");
  player::DesktopPaths d = player::desktop_paths("retro-shelf", "/xdg", "/home/kim");
  CHECK_EQ(d.entry, fs::path("/xdg/applications/kretro-retro-shelf.desktop"));
  CHECK_EQ(d.icon, fs::path("/xdg/icons/kretro-retro-shelf.png"));
  CHECK_EQ(player::desktop_paths("b", "", "/home/kim").entry,
           fs::path("/home/kim/.local/share/applications/kretro-b.desktop"));
  // No home: no menu, rather than one under /tmp that anyone could have made.
  CHECK(player::desktop_paths("b", "", "").entry.empty());
  CHECK_THROWS(player::install_desktop_entry(player::desktop_paths("b", "", ""), "T", "b", "/x.run", ""));

  std::string e = player::desktop_entry("Retro Shelf Classics", "retro-shelf",
                                        "/media/kim/USB STICK/classics.run", "/xdg/icons/kretro-retro-shelf.png");
  CHECK(e.rfind("[Desktop Entry]\n", 0) == 0);
  CHECK(contains(e, "\nType=Application\n"));
  CHECK(contains(e, "\nName=Retro Shelf Classics\n"));
  // Quoted, because the path has a space in it.
  CHECK(contains(e, "\nExec=\"/media/kim/USB STICK/classics.run\"\n"));
  CHECK(contains(e, "\nIcon=/xdg/icons/kretro-retro-shelf.png\n"));
  CHECK(contains(e, "\nCategories=Game;\n"));
  CHECK(contains(e, "\nTerminal=false\n"));
  CHECK(contains(e, "\nX-Kretro-Bundle=retro-shelf\n"));
  // No icon: a generic one, not an empty key.
  CHECK(contains(player::desktop_entry("T", "b", "/x.run", ""), "\nIcon=applications-games\n"));
  // Characters the Exec rules reserve are escaped, and a % is never a field
  // code; the string escaping is applied on top.
  std::string odd = player::desktop_entry("A\nB", "b", "/g/100%/$HOME/\"q\"/x.run", "");
  CHECK(contains(odd, "\nName=A\\nB\n"));
  CHECK(contains(odd, "Exec=\"/g/100%%/\\\\$HOME/\\\\\"q\\\\\"/x.run\"\n"));

  // Installed and removed, and nothing else touched.
  player::DesktopPaths real = player::desktop_paths("retro-shelf", (tmp / "xdg").string(), "");
  player::install_desktop_entry(real, "Retro Shelf Classics", "retro-shelf", "/opt/classics.run",
                                std::string("\x89PNG\r\n\x1a\n", 8) + "icon");
  CHECK(fs::exists(real.entry));
  CHECK(fs::exists(real.icon));
  CHECK(contains(slurp(real.entry), "Icon=" + real.icon.string() + "\n"));
  CHECK((fs::status(real.entry).permissions() & fs::perms::owner_exec) != fs::perms::none);
  CHECK(player::remove_desktop_entry(real));
  CHECK(!fs::exists(real.entry));
  CHECK(!fs::exists(real.icon));
  CHECK(!player::remove_desktop_entry(real));
  CHECK(fs::is_empty(tmp / "xdg" / "applications"));
}

// ---- the command line -------------------------------------------------------------

static void test_cli() {
  section("cli: every form the player takes, and what it refuses");
  using K = Command::Kind;
  auto p = [](std::vector<std::string> a) { return player::parse_command(a); };
  CHECK(p({}).kind == K::Launcher);

  Command c = p({"play", "example-game"});
  CHECK(c.kind == K::Play);
  CHECK_EQ(c.game, std::string("example-game"));
  CHECK(!c.dry_run);
  c = p({"play", "example-game", "--dry-run"});
  CHECK(c.kind == K::Play && c.dry_run);
  c = p({"play", "--dry-run", "classic2", "--fullscreen"});
  CHECK(c.kind == K::Play && c.dry_run && c.game == "classic2" && c.fullscreen_set && c.fullscreen);
  c = p({"play", "classic2", "--windowed"});
  CHECK(c.fullscreen_set && !c.fullscreen);

  CHECK(p({"--doctor"}).kind == K::Doctor);
  c = p({"--doctor", "--save", "report.txt"});
  CHECK(c.kind == K::Doctor && c.file == "report.txt");

  c = p({"saves", "example", "export", "/tmp/example.saves.kgpack"});
  CHECK(c.kind == K::SavesExport && c.game == "example" && c.file == "/tmp/example.saves.kgpack");
  c = p({"saves", "example", "import", "in.kgpack"});
  CHECK(c.kind == K::SavesImport && c.game == "example" && c.file == "in.kgpack");

  c = p({"--extract-to", "/big/disk"});
  CHECK(c.kind == K::ExtractTo && c.file == "/big/disk" && c.game.empty());
  c = p({"--extract-to", "/big/disk", "classic2"});
  CHECK(c.kind == K::ExtractTo && c.game == "classic2");

  CHECK(p({"--licenses"}).kind == K::Licenses);
  CHECK(p({"--licences"}).kind == K::Licenses);
  CHECK(p({"--help"}).kind == K::Help);
  CHECK(p({"-h"}).kind == K::Help);
  c = p({"input", "--display", ":9", "--pid", "42"});
  CHECK(c.kind == K::Input && c.rest.size() == 4);

  // Refusals, each saying what was wrong.
  c = p({"play"});
  CHECK(c.kind == K::Error && contains(c.error, "needs the game"));
  c = p({"play", "a", "b"});
  CHECK(c.kind == K::Error && contains(c.error, "one game"));
  c = p({"play", "a", "--turbo"});
  CHECK(c.kind == K::Error && contains(c.error, "--turbo"));
  c = p({"saves", "example", "delete", "x"});
  CHECK(c.kind == K::Error && contains(c.error, "delete"));
  CHECK(p({"saves", "example", "export"}).kind == K::Error);
  CHECK(p({"--extract-to"}).kind == K::Error);
  CHECK(p({"--extract-to", "--dry-run"}).kind == K::Error);
  CHECK(p({"--doctor", "--save"}).kind == K::Error);
  CHECK(p({"--licenses", "now"}).kind == K::Error);
  c = p({"install", "dash3"});
  CHECK(c.kind == K::Error && contains(c.error, "install"));

  std::string u = player::usage("classics.run");
  CHECK(contains(u, "classics.run play <game>"));
  CHECK(contains(u, "--extract-to DIR"));
  CHECK(contains(u, "--licenses"));
}

// ---- a real player file --------------------------------------------------------------

static Meta pack_meta(const fs::path& tmp, const std::string& id) {
  fs::path root = tmp / ("tree-" + id);
  write(root / "GAME.EXE", "MZ this is " + id);
  write(root / "data" / "level1.dat", std::string(3000, 'L') + id);
  Meta m;
  m.id = id;
  m.name = "The game " + id;
  m.year = 1999;
  m.run.exe = "GAME.EXE";
  m.tree = Tree::from_directory(root);
  return m;
}

static fs::path make_pack(const fs::path& tmp, const std::string& id) {
  fs::path body = tmp / (id + ".body");
  std::string b;
  for (size_t i = 0; i < 20000; ++i) b.push_back(static_cast<char>('a' + (i * 7 + id.size()) % 26));
  write(body, b);
  fs::path out = tmp / "shelf" / (id + ".kgpack");
  fs::create_directories(out.parent_path());
  write_pack(out, pack_meta(tmp, id), WriteOptions{kg::Kind::Game, body, false});
  return out;
}

static fs::path make_player(const fs::path& tmp, const std::vector<std::string>& ids) {
  write(tmp / "in" / "bootstrap", std::string("\x7f" "ELF") + std::string(2000, 'b'));
  write(tmp / "in" / "tools", std::string(6000, 't'));
  write(tmp / "in" / "runtime", std::string(9000, 'r'));
  write(tmp / "in" / "app", std::string(3000, 'a'));
  fs::path base = tmp / "player-base";
  bundle::link_file(base, tmp / "in" / "bootstrap",
                    {{bundle::Kind::Tools, tmp / "in" / "tools", ""},
                     {bundle::Kind::Runtime, tmp / "in" / "runtime", ""},
                     {bundle::Kind::App, tmp / "in" / "app", ""}});
  bundle::BundleMeta m;
  m.id = "retro-shelf";
  m.title = "Retro Shelf Classics";
  m.version = "1.0";
  m.rights_acknowledged = true;
  std::vector<fs::path> packs;
  for (const std::string& id : ids) {
    bundle::GameMeta g;
    g.id = id;
    g.name = "The game " + id;
    g.year = 1999;
    g.display = "fit";
    m.games.push_back(g);
    packs.push_back(make_pack(tmp, id));
  }
  bundle::BaseSource bs;
  bs.path = base;
  return bundle::build_bundle(bs, m, packs, tmp / "classics.run").path;
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

static void test_player_file(const fs::path& tmp) {
  section("player: a built file opens, each pack is checked once, and damage names its game");
  fs::path file = make_player(tmp, {"example", "classic2"});
  player::Bundle b = player::Bundle::open(file);
  CHECK_EQ(b.meta.id, std::string("retro-shelf"));
  CHECK_EQ(b.meta.games.size(), size_t{2});
  CHECK(b.game("example") != nullptr);
  CHECK(b.game("dash3") == nullptr);
  CHECK_EQ(b.game_list(), std::string("example, classic2"));

  // KRETRO_TOC as the bootstrap sets it must agree with the table.
  const std::string toc = std::to_string(b.toc.toc_off) + ":" + std::to_string(b.toc.toc_len);
  CHECK(player::Bundle::open(file, toc).meta.id == "retro-shelf");
  CHECK_THROWS(player::Bundle::open(file, "4096:128"));
  // kretro's own base is not a player.
  CHECK_THROWS(player::Bundle::open(tmp / "player-base"));

  player::StateChoice st;
  st.dir = state_dir();
  player::Player p(b, rt::Env{}, st, gpu::Report{});
  CHECK_THROWS(p.game("dash3"));
  try {
    p.game("dash3");
  } catch (const std::exception& ex) {
    CHECK(contains(ex.what(), "example, classic2"));
  }
  // A pack opened out of the file is the game it says it is.
  CHECK_EQ(p.open_pack("classic2").meta().id, std::string("classic2"));
  CHECK(p.open_pack("classic2").base() == b.toc.at(*b.pack("classic2")));

  CHECK(!p.verified("example"));
  uint64_t seen = 0;
  p.verify("example", [&](uint64_t done, uint64_t) { seen = done; });
  CHECK(p.verified("example"));
  CHECK(seen > 0);
  CHECK(!p.verified("classic2"));

  // The settings default to the author's.
  CHECK(p.settings("example").display.mode == config::ScaleMode::Fit);

  // A byte of classic2's pack goes bad before its first check. It is named,
  // it is not remembered as checked, and example - checked, and whole - still
  // plays.
  const bundle::Entry* e = b.pack("classic2");
  flip(file, b.toc.at(*e) + e->len / 2);
  bool damaged = false;
  try {
    p.verify("classic2");
  } catch (const player::Damaged& ex) {
    damaged = true;
    CHECK_EQ(ex.game(), std::string("classic2"));
    CHECK(contains(ex.what(), "Game The game classic2 is damaged inside this file."));
    CHECK(contains(ex.what(), "other games"));
  }
  CHECK(damaged);
  CHECK(!p.verified("classic2"));
  p.verify("example");  // still fine

  // What unpacking would take, for the consent.
  player::UnpackPlan u = p.unpack_plan("example");
  CHECK(!u.ready);
  CHECK(u.need >= p.open_pack("example").header().body_len);
  CHECK(u.free > 0);
  CHECK_EQ(u.where, game_extract_dir("example"));

  // --extract-to's copy is used while it is there; once it is deleted, as
  // --extract-to says undoes it, the game goes back to the cache rather than
  // to a directory that is not there any more.
  fs::path elsewhere = tmp / "big-disk" / "example";
  fs::create_directories(elsewhere);
  write(state_dir() / "example" / "unpacked-at", elsewhere.string() + "\n");
  CHECK_EQ(p.unpack_plan("example").where, elsewhere);
  fs::remove_all(tmp / "big-disk");
  CHECK_EQ(p.unpack_plan("example").where, game_extract_dir("example"));
  fs::remove(state_dir() / "example" / "unpacked-at");

  // --extract-to ~/Games puts the game at ~/Games/<id>, and an id is the
  // author's choice. A directory of that name that some unpack did not make
  // is somebody's, and is left as it is; an empty one, or one of its own
  // unpacks, is filled. The DwarFS tool is a stand-in that unpacks one file.
  {
    fs::path tool = tmp / "fake-dwarfs";
    write(tool, "#!/bin/sh\nwhile [ $# -gt 0 ]; do [ \"$1\" = -o ] && out=\"$2\"; shift; done\n"
                "mkdir -p \"$out\" && echo unpacked > \"$out/GAME.EXE\"\n");
    fs::permissions(tool, fs::perms::owner_all);
    setenv("KRETRO_DWARFS", tool.c_str(), 1);
    fs::path mine = tmp / "Games" / "example";
    write(mine / "my-own-save.txt", "a year of play");
    bool refused = false;
    try {
      p.unpack("example", mine);
    } catch (const std::exception& ex) {
      refused = contains(ex.what(), "kretro did not unpack it");
    }
    CHECK(refused);
    CHECK_EQ(slurp(mine / "my-own-save.txt"), std::string("a year of play"));
    CHECK(!fs::exists(tmp / "Games" / "example.stamp"));
    CHECK(!fs::exists(state_dir() / "example" / "unpacked-at"));

    fs::path empty = tmp / "Games" / "empty" / "example";
    fs::create_directories(empty);
    CHECK_EQ(p.unpack("example", empty), empty);
    CHECK_EQ(slurp(empty / "GAME.EXE"), std::string("unpacked\n"));
    Pack pk = p.open_pack("example");
    const std::string stamp = session::extraction_stamp(pk.meta(), pk.header());
    CHECK(session::extraction_stamp_matches(session::extraction_stamp_file(empty), stamp));
    // Its own, again: replaced.
    write(empty / "left-over", "x");
    p.unpack("example", empty);
    CHECK(!fs::exists(empty / "left-over"));
    CHECK(fs::exists(empty / "GAME.EXE"));
    fs::remove(state_dir() / "example" / "unpacked-at");
    unsetenv("KRETRO_DWARFS");
  }

  // A second start of a game while the first holds its lock - playing it, or
  // still making its prefix - is told so before it touches the prefix. It
  // used to seed first and ask after, so two starts in the first minute each
  // removed the other's half-made copy.
  {
    // A runtime with a template in it, so there is a seed to be made.
    rt::Env fake;
    fake.root = tmp / "fake-runtime";
    write(fake.root / "opt" / "kretro" / "prefix-template" / ".kretro-wine-version", "wine-11.18\n");
    write(fake.root / "opt" / "kretro" / "prefix-template" / "system.reg", "WINE REGISTRY Version 2\n");
    player::Player pt(b, fake, st, gpu::Report{});
    session::GameLock held = session::lock_game("example");
    CHECK(!held.busy());
    // The author's files are the lowest layer the running game's overlay
    // has, and another build of the bundle - this one carries none - must
    // not restage them, or remove them, from under it.
    write(state_dir() / "example" / "extra" / "dgVoodoo.conf", "the running build's");
    bool refused = false;
    try {
      pt.play("example", player::PlayRequest{});
    } catch (const std::exception& ex) {
      refused = contains(ex.what(), "made ready to play");
    }
    CHECK(refused);
    CHECK(!fs::exists(game_prefix_dir("example") / "system.reg"));
    CHECK_EQ(slurp(state_dir() / "example" / "extra" / "dgVoodoo.conf"), std::string("the running build's"));

    // Nor is it unpacked while the lock is held: without FUSE a session
    // plays from, and writes into, the very tree an unpack removes first.
    setenv("KRETRO_DWARFS", (tmp / "fake-dwarfs").c_str(), 1);
    fs::path played = tmp / "Games" / "empty" / "example";  // unpacked above, stamped
    write(played / "SAVE.DAT", "being written");
    bool busy = false;
    try {
      pt.unpack("example", played);
    } catch (const std::exception& ex) {
      busy = contains(ex.what(), "another copy of this player");
    }
    CHECK(busy);
    CHECK_EQ(slurp(played / "SAVE.DAT"), std::string("being written"));
    unsetenv("KRETRO_DWARFS");
  }
}

static void test_helper_state(const fs::path& tmp) {
  section("state: the gamepad helper keeps the state its player chose, whatever HOME says");
  fs::path file = tmp / "classics.run";
  player::Bundle b = player::Bundle::open(file);
  const std::string saved = std::getenv("KRETRO_STATE");
  const char* h = std::getenv("HOME");
  const std::string saved_home = h ? h : "";

  // What a session hands its helper: the game's own HOME, and KRETRO_STATE as
  // the player set it. Chosen afresh, the state would be under the game's
  // home - and the helper would read another settings.toml than the one the
  // Controls page wrote.
  fs::path game_home = tmp / "state" / "example" / "home";
  setenv("HOME", game_home.c_str(), 1);
  unsetenv("XDG_DATA_HOME");
  CHECK_EQ(player::choose_state(file, b.meta.id, "", game_home.string()).dir,
           game_home / ".local" / "share" / "retro-shelf");
  player::StateChoice c = player::inherited_state(b, file);
  CHECK_EQ(c.dir, fs::path(saved));
  CHECK(bundle_layout());
  CHECK(!fs::exists(game_home / ".local"));

  // Run by hand, with nothing handed down, it is an ordinary choice.
  unsetenv("KRETRO_STATE");
  setenv("XDG_DATA_HOME", (tmp / "xdg").c_str(), 1);
  CHECK_EQ(player::inherited_state(b, file).dir, tmp / "xdg" / "retro-shelf");

  setenv("KRETRO_STATE", saved.c_str(), 1);
  unsetenv("XDG_DATA_HOME");
  if (saved_home.empty()) unsetenv("HOME");
  else setenv("HOME", saved_home.c_str(), 1);
  use_bundle_layout("");
}

static void test_signal_while_mounting(const fs::path& tmp) {
  section("session: a signal while the game is being mounted still releases the mount");
  // The DwarFS tool here is a stand-in that is signalled as it mounts - a
  // Ctrl-C, or a preview's Stop, in the seconds a mount can take - and
  // fusermount3 one that writes down what it was asked to unmount.
  fs::path file = tmp / "classics.run";
  player::Bundle b = player::Bundle::open(file);
  const bundle::Entry* e = b.pack("example");
  fs::path bin = tmp / "signal-bin";
  fs::path log = tmp / "unmounted.log";
  write(bin / "fusermount3", "#!/bin/sh\necho \"$@\" >> '" + log.string() + "'\n");
  write(bin / "dwarfs", "#!/bin/sh\nkill -TERM $PPID\nsleep 2\nexit 1\n");
  fs::permissions(bin / "fusermount3", fs::perms::owner_all);
  fs::permissions(bin / "dwarfs", fs::perms::owner_all);
  pid_t pid = fork();
  if (pid == 0) {
    const char* path = std::getenv("PATH");
    setenv("PATH", (bin.string() + ":" + (path ? path : "/usr/bin:/bin")).c_str(), 1);
    setenv("KRETRO_DWARFS", (bin / "dwarfs").c_str(), 1);
    session::Options o;
    session::Source s;
    s.file = file;
    s.off = b.toc.at(*e);
    s.len = e->len;
    o.source = s;
    o.dry_run = true;
    try {
      session::play(rt::Env{}, "example", o);
    } catch (const std::exception&) {
      _exit(3);
    }
    _exit(0);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  // The handler's way out, not the signal's default.
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGTERM);
  CHECK(contains(slurp(log), (game_mount_dir("example") / "image").string()));
}

static void test_tied_children() {
  section("session: a helper a session starts ends with the process that started it");
  // What a session does for Weston: fork it tied, give it a session of its
  // own, and then end the way a Ctrl-C ends a player - _exit from a signal
  // handler, no destructor run. The helper must not outlive it. This process
  // stands in for init, so the orphan comes back here to be waited for.
  prctl(PR_SET_CHILD_SUBREAPER, 1);
  int p[2];
  if (pipe(p) != 0) {
    CHECK(false);
    return;
  }
  pid_t player = fork();
  if (player == 0) {
    close(p[0]);
    pid_t helper = kg::fork_tied(SIGTERM);
    if (helper == 0) {
      setsid();
      for (;;) pause();
    }
    ssize_t w = write(p[1], &helper, sizeof(helper));
    (void)w;
    _exit(130);
  }
  close(p[1]);
  pid_t helper = -1;
  ssize_t r = read(p[0], &helper, sizeof(helper));
  close(p[0]);
  int st = 0;
  waitpid(player, &st, 0);
  CHECK(r == static_cast<ssize_t>(sizeof(helper)) && helper > 0);
  bool ended = false;
  for (int i = 0; i < 50 && helper > 0; ++i) {
    if (waitpid(helper, &st, WNOHANG) == helper) {
      ended = WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM;
      helper = -1;
      break;
    }
    usleep(50 * 1000);
  }
  if (helper > 0) {
    kill(helper, SIGKILL);
    waitpid(helper, &st, 0);
  }
  CHECK(ended);
  prctl(PR_SET_CHILD_SUBREAPER, 0);
}

// Wine's home holds every XDG directory, whatever the person's own are set to:
// winemenubuilder writes wherever XDG_DATA_HOME says, and that must be the
// game's home and never the real application menu.
static void test_wine_home_is_confined() {
  rt::Env e;
  e.set("XDG_DATA_HOME", "/home/someone/.local/share");
  e.set("XDG_CACHE_HOME", "/home/someone/.cache");
  rt::confine_home(e, "/state/g/home");
  auto get = [&](const std::string& k) {
    for (const auto& kv : e.vars) if (kv.first == k) return kv.second;
    return std::string("<unset>");
  };
  CHECK_EQ(get("HOME"), std::string("/state/g/home"));
  CHECK_EQ(get("XDG_CONFIG_HOME"), std::string("/state/g/home/.config"));
  CHECK_EQ(get("XDG_DATA_HOME"), std::string("/state/g/home/.local/share"));
  CHECK_EQ(get("XDG_CACHE_HOME"), std::string("/state/g/home/.cache"));
  CHECK_EQ(get("XDG_STATE_HOME"), std::string("/state/g/home/.local/state"));
  size_t data = 0;
  for (const auto& kv : e.vars) data += kv.first == "XDG_DATA_HOME";
  CHECK_EQ(data, size_t(1));
}

int main() {
  const char* base = std::getenv("TMPDIR");
  fs::path tmp = fs::path(base && *base ? base : "/tmp") / ("kretro-test-player-" + std::to_string(getpid()));
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  // Before anything asks where the state is: it is resolved once.
  setenv("KRETRO_STATE", (tmp / "state").c_str(), 1);
  setenv("XDG_RUNTIME_DIR", (tmp / "run").c_str(), 1);
  setenv("XDG_CACHE_HOME", (tmp / "cache").c_str(), 1);

  try {
    test_state_choice(tmp);
    test_bundle_layout();
    test_launcher_and_settings(tmp);
    test_warnings_once(tmp);
    test_memo(tmp);
    test_prefix_decision();
    test_prefix_seed(tmp);
    test_key_registry();
    test_extra_files(tmp);
    test_gamepad_bindings();
    test_profile_adoption(tmp);
    test_desktop(tmp);
    test_cli();
    test_player_file(tmp);
    test_helper_state(tmp);
    test_signal_while_mounting(tmp);
    test_tied_children();
    test_wine_home_is_confined();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  FAIL unexpected exception: %s\n", e.what());
    ++failures;
  }

  fs::remove_all(tmp);
  std::fprintf(stderr, "\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
