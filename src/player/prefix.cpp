#include "prefix.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <stdexcept>

#include "../util/hash.h"
#include "../util/text.h"

namespace kg::player {
namespace fs = std::filesystem;

const char* prefix_action_name(PrefixAction a) {
  switch (a) {
    case PrefixAction::Seed: return "seeded from the runtime's template";
    case PrefixAction::Upgrade: return "upgraded in place to this Wine";
    default: return "ready";
  }
}

PrefixAction prefix_action(const std::optional<std::string>& template_stamp,
                           const std::optional<std::string>& prefix_stamp, bool prefix_exists) {
  if (!prefix_exists) return template_stamp ? PrefixAction::Seed : PrefixAction::Ready;
  if (!template_stamp || !prefix_stamp) return PrefixAction::Ready;
  return *template_stamp == *prefix_stamp ? PrefixAction::Ready : PrefixAction::Upgrade;
}

std::optional<std::string> read_stamp(const fs::path& dir) {
  std::ifstream f(dir / ".kretro-wine-version");
  if (!f) return std::nullopt;
  std::string s;
  std::getline(f, s);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
  if (s.empty()) return std::nullopt;
  return s;
}

fs::path template_dir(const rt::Env& e) {
  if (!e.valid()) return {};
  fs::path t = e.root / "opt" / "kretro" / "prefix-template";
  std::error_code ec;
  return fs::is_directory(t, ec) ? t : fs::path();
}

namespace {

// One file, cloned when the filesystem shares extents and copied otherwise.
// The template is on a read-only DwarFS mount, so today this is a copy; a
// runtime extracted onto btrfs or XFS gets the clone for free.
bool clone_or_copy(const fs::path& from, const fs::path& to, fs::perms perms) {
  int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return false;
  int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    ::close(in);
    return false;
  }
  bool ok = ::ioctl(out, FICLONE, in) == 0;
  if (!ok) {
    char buf[1 << 16];
    ok = true;
    for (;;) {
      ssize_t n = ::read(in, buf, sizeof(buf));
      if (n == 0) break;
      if (n < 0) {
        if (errno == EINTR) continue;
        ok = false;
        break;
      }
      for (ssize_t done = 0; done < n;) {
        ssize_t w = ::write(out, buf + done, static_cast<size_t>(n - done));
        if (w < 0) {
          if (errno == EINTR) continue;
          ok = false;
          break;
        }
        done += w;
      }
      if (!ok) break;
    }
  }
  ::close(in);
  if (::close(out) != 0) ok = false;
  // The template is read-only; its copy is the game's, and Wine writes to it.
  std::error_code ec;
  fs::permissions(to, perms | fs::perms::owner_write, ec);
  return ok;
}

// A whole tree: directories made, symlinks copied as symlinks, files cloned.
// False when anything did not arrive.
bool copy_tree(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) return false;
  auto it = fs::recursive_directory_iterator(from, ec);
  if (ec) return false;
  for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) return false;
    std::error_code e;
    // Lexically: fs::relative resolves symlinks first, and dosdevices/c: would
    // come out as drive_c - the directory it points at, not the link.
    fs::path rel = it->path().lexically_relative(from);
    if (rel.empty()) return false;
    fs::path dst = to / rel;
    if (it->is_symlink(e)) {
      fs::copy_symlink(it->path(), dst, e);
      if (e) return false;
      continue;
    }
    if (it->is_directory(e)) {
      fs::create_directories(dst, e);
      if (e) return false;
      // Writable, whatever the read-only template said, or Wine cannot make
      // a file in it.
      fs::permissions(dst, fs::perms::owner_all, fs::perm_options::add, e);
      continue;
    }
    if (!clone_or_copy(it->path(), dst, it->status(e).permissions())) return false;
  }
  return !ec;
}

}  // namespace

