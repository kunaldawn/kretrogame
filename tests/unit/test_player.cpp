// Tier 1 unit tests for the player: where it keeps its state, what it
// remembers, how a game's prefix is made and upgraded, the desktop entry, the
// command line, an author's key in the registry - and a real player file,
// built from small fake packs, opened and checked the way a launch checks it.
//
// No Wine, no display, no FUSE. Every decision that would start one is a pure
// function and tested as such; the files it makes are made under a temporary
// directory and read back.
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "player/doctor.h"
#include "player/player.h"
#include "player/prefix.h"
#include "player/settings.h"
#include "player/state_dir.h"
#include "player/unpack.h"
#include "player/verify_memo.h"
#include "session/compositor.h"
#include "session/input_helper.h"
#include "session/internal.h"
#include "session/prefix.h"
#include "util/paths.h"
#include "util/proc.h"
#include "support/bundle_fixtures.h"
#include "support/check.h"
#include "support/files.h"

namespace fs = std::filesystem;
using namespace kg;
using player::Command;
using player::PrefixAction;

using kgtest::contains;
using kgtest::flip_byte;
using kgtest::make_base;
using kgtest::make_pack;
using kgtest::section;
using kgtest::slurp;
using kgtest::write_file;

// ---- state ------------------------------------------------------------------

