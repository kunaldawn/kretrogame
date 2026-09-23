#include "state.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "../util/toml.h"

namespace kg::player {
namespace fs = std::filesystem;

fs::path portable_dir(const fs::path& exe) {
  return exe.parent_path() / (exe.filename().string() + "-data");
}

bool writable_dir(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
  // access(W_OK) answers from the mode bits and says yes on a read-only
  // mount of a directory those bits allow; only a write says no there.
  fs::path probe = dir / (".kretro-write-test-" + std::to_string(getpid()));
  int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  ::close(fd);
  ::unlink(probe.c_str());
  return true;
}

bool holds_links(const fs::path& dir) {
  // A prefix's drives are symbolic links - dosdevices/c: is ../drive_c - and
  // FAT and exFAT, what most USB sticks come formatted with, have none: the
  // kernel says EPERM. Taken as the state, such a directory passes every
  // other test and then fails every game's first launch.
  fs::path probe = dir / (".kretro-link-test-" + std::to_string(getpid()));
  if (::symlink(".", probe.c_str()) != 0) return false;
  ::unlink(probe.c_str());
  return true;
}

StateChoice choose_state(const fs::path& exe, const std::string& bundle_id,
                         const std::string& xdg_data_home, const std::string& home, uid_t me) {
  StateChoice c;
  std::error_code ec;
  fs::path portable = portable_dir(exe);
  if (!exe.empty() && fs::is_directory(portable, ec)) {
    // The directory it names, followed through a link: whose it is decides
    // whose prefix and whose HOME the game is given.
    struct stat st {};
    if (::stat(portable.c_str(), &st) == 0 && st.st_uid != me) {
      c.refused_why = "belongs to another user";
    } else if (!writable_dir(portable)) {
      c.refused_why = "cannot be written";
    } else if (!holds_links(portable)) {
      c.refused_why = "is on a disk that cannot hold symbolic links (FAT or exFAT), which a Wine prefix is "
                      "made of";
    } else {
      c.dir = portable;
      c.portable = true;
      return c;
    }
    c.refused_portable = portable;
  }
  // XDG says a relative XDG_DATA_HOME is invalid and is to be ignored.
  if (!xdg_data_home.empty() && fs::path(xdg_data_home).is_absolute()) {
    c.dir = fs::path(xdg_data_home) / bundle_id;
  } else if (!home.empty()) {
    c.dir = fs::path(home) / ".local" / "share" / bundle_id;
  } else {
    c.private_root = fs::path("/tmp") / ("kretro-" + std::to_string(me));
    c.dir = c.private_root / bundle_id;
  }
  return c;
}

void claim_private_dir(const fs::path& dir, uid_t me) {
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    throw std::runtime_error("cannot make " + dir.string() + ": " + std::strerror(errno));
  }
  // lstat: a link there is somebody pointing us somewhere, whatever it points at.
  struct stat st {};
  if (::lstat(dir.c_str(), &st) != 0) {
    throw std::runtime_error("cannot look at " + dir.string() + ": " + std::strerror(errno));
  }
  if (!S_ISDIR(st.st_mode) || st.st_uid != me) {
    throw std::runtime_error(dir.string() + " belongs to someone else, and this player keeps its saves "
                             "there when there is no home directory. Remove it, or set HOME.");
  }
  if ((st.st_mode & 077) != 0 && ::chmod(dir.c_str(), 0700) != 0) {
    throw std::runtime_error("cannot make " + dir.string() + " private: " + std::strerror(errno));
  }
}

std::string toml_string(const std::string& s) {
  std::string o = "\"";
  for (char ch : s) {
    switch (ch) {
      case '\\': o += "\\\\"; break;
      case '"': o += "\\\""; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      case '\r': o += "\\r"; break;
      default: o.push_back(ch);
    }
  }
  return o + "\"";
}

void write_atomically(const fs::path& file, const std::string& bytes) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  // A temporary of this writer's own. One fixed <file>.tmp was shared by
  // every copy of the player on one state - two launchers, a launcher and a
  // terminal - and the second to rename found it gone and said "cannot
  // write", or the first renamed the other's half-written bytes into place.
  // mkstemp's name is new, and O_EXCL: nothing already there is followed.
  std::string tmpl = file.string() + ".XXXXXX";
  int fd = ::mkstemp(tmpl.data());
  if (fd < 0) throw std::runtime_error("cannot write " + file.string() + ": " + std::strerror(errno));
  const fs::path tmp = tmpl;
  bool ok = ::fchmod(fd, 0644) == 0;
  for (size_t done = 0; ok && done < bytes.size();) {
    ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) ok = false;
    else done += static_cast<size_t>(n);
  }
  if (::close(fd) != 0) ok = false;
  if (ok) fs::rename(tmp, file, ec);
  if (!ok || ec) {
    fs::remove(tmp, ec);
    throw std::runtime_error("cannot write " + file.string());
  }
}