void seed_prefix(const fs::path& tmpl, const fs::path& prefix) {
  std::error_code ec;
  fs::path staged = prefix;
  staged += ".seeding";
  fs::remove_all(staged, ec);
  if (!copy_tree(tmpl, staged)) {
    fs::remove_all(staged, ec);
    throw std::runtime_error("could not copy the Wine prefix into " + prefix.parent_path().string() +
                             " - is the disk full?");
  }
  // c: and nothing else. The discs and the game's own drive are the
  // session's, made fresh each launch; z: to / is the template's and is
  // exactly what this prefix must not keep.
  fs::path dd = staged / "dosdevices";
  fs::remove_all(dd, ec);
  fs::create_directories(dd, ec);
  fs::create_directory_symlink("../drive_c", dd / "c:", ec);
  if (ec) {
    fs::remove_all(staged, ec);
    throw std::runtime_error("could not make the prefix's C: drive");
  }
  fs::remove_all(prefix, ec);
  fs::rename(staged, prefix, ec);
  if (ec) {
    std::error_code e2;
    fs::remove_all(staged, e2);
    throw std::runtime_error("could not put the Wine prefix in place: " + ec.message());
  }
}

fs::path snapshot_prefix(const fs::path& prefix, const fs::path& into) {
  std::error_code ec;
  std::string stamp = read_stamp(prefix).value_or("unknown wine");
  std::string name;
  for (char c : stamp) {
    name.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' ? c : '-');
  }
  char when[32];
  std::time_t now = std::time(nullptr);
  std::strftime(when, sizeof(when), "%Y%m%d-%H%M%S", std::localtime(&now));
  fs::path dst = into / (name + "-" + when);
  fs::create_directories(dst, ec);
  if (ec) throw std::runtime_error("could not snapshot the prefix: " + ec.message());

  // A real copy, not links: wineboot -u rewrites files in place, and a
  // hardlinked snapshot would be rewritten with them.
  bool ok = true;
  for (const char* f : {"system.reg", "user.reg", "userdef.reg", ".kretro-wine-version"}) {
    if (!fs::exists(prefix / f, ec)) continue;
    ok = ok && clone_or_copy(prefix / f, dst / f, fs::perms::owner_read | fs::perms::owner_write);
  }
  if (fs::exists(prefix / "drive_c" / "users", ec)) {
    ok = ok && copy_tree(prefix / "drive_c" / "users", dst / "drive_c" / "users");
  }
  if (!ok) {
    fs::remove_all(dst, ec);
    throw std::runtime_error("could not snapshot the prefix before upgrading it; nothing was changed");
  }
  return dst;
}

PrefixResult ensure_prefix(const rt::Env& e, const fs::path& prefix, const fs::path& snapshots,
                           const std::function<void(const std::string&)>& say) {
  PrefixResult r;
  const fs::path tmpl = template_dir(e);
  std::error_code ec;
  const bool exists = fs::exists(prefix / "system.reg", ec);
  // A half-seeded prefix has no system.reg and is seeded again; anything
  // else left there by a crash is not a prefix.
  r.action = prefix_action(tmpl.empty() ? std::nullopt : read_stamp(tmpl), read_stamp(prefix), exists);

  if (r.action == PrefixAction::Seed) {
    say("making this game's Wine prefix");
    seed_prefix(tmpl, prefix);
  } else if (r.action == PrefixAction::Upgrade) {
    say("this player carries a newer Wine (" + read_stamp(tmpl).value_or("?") +
        "); keeping a copy of the old prefix's settings first");
    r.snapshot = snapshot_prefix(prefix, snapshots);
    fs::path wine = rt::find_wine(e.root);
    // No display, or wineboot's "being updated" dialog comes up on the host's
    // desktop; see rt::offscreen.
    rt::Env we = rt::offscreen(e);
    we.set("WINEPREFIX", prefix.string());
    we.set("WINEDLLOVERRIDES", "mscoree,mshtml=");
    ProcOptions po;
    po.timeout_sec = 600;
    ProcResult pr = rt::run(we, wine, {"wineboot", "-u"}, po);
    if (!pr.ok()) {
      throw std::runtime_error("could not upgrade this game's Wine prefix; the old settings are in " +
                               r.snapshot.string() + "\n" + pr.out);
    }
    // wineboot -u of this Wine may have put a z: back.
    fs::remove(prefix / "dosdevices" / "z:", ec);
    std::ifstream in(tmpl / ".kretro-wine-version");
    std::ofstream(prefix / ".kretro-wine-version", std::ios::trunc) << in.rdbuf();
  }
  return r;
}