static void test_state_choice(const fs::path& tmp) {
  section("state: portable beside the file, else XDG, and a read-only portable falls back");
  fs::path dir = tmp / "stick";
  fs::path exe = dir / "classics.run";
  write_file(exe, "ELF");

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
  write_file(dir / "other.run-data", "not a directory");
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
  write_file(f, "[window\nwidth = = =\n");
  CHECK(!player::load_launcher(f).desktop_offered);
  write_file(f, "[window]\nwidth = 5\n");  // absurd: the default, not a 5-pixel window
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
  write_file(sf, "[controls]\ngamepad = \"joystick-of-legend\"\n");
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

// The exact text launcher.toml and settings.toml are written as, pinned so
// that moving the code that writes them (toml_string among it) can be shown to
// change nothing in a player's state directory.
static void test_golden_state_files(const fs::path& tmp) {
  section("state: launcher.toml and settings.toml, byte for byte");
  player::LauncherState s;
  s.window_w = 1600;
  s.window_h = 900;
  s.window_fullscreen = true;
  s.desktop_offered = true;
  s.desktop_installed = false;
  s.portable_fallback_said = true;
  // Not warning_key()s, which are hashes: two entries that make toml_string
  // escape a tab, a quote and a backslash.
  s.warnings_seen = {"b\"q\\x", "a\tb"};
  fs::path f = tmp / "golden" / "launcher.toml";
  player::save_launcher(f, s);
  const std::string launcher =
      "# The launcher's memory. Safe to delete: it only means being asked again.\n"
      "\n"
      "[window]\n"
      "width = 1600\n"
      "height = 900\n"
      "fullscreen = true\n"
      "\n"
      "[desktop]\n"
      "offered = true\n"
      "installed = false\n"
      "\n"
      "[notes]\n"
      "portable_fallback_said = true\n"
      "warnings_seen = [\"a\\tb\", \"b\\\"q\\\\x\"]\n";
  CHECK_EQ(slurp(f), launcher);

  player::GameSettings g;
  g.display.mode = config::ScaleMode::Native;
  g.display.scale = 3;
  g.fullscreen = true;
  g.gamepad = "kretro";
  fs::path sf = tmp / "golden" / "settings.toml";
  player::save_game_settings(sf, g);
  const std::string settings =
      "[display]\n"
      "mode = \"native\"\n"
      "scale = 3\n"
      "fullscreen = true\n"
      "\n"
      "[controls]\n"
      "gamepad = \"kretro\"\n";
  CHECK_EQ(slurp(sf), settings);
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
  write_file(t / ".kretro-wine-version", "wine-11.18 (Staging)\n");
  write_file(t / "system.reg", "WINE REGISTRY Version 2\n[Software] 1\n");
  write_file(t / "user.reg", "WINE REGISTRY Version 2\n");
  write_file(t / "drive_c" / "windows" / "system32" / "kernel32.dll", std::string(5000, 'k'));
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
  write_file(p / "drive_c" / "users" / "player" / "Documents" / "save1.dat", "level 7");
  write_file(p / "user.reg", "WINE REGISTRY Version 2\n[Software\\\\Example Publisher] 2\n\"Volume\"=\"9\"\n");
  fs::path snap = player::snapshot_prefix(p, tmp / "state" / "example" / "prefix-before");
  CHECK(contains(snap.filename().string(), "wine-11.18"));
  CHECK_EQ(slurp(snap / "drive_c" / "users" / "player" / "Documents" / "save1.dat"), std::string("level 7"));
  CHECK(contains(slurp(snap / "user.reg"), "Volume"));
  CHECK(fs::exists(snap / ".kretro-wine-version"));
  // The whole of drive_c/windows is Wine's to rewrite and is not copied.
  CHECK(!fs::exists(snap / "drive_c" / "windows"));
  // A copy, not a link: an upgrade rewriting the prefix leaves it as it was.
  write_file(p / "user.reg", "rewritten");
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

static void test_host_device_links(const fs::path& tmp) {
  section("prefix: host device links go, drive links stay");
  fs::path p = tmp / "devlinks" / "prefix";
  fs::path dd = p / "dosdevices";
  fs::create_directories(p / "drive_c");
  fs::create_directories(dd);
  fs::create_directory_symlink("../drive_c", dd / "c:");
  fs::create_directory_symlink("../drive_c", dd / "d:");
  // What a mount manager makes for a drive of the machine's own: dangling
  // here, as it is on a machine without that device.
  fs::create_symlink("/nonexistent/kretro-test-sr0", dd / "d::");
  fs::create_symlink("/nonexistent/kretro-test-sr1", dd / "e::");
  // Not a letter and two colons: left alone.
  fs::create_symlink("/nonexistent/kretro-test-com1", dd / "com1");
  fs::create_symlink("/nonexistent/kretro-test-ab", dd / "ab::");
  player::drop_host_device_links(p);
  CHECK(!fs::exists(fs::symlink_status(dd / "d::")));
  CHECK(!fs::exists(fs::symlink_status(dd / "e::")));
  CHECK(fs::is_symlink(dd / "c:"));
  CHECK(fs::is_symlink(dd / "d:"));
  CHECK(fs::is_symlink(dd / "com1"));
  CHECK(fs::is_symlink(dd / "ab::"));
  CHECK(fs::is_directory(p / "drive_c"));
  // A prefix with no dosdevices yet is not an error.
  player::drop_host_device_links(tmp / "devlinks" / "no-prefix");
  CHECK(!fs::exists(tmp / "devlinks" / "no-prefix"));
}

static void test_embedded_key_once(const fs::path& tmp) {
  section("key: a prefix that already has the key is not given it again");
  // Only the path that needs no Wine: the marker already holds the hash of
  // this key's text, so regedit is never run. Putting the key in is covered
  // by the bundle integration test's dry run.
  bundle::GameMeta::Key k;
  k.value = "1234-5678-9ABC";
  k.registry_path = "Software\\Example Publisher\\EXAMPLE";
  k.registry_value = "CDKey";
  fs::path p = tmp / "keyed" / "prefix";
  fs::create_directories(p / "drive_c");
  const std::string want = kg::to_hex(hash_string(player::key_registry(k)));
  write_file(p / ".kretro-key", want + "\n");
  std::vector<std::string> said;
  player::apply_embedded_key(rt::Env{}, p, k, [&](const std::string& l) { said.push_back(l); });
  CHECK(said.empty());
  CHECK(!fs::exists(p / "drive_c" / ".kretro-key.reg"));
  CHECK_EQ(slurp(p / ".kretro-key"), want + "\n");
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
  write_file(tree / "Bin" / "GAME.EXE", "MZ");
  write_file(tree / "readme.txt", "x");
  m.tree = Tree::from_directory(tree);
  CHECK_EQ(player::exe_dir_in_tree(m), std::string("Bin"));
  m.run.exe = "readme.txt";
  CHECK_EQ(player::exe_dir_in_tree(m), std::string());
  m.run.exe = "../../x.exe";
  CHECK_EQ(player::exe_dir_in_tree(m), std::string());

  // The layer: beside the executable, and exactly the bundle's files - what a
  // newer build dropped goes, and so does a copy where the exe was before.
  fs::path layer = tmp / "extra-layer";
  write_file(layer / "old.dll", "gone");
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
  write_file(p / "drive_c" / "users" / "kim" / "Documents" / "save.dat", "level 7");
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
  write_file(two / "drive_c" / "users" / "kim" / "a", "1");
  write_file(two / "drive_c" / "users" / "root" / "b", "2");
  CHECK_EQ(session::adopt_player_profile(two), std::string());
  CHECK(!fs::exists(two / "drive_c" / "users" / "player"));

  // A player's own prefix, and one that already has both: left alone.
  fs::path both = tmp / "both-profiles";
  write_file(both / "drive_c" / "users" / "kim" / "a", "1");
  write_file(both / "drive_c" / "users" / "player" / "b", "2");
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

// The line a session starts the gamepad helper with, and the helper's reading
// of it: kretro and the player both read what the session wrote, so the two
// halves have to agree word for word.
static void test_input_helper_args() {
  section("input helper: the session's argv reads back as it was written");
  session::InputHelperArgs in;
  in.display = ":7";
  in.pid = 4242;
  in.game = "example-game";
  in.pause = false;
  const std::vector<std::string> argv = session::input_helper_argv(in);
  CHECK((argv == std::vector<std::string>{"input", "--display", ":7", "--pid", "4242", "--game", "example-game",
                                          "--no-pause"}));
  // parse_input_helper takes the words after "input".
  session::InputHelperArgs out = session::parse_input_helper({argv.begin() + 1, argv.end()});
  CHECK_EQ(out.display, in.display);
  CHECK_EQ(out.pid, in.pid);
  CHECK_EQ(out.game, in.game);
  CHECK_EQ(out.pause, in.pause);
  CHECK(out.valid());

  // No game and pausing: neither --game nor --no-pause is written.
  session::InputHelperArgs bare;
  bare.display = ":1";
  bare.pid = 9;
  const std::vector<std::string> bare_argv = session::input_helper_argv(bare);
  CHECK((bare_argv == std::vector<std::string>{"input", "--display", ":1", "--pid", "9"}));
  out = session::parse_input_helper({bare_argv.begin() + 1, bare_argv.end()});
  CHECK_EQ(out.display, std::string(":1"));
  CHECK_EQ(out.pid, 9);
  CHECK(out.game.empty());
  CHECK(out.pause);

  // A word it does not know is passed over, and a --pid with nothing after it
  // is ignored, which leaves the arguments not valid.
  out = session::parse_input_helper({"--display", ":3", "--bogus", "--pid"});
  CHECK_EQ(out.display, std::string(":3"));
  CHECK_EQ(out.pid, 0);
  CHECK(out.pause);
  CHECK(!out.valid());
}

// The newest of the logs a doctor report ends with: the most recently written
// file that exists, whichever the candidates are.
static void test_newest_file(const fs::path& tmp) {
  section("doctor: the newest log of those that exist");
  const fs::path older = tmp / "logs" / "older.log";
  const fs::path newer = tmp / "logs" / "newer.log";
  const fs::path missing = tmp / "logs" / "missing.log";
  write_file(older, "old\n");
  write_file(newer, "new\n");
  const auto now = fs::last_write_time(newer);
  fs::last_write_time(older, now - std::chrono::hours(1));
  CHECK_EQ(player::doctor::newest_file({older, missing, newer}), newer);
  CHECK_EQ(player::doctor::newest_file({newer, older}), newer);
  CHECK_EQ(player::doctor::newest_file({missing}), fs::path());
}

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
  // The launcher's own measure of the panel, from a process whose video is
  // down.
  CHECK(p({"panel"}).kind == K::Panel);
  CHECK(p({"panel", "now"}).kind == K::Error);

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

static fs::path make_player(const fs::path& tmp, const std::vector<std::string>& ids) {
  fs::path base = make_base(tmp);
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
  flip_byte(file, b.toc.at(*e) + e->len / 2);
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
  write_file(state_dir() / "example" / "unpacked-at", elsewhere.string() + "\n");
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
    write_file(tool, "#!/bin/sh\nwhile [ $# -gt 0 ]; do [ \"$1\" = -o ] && out=\"$2\"; shift; done\n"
               "mkdir -p \"$out\" && echo unpacked > \"$out/GAME.EXE\"\n");
    fs::permissions(tool, fs::perms::owner_all);
    setenv("KRETRO_DWARFS", tool.c_str(), 1);
    fs::path mine = tmp / "Games" / "example";
    write_file(mine / "my-own-save.txt", "a year of play");
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
    write_file(empty / "left-over", "x");
    p.unpack("example", empty);
    CHECK(!fs::exists(empty / "left-over"));
    CHECK(fs::exists(empty / "GAME.EXE"));
    fs::remove(state_dir() / "example" / "unpacked-at");
    unsetenv("KRETRO_DWARFS");
  }

  // A second start of a game while the first holds its lock - playing it, or
  // still making its prefix - is told so before it touches the prefix. Seeding
  // first and asking after would let two starts in the first minute each
  // remove the other's half-made copy.
  {
    // A runtime with a template in it, so there is a seed to be made.
    rt::Env fake;
    fake.root = tmp / "fake-runtime";
    write_file(fake.root / "opt" / "kretro" / "prefix-template" / ".kretro-wine-version", "wine-11.18\n");
    write_file(fake.root / "opt" / "kretro" / "prefix-template" / "system.reg", "WINE REGISTRY Version 2\n");
    player::Player pt(b, fake, st, gpu::Report{});
    session::GameLock held = session::lock_game("example");
    CHECK(!held.busy());
    // The author's files are the lowest layer the running game's overlay
    // has, and another build of the bundle - this one carries none - must
    // not restage them, or remove them, from under it.
    write_file(state_dir() / "example" / "extra" / "dgVoodoo.conf", "the running build's");
    bool refused = false;
    try {
      pt.play("example", player::PlayOverrides{});
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
    write_file(played / "SAVE.DAT", "being written");
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
  write_file(bin / "fusermount3", "#!/bin/sh\necho \"$@\" >> '" + log.string() + "'\n");
  write_file(bin / "dwarfs", "#!/bin/sh\nkill -TERM $PPID\nsleep 2\nexit 1\n");
  fs::permissions(bin / "fusermount3", fs::perms::owner_all);
  fs::permissions(bin / "dwarfs", fs::perms::owner_all);
  pid_t pid = fork();
  if (pid == 0) {
    const char* path = std::getenv("PATH");
    setenv("PATH", (bin.string() + ":" + (path ? path : "/usr/bin:/bin")).c_str(), 1);
    setenv("KRETRO_DWARFS", (bin / "dwarfs").c_str(), 1);
    session::PlayRequest req;
    req.id = "example";
    session::Source s;
    s.file = file;
    s.off = b.toc.at(*e);
    s.len = e->len;
    req.source = s;
    req.dry_run = true;
    try {
      session::play(rt::Env{}, req);
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

// Wine commands run before the compositor get no display. Both names are there
// with an empty value rather than missing: a missing one falls back to the
// host's (winewayland connects to wayland-0), and only an empty one leaves Wine
// on its null driver.
static void test_offscreen_env_has_no_display() {
  rt::Env e;
  e.root = "/rt";
  e.library_path = "/rt/lib";
  e.set("DISPLAY", ":0");
  e.set("WINEPREFIX", "/state/g/prefix");
  rt::Env o = rt::offscreen(e);
  auto count = [&](const std::string& k) {
    size_t n = 0;
    for (const auto& kv : o.vars) n += kv.first == k;
    return n;
  };
  CHECK_EQ(count("DISPLAY"), size_t(1));
  CHECK_EQ(count("WAYLAND_DISPLAY"), size_t(1));
  CHECK_EQ(o.get("DISPLAY"), std::string());
  CHECK_EQ(o.get("WAYLAND_DISPLAY"), std::string());
  CHECK_EQ(o.get("WINEPREFIX"), std::string("/state/g/prefix"));
  CHECK_EQ(o.vars.size(), size_t(3));
  CHECK_EQ(o.root, fs::path("/rt"));
  CHECK_EQ(o.library_path, std::string("/rt/lib"));
  // The environment it came from keeps its display: the game needs it.
  CHECK_EQ(e.get("DISPLAY"), std::string(":0"));
  CHECK_EQ(e.vars.size(), size_t(2));
}

static double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static void test_run_returns_with_its_child() {
  section("proc: run returns when its program exits, not when what it left behind does");
  // A Wine command leaves its wineserver and services running for seconds,
  // each holding the command's stdout; a daemon holds it for as long as it
  // lives. The call is over when the program is.
  auto t0 = std::chrono::steady_clock::now();
  ProcResult r = kg::run({"/bin/sh", "-c", "echo before; sleep 30 & echo \"daemon=$!\"; echo after"});
  const double took = seconds_since(t0);
  CHECK(r.ok());
  CHECK(took < 1.0);
  CHECK(contains(r.out, "before"));
  CHECK(contains(r.out, "after"));
  // The daemon is this test's to end.
  const size_t at = r.out.find("daemon=");
  if (at != std::string::npos) {
    const pid_t daemon = static_cast<pid_t>(std::atoi(r.out.c_str() + at + 7));
    if (daemon > 0) kill(daemon, SIGKILL);
  }

  // The status and the output of a program that fails are still its own.
  r = kg::run({"/bin/sh", "-c", "echo no >&2; exit 3"});
  CHECK_EQ(r.status, 3);
  CHECK(contains(r.out, "no"));

  // A program that writes more than a pipe holds is read while it runs.
  r = kg::run({"/bin/sh", "-c", "head -c 200000 /dev/zero | tr '\\0' x"});
  CHECK(r.ok());
  CHECK_EQ(r.out.size(), size_t(200000));
}

static void test_run_timeout_covers_the_read() {
  section("proc: a timeout ends a program that is still writing, or still silent");
  // Blocked in the read: the program neither writes nor exits. The timeout
  // used to be looked at only once the output had ended.
  auto t0 = std::chrono::steady_clock::now();
  ProcOptions po;
  po.timeout_sec = 1;
  ProcResult r = kg::run({"/bin/sh", "-c", "echo started; exec sleep 30"}, po);
  const double took = seconds_since(t0);
  CHECK(r.signalled);
  CHECK_EQ(r.signal, SIGKILL);
  CHECK(!r.ok());
  CHECK(took >= 0.9 && took < 3.0);
  CHECK(contains(r.out, "started"));

  // Without capture, as before.
  po.capture = false;
  t0 = std::chrono::steady_clock::now();
  r = kg::run({"/bin/sleep", "30"}, po);
  CHECK(r.signalled);
  CHECK(seconds_since(t0) < 3.0);

  // A program that finishes in time is not touched.
  po.capture = true;
  po.timeout_sec = 5;
  r = kg::run({"/bin/sh", "-c", "echo quick"}, po);
  CHECK(r.ok());
  CHECK(contains(r.out, "quick"));
}

static void test_run_reads_what_is_left_in_the_pipe() {
  section("proc: what a program wrote before it exited is all read, however long that takes");
  // The last of a program's output is still in the pipe when it exits, and
  // the grace for what it left running used to cut that short too: 7z -so
  // piped into a hash, on a busy machine, was a short read reported as ok.
  const std::string cmd = "head -c 6000000 /dev/zero | tr '\\0' x";
  ProcResult r = kg::run({"/bin/sh", "-c", cmd});
  CHECK(r.ok());
  CHECK_EQ(r.out.size(), size_t(6000000));
  // With no grace at all, which is as late as a busy machine gets there,
  // and from a program that leaves a whole megabyte in the pipe as it exits.
  ProcOptions po;
  po.grace_ms = 0;
  for (int i = 0; i < 5; ++i) {
    r = kg::run({"/proc/self/exe", "--write-and-exit", "6000000"}, po);
    CHECK(r.ok());
    CHECK_EQ(r.out.size(), size_t(6000000));
  }
  // And still no waiting on a daemon with nothing to say.
  auto t0 = std::chrono::steady_clock::now();
  r = kg::run({"/bin/sh", "-c", "sleep 30 & echo \"daemon=$!\""}, po);
  CHECK(r.ok());
  CHECK(seconds_since(t0) < 1.0);
  const size_t at = r.out.find("daemon=");
  if (at != std::string::npos) {
    const pid_t daemon = static_cast<pid_t>(std::atoi(r.out.c_str() + at + 7));
    if (daemon > 0) kill(daemon, SIGKILL);
  }
}

static void test_run_with_its_own_output_closed() {
  section("proc: a caller with stdout or stderr closed still hears both from its program");
  // pipe2 hands out the lowest free descriptors, so a caller with 1 and 2
  // closed gets the pipe there, close-on-exec; dup2 onto itself is a no-op,
  // and the program started with that output closed.
  for (int lowest : {0, 1}) {
    pid_t pid = fork();
    if (pid == 0) {
      alarm(30);
      for (int fd = lowest; fd <= 2; ++fd) close(fd);
      ProcResult r = kg::run({"/bin/sh", "-c", "echo out; echo err >&2"});
      const bool out = r.out.find("out") != std::string::npos;
      const bool err = r.out.find("err") != std::string::npos;
      _exit((r.ok() ? 0 : 1) + (out ? 0 : 2) + (err ? 0 : 4));
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
  }
}

static std::atomic<int> g_own_handler_calls{0};
static void own_handler(int) { ++g_own_handler_calls; }

static void test_session_restores_signal_handlers(const fs::path& tmp) {
  section("session: a session puts back the signal handlers it found");
  // The shelf and a launcher carry on after a game. SDL's handlers were
  // replaced for good by the session's, which exits on the spot. Here the
  // mount fails at once, the session ends, and the handler before it must
  // be the one in place again.
  fs::path file = tmp / "classics.run";
  player::Bundle b = player::Bundle::open(file);
  const bundle::Entry* e = b.pack("example");
  fs::path bin = tmp / "restore-bin";
  write_file(bin / "fusermount3", "#!/bin/sh\nexit 0\n");
  write_file(bin / "dwarfs", "#!/bin/sh\nexit 1\n");
  fs::permissions(bin / "fusermount3", fs::perms::owner_all);
  fs::permissions(bin / "dwarfs", fs::perms::owner_all);
  pid_t pid = fork();
  if (pid == 0) {
    alarm(60);
    const char* path = std::getenv("PATH");
    setenv("PATH", (bin.string() + ":" + (path ? path : "/usr/bin:/bin")).c_str(), 1);
    setenv("KRETRO_DWARFS", (bin / "dwarfs").c_str(), 1);
    struct sigaction own {};
    own.sa_handler = own_handler;
    sigemptyset(&own.sa_mask);
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) sigaction(sig, &own, nullptr);
    session::PlayRequest req;
    req.id = "example";
    session::Source s;
    s.file = file;
    s.off = b.toc.at(*e);
    s.len = e->len;
    s.may_unpack = false;
    req.source = s;
    req.dry_run = true;
    try {
      session::play(rt::Env{}, req);
    } catch (const std::exception&) {
    }
    int wrong = 0;
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) {
      struct sigaction now {};
      sigaction(sig, nullptr, &now);
      if (now.sa_handler != own_handler) ++wrong;
    }
    // And it is called, rather than the session's exit.
    raise(SIGTERM);
    _exit(wrong == 0 && g_own_handler_calls == 1 ? 0 : 10 + wrong);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st));
  CHECK_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
}

static void test_signal_is_released_off_the_handler(const fs::path& tmp) {
  section("session: the signal handler only wakes the thread that releases the mounts");
  // The session plays on a worker while the window's thread goes on drawing,
  // and a handler that forks fusermount3 can land in the middle of a malloc
  // and hang on its lock. The handler now returns at once, and a thread of
  // the guard's unmounts and exits. The stand-in fusermount3 waits for word
  // that the handler came back, which a handler that did the work itself
  // could never send.
  fs::path bin = tmp / "wake-bin";
  fs::path log = tmp / "wake-unmounted.log";
  fs::path back = tmp / "wake-handler-returned";
  const std::string wait = "i=0\nwhile [ ! -e '" + back.string() +
                           "' ] && [ $i -lt 300 ]; do sleep 0.01; i=$((i+1)); done\n";
  write_file(bin / "fusermount3", "#!/bin/sh\n" + wait + "echo \"$@\" >> '" + log.string() + "'\n");
  fs::permissions(bin / "fusermount3", fs::perms::owner_all);
  const fs::path image = tmp / "wake-image";
  pid_t pid = fork();
  if (pid == 0) {
    alarm(30);
    const char* path = std::getenv("PATH");
    setenv("PATH", (bin.string() + ":" + (path ? path : "/usr/bin:/bin")).c_str(), 1);
    session::Layers l;
    l.image = image;
    l.image_mounted = true;
    session::detail::MountInterruptGuard guard(l);
    raise(SIGTERM);
    std::ofstream(back) << "returned\n";
    for (;;) pause();
  }
  int st = 0;
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st));
  CHECK_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 128 + SIGTERM);
  CHECK(fs::exists(back));
  CHECK(contains(slurp(log), "-u " + image.string()));
}

// Pointer capture is for a game. An installer run in a window got the swallowed
// first click and the locked pointer too, because the compositor turned capture
// on for every session on a Wayland host.
static void test_pointer_capture_is_for_games() {
  section("compositor: only a game session captures the pointer");
  session::CompositorOptions co;
  CHECK(!co.pointer_capture);  // installers and the stage probe leave it alone
  CHECK(session::detail::pointer_capture_on(true, nullptr));
  CHECK(session::detail::pointer_capture_on(true, "1"));
  // The person's own KRETRO_WESTON_CAPTURE=0 still wins for a game...
  CHECK(!session::detail::pointer_capture_on(true, "0"));
  CHECK(!session::detail::pointer_capture_on(true, "yes"));
  // ...and their KRETRO_WESTON_CAPTURE=1 does not reach an installer.
  CHECK(!session::detail::pointer_capture_on(false, "1"));
  CHECK(!session::detail::pointer_capture_on(false, nullptr));
}

static void test_socket_names_are_claimed(const fs::path& tmp) {
  section("compositor: two sessions starting together never share a Wayland socket name");
  // libwayland's own lock is only taken once a Weston is up, so two sessions
  // starting side by side both saw the name free, and the second removed the
  // first one's socket as it appeared.
  const fs::path dir = tmp / "sockets";
  fs::create_directories(dir);
  session::SocketName a = session::claim_socket_name(dir, "example");
  session::SocketName b = session::claim_socket_name(dir, "example");
  CHECK_EQ(a.name, std::string("kretro-example"));
  CHECK_EQ(b.name, std::string("kretro-example-2"));
  CHECK(a.claim_fd >= 0 && b.claim_fd >= 0);
  // Another process sees them taken too.
  int p[2];
  if (pipe(p) == 0) {
    pid_t pid = fork();
    if (pid == 0) {
      close(p[0]);
      session::SocketName c = session::claim_socket_name(dir, "example");
      const ssize_t w = write(p[1], c.name.data(), c.name.size());
      (void)w;
      _exit(0);
    }
    close(p[1]);
    char buf[64] = {};
    const ssize_t got = read(p[0], buf, sizeof(buf) - 1);
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK_EQ(std::string(buf, got > 0 ? static_cast<size_t>(got) : 0), std::string("kretro-example-3"));
  }
  // Let go, the name is free again; a socket a killed Weston left is removed.
  close(a.claim_fd);
  write_file(dir / "kretro-example", "stale");
  write_file(dir / "kretro-example.lock", "");
  session::SocketName again = session::claim_socket_name(dir, "example");
  CHECK_EQ(again.name, std::string("kretro-example"));
  CHECK(!fs::exists(dir / "kretro-example"));
  close(again.claim_fd);
  // A live compositor's socket - its lock held - is left alone.
  write_file(dir / "kretro-example", "live");
  const int held = open((dir / "kretro-example.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  CHECK(held >= 0 && flock(held, LOCK_EX | LOCK_NB) == 0);
  session::SocketName next = session::claim_socket_name(dir, "example");
  CHECK_EQ(next.name, std::string("kretro-example-3"));
  CHECK(fs::exists(dir / "kretro-example"));
  close(next.claim_fd);
  close(held);
  close(b.claim_fd);
}

static void test_play_stops_when_asked(const fs::path& tmp) {
  section("session: a stop asked before the prefix is touched ends the session there");
  fs::path file = tmp / "classics.run";
  player::Bundle b = player::Bundle::open(file);
  const bundle::Entry* e = b.pack("example");
  pid_t pid = fork();
  if (pid == 0) {
    alarm(60);
    // The stand-in from test_player_file, which unpacks GAME.EXE.
    setenv("KRETRO_DWARFS", (tmp / "fake-dwarfs").c_str(), 1);
    session::PlayRequest req;
    req.id = "example";
    session::Source s;
    s.file = file;
    s.off = b.toc.at(*e);
    s.len = e->len;
    s.no_fuse = true;
    s.may_unpack = true;
    s.extract_dir = tmp / "stop-unpacked";
    req.source = s;
    req.dry_run = true;
    std::vector<std::string> said;
    req.hooks.say = [&said](const std::string& l) { said.push_back(l); };
    req.hooks.stop_requested = [] { return true; };
    bool stopped = false;
    try {
      session::play(rt::Env{}, req);
    } catch (const std::exception& ex) {
      stopped = contains(ex.what(), "stopped before example started");
    }
    // Nothing of the prefix was made, and the window heard what was said.
    const bool no_prefix = !fs::exists(game_prefix_dir("example") / ".kretro-ready");
    const bool heard = !said.empty() && contains(said.front(), "game directory");
    _exit(stopped && no_prefix && heard ? 0 : 1 + (stopped ? 0 : 1) + (no_prefix ? 0 : 2) + (heard ? 0 : 4));
  }
  int st = 0;
  waitpid(pid, &st, 0);
  CHECK_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
}

// What test_run_reads_what_is_left_in_the_pipe runs: `n` bytes into a pipe
// made as big as the kernel allows, and an exit the moment the last of them
// is in it.
static int write_and_exit(size_t n) {
  fcntl(STDOUT_FILENO, F_SETPIPE_SZ, 1 << 20);
  const std::string buf(1 << 16, 'x');
  while (n > 0) {
    const ssize_t w = write(STDOUT_FILENO, buf.data(), std::min(n, buf.size()));
    if (w <= 0) return 1;
    n -= static_cast<size_t>(w);
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--write-and-exit") {
    return write_and_exit(static_cast<size_t>(std::strtoull(argv[2], nullptr, 10)));
  }
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
    test_golden_state_files(tmp);
    test_warnings_once(tmp);
    test_memo(tmp);
    test_prefix_decision();
    test_prefix_seed(tmp);
    test_key_registry();
    test_embedded_key_once(tmp);
    test_host_device_links(tmp);
    test_extra_files(tmp);
    test_gamepad_bindings();
    test_profile_adoption(tmp);
    test_desktop(tmp);
    test_cli();
    test_input_helper_args();
    test_newest_file(tmp);
    test_player_file(tmp);
    test_helper_state(tmp);
    test_signal_while_mounting(tmp);
    test_tied_children();
    test_wine_home_is_confined();
    test_offscreen_env_has_no_display();
    test_run_returns_with_its_child();
    test_run_timeout_covers_the_read();
    test_run_reads_what_is_left_in_the_pipe();
    test_run_with_its_own_output_closed();
    test_session_restores_signal_handlers(tmp);
    test_signal_is_released_off_the_handler(tmp);
    test_pointer_capture_is_for_games();
    test_socket_names_are_claimed(tmp);
    test_play_stops_when_asked(tmp);
  } catch (const std::exception& e) {
    kgtest::unexpected(e);
  }

  fs::remove_all(tmp);
  return kgtest::finish();
}