namespace {

// A settings file that does not parse is a new machine rather than an error:
// the player must start whatever state a crash or a hand edit left it in.
Toml read_or_empty(const fs::path& file) {
  std::error_code ec;
  if (!fs::exists(file, ec)) return Toml::parse("");
  try {
    return Toml::parse_file(file);
  } catch (const std::exception&) {
    return Toml::parse("");
  }
}

uint32_t clamp_u32(int64_t v, uint32_t lo, uint32_t hi, uint32_t def) {
  if (v < lo || v > hi) return def;
  return static_cast<uint32_t>(v);
}

}  // namespace

std::string warning_key(const std::string& line) { return to_hex(hash_string(line)).substr(0, 16); }

LauncherState load_launcher(const fs::path& file) {
  LauncherState s;
  Toml t = read_or_empty(file);
  s.window_w = clamp_u32(t.integer("window.width", s.window_w), 320, 16384, s.window_w);
  s.window_h = clamp_u32(t.integer("window.height", s.window_h), 240, 16384, s.window_h);
  s.window_fullscreen = t.boolean("window.fullscreen", false);
  s.desktop_offered = t.boolean("desktop.offered", false);
  s.desktop_installed = t.boolean("desktop.installed", false);
  s.portable_fallback_said = t.boolean("notes.portable_fallback_said", false);
  for (const std::string& w : t.array("notes.warnings_seen")) s.warnings_seen.insert(w);
  return s;
}

void save_launcher(const fs::path& file, const LauncherState& s) {
  std::ostringstream o;
  o << "# The launcher's memory. Safe to delete: it only means being asked again.\n\n"
    << "[window]\n"
    << "width = " << s.window_w << "\n"
    << "height = " << s.window_h << "\n"
    << "fullscreen = " << (s.window_fullscreen ? "true" : "false") << "\n\n"
    << "[desktop]\n"
    << "offered = " << (s.desktop_offered ? "true" : "false") << "\n"
    << "installed = " << (s.desktop_installed ? "true" : "false") << "\n\n"
    << "[notes]\n"
    << "portable_fallback_said = " << (s.portable_fallback_said ? "true" : "false") << "\n"
    << "warnings_seen = [";
  bool first = true;
  for (const std::string& w : s.warnings_seen) {
    o << (first ? "" : ", ") << toml_string(w);
    first = false;
  }
  o << "]\n";
  write_atomically(file, o.str());
}

GameSettings default_settings(const std::string& author_display, bool author_fullscreen) {
  GameSettings g;
  g.display.mode = config::parse_scale_mode(author_display);
  g.fullscreen = author_fullscreen;
  return g;
}

GameSettings load_game_settings(const fs::path& file, const GameSettings& defaults) {
  GameSettings g = defaults;
  Toml t = read_or_empty(file);
  if (t.has("display.mode")) g.display.mode = config::parse_scale_mode(t.str("display.mode"));
  if (t.has("display.scale")) g.display.scale = clamp_u32(t.integer("display.scale"), 0, 16, 0);
  if (t.has("display.fullscreen")) g.fullscreen = t.boolean("display.fullscreen");
  if (t.has("controls.gamepad")) {
    std::string p = t.str("controls.gamepad");
    if (p == "author" || p == "kretro") g.gamepad = p;
  }
  return g;
}

void save_game_settings(const fs::path& file, const GameSettings& s) {
  std::ostringstream o;
  o << "[display]\n"
    << "mode = " << toml_string(config::name_of(s.display.mode)) << "\n"
    << "scale = " << s.display.scale << "\n"
    << "fullscreen = " << (s.fullscreen ? "true" : "false") << "\n\n"
    << "[controls]\n"
    << "gamepad = " << toml_string(s.gamepad) << "\n";
  write_atomically(file, o.str());
}

// One line per pack checked: game, version, hash. The version is quoted so a
// version string with spaces in it is still one field.
namespace {
std::string memo_line(const std::string& game, const std::string& version, const Hash& h) {
  return game + " " + toml_string(version) + " " + to_hex(h);
}
}  // namespace

bool pack_verified(const fs::path& memo, const std::string& game, const std::string& version,
                   const Hash& h) {
  std::ifstream f(memo);
  const std::string want = memo_line(game, version, h);
  std::string line;
  while (std::getline(f, line)) {
    if (line == want) return true;
  }
  return false;
}

void remember_verified(const fs::path& memo, const std::string& game, const std::string& version,
                       const Hash& h) {
  if (pack_verified(memo, game, version, h)) return;
  // Older versions' lines are kept: two builds of one bundle share a state
  // directory on purpose, and a person going back and forth between them
  // should not pay for a full check each time. Bounded, so it cannot grow for
  // ever; what falls off the front is only checked again.
  std::vector<std::string> keep;
  {
    std::ifstream f(memo);
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty()) keep.push_back(line);
    }
  }
  keep.push_back(memo_line(game, version, h));
  constexpr size_t kMemoLines = 256;
  if (keep.size() > kMemoLines) keep.erase(keep.begin(), keep.end() - kMemoLines);
  std::string text;
  for (const std::string& l : keep) text += l + "\n";
  write_atomically(memo, text);
}

}  // namespace kg::player