namespace {

// REGEDIT4 escapes backslashes and quotes inside a quoted string.
std::string reg_quote(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') o.push_back('\\');
    o.push_back(c);
  }
  return o + "\"";
}

}  // namespace

std::string key_registry(const bundle::GameMeta::Key& k) {
  std::string path = k.registry_path;
  while (!path.empty() && (path.front() == '\\' || path.front() == '/')) path.erase(path.begin());
  while (!path.empty() && (path.back() == '\\' || path.back() == '/')) path.pop_back();
  for (char& c : path) {
    if (c == '/') c = '\\';
  }
  if (path.empty()) throw std::runtime_error("the embedded key does not say where in the registry it goes");
  if (k.registry_value.empty()) throw std::runtime_error("the embedded key has no value name");
  if (k.registry_path.find('\n') != std::string::npos || k.registry_value.find('\n') != std::string::npos ||
      k.value.find('\n') != std::string::npos) {
    throw std::runtime_error("the embedded key has a line break in it");
  }

  static const std::pair<const char*, const char*> kHives[] = {
      {"HKEY_LOCAL_MACHINE", "HKEY_LOCAL_MACHINE"}, {"HKLM", "HKEY_LOCAL_MACHINE"},
      {"HKEY_CURRENT_USER", "HKEY_CURRENT_USER"},   {"HKCU", "HKEY_CURRENT_USER"},
      {"HKEY_USERS", "HKEY_USERS"},                 {"HKU", "HKEY_USERS"},
      {"HKEY_CLASSES_ROOT", "HKEY_CLASSES_ROOT"},   {"HKCR", "HKEY_CLASSES_ROOT"},
  };
  std::string first = path.substr(0, path.find('\\'));
  std::string rest = path.find('\\') == std::string::npos ? "" : path.substr(path.find('\\') + 1);
  std::string hive;
  for (const auto& [name, full] : kHives) {
    if (to_upper(first) == name) hive = full;
  }
  if (hive.empty()) {
    hive = "HKEY_LOCAL_MACHINE";
    rest = path;
  }
  if (rest.empty()) throw std::runtime_error("the embedded key names a hive and no key in it");

  // A 32-bit game under WoW64 reads HKLM\Software through Wow6432Node, and
  // regedit here is the 64-bit one, which writes the path as given. So the
  // key goes where the game's half of the registry is. HKCU is shared by both
  // halves, Software\Classes is merged by Wine, and a path the author already
  // wrote with Wow6432Node in it is already there.
  if (k.view == "32" && hive == "HKEY_LOCAL_MACHINE") {
    const std::string u = to_upper(rest);
    const std::string sw = "SOFTWARE\\";
    if (u.rfind(sw, 0) == 0 && u.rfind(sw + "WOW6432NODE\\", 0) != 0 && u.rfind(sw + "CLASSES\\", 0) != 0) {
      rest = rest.substr(0, sw.size()) + "Wow6432Node\\" + rest.substr(sw.size());
    }
  }

  std::string name = k.registry_value == "@" ? "@" : reg_quote(k.registry_value);
  return "REGEDIT4\n\n[" + hive + "\\" + rest + "]\n" + name + "=" + reg_quote(k.value) + "\n";
}

void drop_host_device_links(const fs::path& prefix) {
  std::error_code dl;
  for (const fs::directory_entry& de : fs::directory_iterator(prefix / "dosdevices", dl)) {
    const std::string n = de.path().filename().string();
    if (n.size() == 3 && n.substr(1) == "::") fs::remove(de.path(), dl);
  }
}

void apply_embedded_key(const rt::Env& wine_env, const fs::path& prefix, const bundle::GameMeta::Key& key,
                        const std::function<void(const std::string&)>& say) {
  std::string reg = key_registry(key);
  fs::path marker = prefix / ".kretro-key";
  std::string want = to_hex(hash_string(reg));
  std::string have;
  std::ifstream(marker) >> have;
  if (have != want) {
    std::ofstream(prefix / "drive_c" / ".kretro-key.reg", std::ios::trunc) << reg;
    ProcResult r =
        rt::run(rt::offscreen(wine_env), rt::find_wine(wine_env.root), {"regedit", "/S", "C:\\.kretro-key.reg"});
    std::error_code ec;
    fs::remove(prefix / "drive_c" / ".kretro-key.reg", ec);
    if (!r.ok()) throw std::runtime_error("could not put the author's key into the registry");
    std::ofstream(marker, std::ios::trunc) << want << "\n";
    say("the author's key is in the registry");
  }
}

std::string place_extra_files(const std::vector<bundle::GameMeta::Dll>& files,
                              const fs::path& game_dir) {
  std::string overrides;
  std::error_code ec;
  for (const bundle::GameMeta::Dll& f : files) {
    // bundle.meta refuses anything but one plain component; this is the line
    // that writes, so it looks again.
    if (f.name.empty() || f.name.find('/') != std::string::npos || f.name == "." || f.name == "..") {
      continue;
    }
    fs::path dst = game_dir / f.name;
    bool same = false;
    if (fs::exists(dst, ec) && fs::file_size(dst, ec) == f.data.size()) {
      std::ifstream in(dst, std::ios::binary);
      std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      same = have == f.data;
    }
    if (!same) {
      std::ofstream out(dst, std::ios::binary | std::ios::trunc);
      out << f.data;
      if (!out) throw std::runtime_error("could not put " + f.name + " beside the game");
    }
    std::string lower = to_lower(f.name);
    if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".dll") {
      overrides += (overrides.empty() ? "" : ";") + lower.substr(0, lower.size() - 4) + "=n,b";
    }
  }
  return overrides;
}

std::string exe_dir_in_tree(const Meta& m) {
  std::string want = m.run.exe;
  std::replace(want.begin(), want.end(), '\\', '/');
  while (!want.empty() && want.front() == '/') want.erase(want.begin());
  std::string path = want;
  for (const TreeEntry& e : m.tree.entries()) {
    if (e.is_regular() && iequals(e.path, want)) {
      path = e.path;
      break;
    }
  }
  size_t slash = path.rfind('/');
  if (slash == std::string::npos) return "";
  std::string dir = path.substr(0, slash);
  // Never out of the game: a run.exe of "../x.exe" was refused when the pack
  // was read, and is refused again where it would decide where a file goes.
  for (const fs::path& part : fs::path(dir)) {
    if (part == ".." || part == ".") return "";
  }
  return dir;
}

std::string stage_extra_layer(const std::vector<bundle::GameMeta::Dll>& files, const std::string& exe_dir,
                              const fs::path& layer) {
  std::error_code ec;
  if (files.empty()) {
    fs::remove_all(layer, ec);
    return "";
  }
  const fs::path dir = exe_dir.empty() ? layer : layer / exe_dir;
  // What a newer build of the bundle no longer carries, or a game whose
  // executable moved, is taken out: the layer is exactly this bundle's files.
  std::vector<fs::path> stale;
  for (auto it = fs::recursive_directory_iterator(layer, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_directory(ec)) continue;
    const fs::path& p = it->path();
    bool wanted = p.parent_path() == dir &&
                  std::any_of(files.begin(), files.end(), [&](const bundle::GameMeta::Dll& f) {
                    return p.filename() == f.name;
                  });
    if (!wanted) stale.push_back(p);
  }
  for (const fs::path& p : stale) fs::remove(p, ec);
  fs::create_directories(dir, ec);
  if (ec) throw std::runtime_error("cannot make a place for the author's files at " + dir.string());
  return place_extra_files(files, dir);
}

}  // namespace kg::player
