#include "env.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "../gpu/probe.h"
#include "../util/paths.h"

namespace kg::rt {
namespace fs = std::filesystem;

void Env::set(const std::string& key, const std::string& value) {
  for (auto& kv : vars) {
    if (kv.first == key) {
      kv.second = value;
      return;
    }
  }
  vars.emplace_back(key, value);
}

std::string Env::get(const std::string& key) const {
  std::string value;
  for (const auto& kv : vars) {
    if (kv.first == key) value = kv.second;
  }
  return value;
}

void Env::append(const std::string& key, const std::string& value, char sep) {
  const std::string cur = get(key);
  set(key, cur.empty() ? value : cur + sep + value);
}

fs::path find_wine(const fs::path& root) {
  for (const char* c : {"opt/wine-staging/bin/wine", "opt/wine-devel/bin/wine",
                        "opt/wine-stable/bin/wine", "usr/bin/wine"}) {
    std::error_code ec;
    if (fs::exists(root / c, ec)) return root / c;
  }
  return {};
}

// Only when the runtime carries it: a variable naming a file that is not
// there is worse than none, because the library then fails rather than
// falling back to its default.
static void set_if_there(Env& e, const char* key, const char* rel) {
  std::error_code ec;
  if (fs::exists(e.root / rel, ec)) e.set(key, (e.root / rel).string());
}

Env make(const gpu::Report* gl) {
  Env e;
  e.root = runtime_dir();
  if (e.root.empty()) return e;

  // Ours first. The host's graphics driver, when there is one, comes last and
  // is the only thing we ever take from outside.
  std::vector<std::string> libs = {
      (e.root / "lib").string(),
      (e.root / "lib/x86_64-linux-gnu").string(),
      (e.root / "usr/lib/x86_64-linux-gnu").string(),
      (e.root / "usr/lib").string(),
      (e.root / "lib64").string(),
      // Private directories that are on no standard search path. Weston keeps
      // libexec_weston.so.0 in one; pulseaudio keeps libpulsecommon in
      // another, which SDL needs and which fails as a puzzling error from
      // somewhere else entirely.
      (e.root / "usr/lib/x86_64-linux-gnu/weston").string(),
      (e.root / "usr/lib/x86_64-linux-gnu/libweston-14").string(),
      (e.root / "usr/lib/x86_64-linux-gnu/pulseaudio").string(),
  };
  if (gl && !gl->library_path.empty()) libs.push_back(gl->library_path);
  for (const std::string& l : libs) {
    if (!e.library_path.empty()) e.library_path += ":";
    e.library_path += l;
  }

  e.set("PATH", (e.root / "usr/bin").string() + ":" + (e.root / "bin").string() + ":/usr/bin:/bin");
  // Each of these overrides a path that was compiled into something and no
  // longer exists once the tree moved.
  e.set("LIBGL_DRIVERS_PATH", (e.root / "usr/lib/x86_64-linux-gnu/dri").string());
  e.set("XKB_CONFIG_ROOT", (e.root / "usr/share/X11/xkb").string());
  e.set("FONTCONFIG_PATH", (e.root / "etc/fonts").string());
  e.set("XDG_DATA_DIRS", (e.root / "usr/share").string());
  e.set("LD_LIBRARY_PATH", e.library_path);

  // Wine works out where its own parts live from /proc/self/exe, which is the
  // loader rather than the wine binary when we exec through the loader. Told
  // directly, everything else relocates off it.
  fs::path wine = find_wine(e.root);
  if (!wine.empty()) {
    fs::path base = wine.parent_path().parent_path();
    e.set("WINELOADER", wine.string());
    e.set("WINEDLLPATH", (base / "lib" / "wine").string());
    std::error_code ec;
    if (fs::exists(wine.parent_path() / "wineserver", ec)) {
      e.set("WINESERVER", (wine.parent_path() / "wineserver").string());
    }
    if (!std::getenv("WINEDEBUG")) e.set("WINEDEBUG", "-all");
  }

  // Weston resolves its backends and shells against a directory that was
  // compiled in and no longer exists once the tree moves. WESTON_MODULE_MAP
  // redirects each one by name to the copy we actually ship.
  {
    std::error_code ec;
    std::string map;
    for (const char* dir : {"usr/lib/x86_64-linux-gnu/libweston-14",
                            "usr/lib/x86_64-linux-gnu/weston"}) {
      fs::path d = e.root / dir;
      if (!fs::exists(d, ec)) continue;
      for (const fs::directory_entry& de : fs::directory_iterator(d, ec)) {
        std::string name = de.path().filename().string();
        if (name.size() < 3 || name.substr(name.size() - 3) != ".so") continue;
        map += name + "=" + de.path().string() + ";";
      }
    }
    if (!map.empty()) e.set("WESTON_MODULE_MAP", map);
  }

  // alsa-lib reads /usr/share/alsa/alsa.conf off the host unless told
  // otherwise, and that file can pull in any plugin the host's distribution
  // ships - built against a different libc, loaded into our process. Ours
  // names hw and dmix and nothing else.
  set_if_there(e, "ALSA_CONFIG_PATH", "usr/share/kretro/alsa/asound.conf");
  // GStreamer is how Wine plays the cutscenes of the era: its plugins, and
  // the scanner it forks to inspect them, are ours. The registry it builds is
  // a cache of that scan, kept with the other caches so it is not rebuilt on
  // every start - and not under the host's ~/.cache/gstreamer-1.0, where a
  // registry written by the host's GStreamer would be read by ours.
  set_if_there(e, "GST_PLUGIN_SYSTEM_PATH_1_0", "usr/lib/x86_64-linux-gnu/gstreamer-1.0");
  set_if_there(e, "GST_PLUGIN_SCANNER_1_0",
               "usr/lib/x86_64-linux-gnu/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner");
  e.set("GST_REGISTRY_1_0", (cache_dir() / "gstreamer-registry.x86_64.bin").string());
  // The prefix template was made as "player", and a prefix names its user
  // directory after whoever Wine thinks is running it. Left to the host's
  // name, the first start would make C:\users\<you> beside the template's
  // C:\users\player, the profile the registry points at would be the empty
  // one, and a save a game kept in My Documents would move with the machine.
  // One fixed name is also one fewer place a person's login ends up in a
  // saves export. A prefix kretro made before this named the profile after
  // the person; session::adopt_player_profile moves that one over, once.
  e.set("WINEUSERNAME", "player");
  e.set("USER", "player");

  if (gl) {
    for (const auto& kv : gl->env) e.set(kv.first, kv.second);
  }
  return e;
}

namespace {

// True for a real ELF; false for a shell script or anything else.
bool is_elf(const fs::path& p) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return false;
  char m[4] = {};
  size_t n = std::fread(m, 1, 4, f);
  std::fclose(f);
  return n == 4 && std::memcmp(m, "\x7f" "ELF", 4) == 0;
}

}  // namespace

// The interpreter a #! line names, remapped into the runtime. Returns an empty
// path for a binary, or when the script names an interpreter we do not carry.
static fs::path shebang(const Env& e, const fs::path& p, std::string* arg) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return {};
  char buf[256] = {};
  size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  if (n < 3 || buf[0] != '#' || buf[1] != '!') return {};

  std::string line(buf, n);
  size_t nl = line.find('\n');
  if (nl != std::string::npos) line = line.substr(0, nl);
  line = line.substr(2);
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());

  std::string interp = line, extra;
  size_t sp = line.find(' ');
  if (sp != std::string::npos) {
    interp = line.substr(0, sp);
    extra = line.substr(sp + 1);
    while (!extra.empty() && extra.back() == ' ') extra.pop_back();
  }
  if (arg) *arg = extra;

  std::error_code ec;
  // The script names a host path; the same program in our tree is what has to
  // run, or nothing about the runtime is self-contained.
  fs::path mapped = e.root / fs::path(interp).relative_path();
  if (fs::exists(mapped, ec)) return mapped;
  fs::path sh = e.root / "bin" / "sh";
  if (fs::exists(sh, ec)) return sh;
  return {};
}

// A wrapper such as Ubuntu's /usr/bin/7z is two lines: a #! and an exec of an
// absolute path. Run as-is inside a relocated tree that exec would reach the
// *host's* copy, so the wrapper is followed here and the real binary in our
// tree is used instead.
static fs::path follow_wrapper(const Env& e, const fs::path& script) {
  std::ifstream f(script);
  if (!f) return {};
  std::string line;
  int lines = 0;
  while (std::getline(f, line) && ++lines < 20) {
    size_t pos = line.find("exec ");
    if (pos == std::string::npos) continue;
    std::string rest = line.substr(pos + 5);
    while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
    size_t end = rest.find_first_of(" \t\"");
    std::string target = end == std::string::npos ? rest : rest.substr(0, end);
    if (target.empty() || target[0] != '/') continue;
    std::error_code ec;
    fs::path mapped = e.root / fs::path(target).relative_path();
    if (fs::exists(mapped, ec) && is_elf(mapped)) return mapped;
  }
  return {};
}

fs::path which(const Env& e, const std::string& name) {
  std::error_code ec;
  std::vector<fs::path> dirs = {e.root / "usr/bin", e.root / "bin", e.root / "usr/sbin",
                                e.root / "sbin"};
  fs::path wine = find_wine(e.root);
  if (!wine.empty()) dirs.push_back(wine.parent_path());
  // Some packages put the real binary under lib and ship a wrapper in bin.
  for (const char* d : {"usr/lib/7zip", "usr/lib/p7zip", "usr/libexec"}) {
    dirs.push_back(e.root / d);
  }

  fs::path script_fallback;
  for (const fs::path& d : dirs) {
    fs::path p = d / name;
    if (!fs::exists(p, ec)) continue;
    if (is_elf(p)) return p;
    if (fs::path real = follow_wrapper(e, p); !real.empty()) return real;
    if (script_fallback.empty()) script_fallback = p;  // a genuine script
  }
  return script_fallback;
}

static std::vector<std::string> loader_argv(const Env& e, const fs::path& prog,
                                            const std::vector<std::string>& args) {
  std::vector<std::string> v;
  v.push_back(e.loader().string());
  v.push_back("--library-path");
  v.push_back(e.library_path);

  // A script cannot be handed to the loader; its interpreter can, with the
  // script as an argument. winetricks and several extraction wrappers are
  // shell scripts, so this is not a corner case.
  if (!is_elf(prog)) {
    std::string extra;
    fs::path interp = shebang(e, prog, &extra);
    if (!interp.empty()) {
      v.push_back(interp.string());
      if (!extra.empty()) v.push_back(extra);
    }
  }
  v.push_back(prog.string());
  for (const std::string& a : args) v.push_back(a);
  return v;
}

namespace {

// What run and run_unnamed check first: a runtime, its loader and the program.
// When one is missing, says so in `r` and returns false.
bool runnable(const Env& e, const fs::path& prog, ProcResult* r) {
  std::error_code ec;
  if (!e.valid() || !fs::exists(e.loader(), ec) || !fs::exists(prog, ec)) {
    r->out = "not present in this runtime: " + prog.string();
    return false;
  }
  return true;
}

}  // namespace

ProcResult run(const Env& e, const fs::path& prog, const std::vector<std::string>& args,
               ProcOptions opt) {
  ProcResult r;
  if (!runnable(e, prog, &r)) return r;
  for (const auto& kv : e.vars) opt.env.push_back(kv);
  return kg::run(loader_argv(e, prog, args), opt);
}

ProcResult run_unnamed(const Env& e, const fs::path& prog, const std::vector<std::string>& args,
                       ProcOptions opt) {
  ProcResult r;
  if (!runnable(e, prog, &r)) return r;
  // Linux 6.3 added MFD_EXEC, and a kernel with vm.memfd_noexec=1 makes a
  // memfd not executable unless asked; older kernels refuse the flag, and
  // every memfd of theirs is executable anyway. Close-on-exec is fine for an
  // ELF: the kernel has the file open before it closes the descriptor.
  int fd = static_cast<int>(::syscall(SYS_memfd_create, "kretro-loader", MFD_CLOEXEC | 0x0010U));
  if (fd < 0 && errno == EINVAL) fd = static_cast<int>(::syscall(SYS_memfd_create, "kretro-loader", MFD_CLOEXEC));
  if (fd < 0) {
    r.out = std::string("cannot make an in-memory loader: ") + std::strerror(errno);
    return r;
  }
  int in = ::open(e.loader().c_str(), O_RDONLY | O_CLOEXEC);
  bool copied = in >= 0;
  char buf[1 << 16];
  ssize_t n;
  while (copied && (n = ::read(in, buf, sizeof(buf))) != 0) {
    if (n < 0 || ::write(fd, buf, static_cast<size_t>(n)) != n) copied = false;
  }
  if (in >= 0) ::close(in);
  if (!copied) {
    ::close(fd);
    r.out = "cannot copy the loader into memory: " + e.loader().string();
    return r;
  }
  for (const auto& kv : e.vars) opt.env.push_back(kv);
  std::vector<std::string> argv = loader_argv(e, prog, args);
  argv[0] = "/proc/self/fd/" + std::to_string(fd);
  r = kg::run(argv, opt);
  ::close(fd);
  return r;
}

std::string confinement() {
  for (const char* f : {"/proc/self/attr/apparmor/current", "/proc/self/attr/current"}) {
    std::ifstream in(f);
    std::string label;
    if (!in || !std::getline(in, label)) continue;
    while (!label.empty() && (label.back() == '\n' || label.back() == '\0')) label.pop_back();
    // SELinux answers the second file too, with a context that is not an
    // AppArmor label and has nothing to do with fusermount3's profile.
    if (label.empty() || label == "unconfined" || label.find(':') != std::string::npos) return "";
    return label;
  }
  return "";
}

void exec(const Env& e, const fs::path& prog, const std::vector<std::string>& args) {
  std::error_code ec;
  if (!e.valid()) {
    std::fprintf(stderr, "kretro: no runtime (KRETRO_RUNTIME is unset)\n");
    std::exit(1);
  }
  if (!fs::exists(e.loader(), ec)) {
    std::fprintf(stderr, "kretro: the runtime has no loader at %s\n", e.loader().c_str());
    std::exit(1);
  }
  if (!fs::exists(prog, ec)) {
    std::fprintf(stderr, "kretro: %s is not in this runtime\n", prog.c_str());
    std::exit(1);
  }
  for (const auto& [k, v] : e.vars) setenv(k.c_str(), v.c_str(), 1);

  std::vector<std::string> owned = loader_argv(e, prog, args);
  std::vector<char*> argv;
  argv.reserve(owned.size() + 1);
  for (std::string& s : owned) argv.push_back(s.data());
  argv.push_back(nullptr);
  execv(owned[0].c_str(), argv.data());
  std::fprintf(stderr, "kretro: cannot exec %s: %s\n", prog.c_str(), std::strerror(errno));
  std::exit(1);
}

void confine_home(Env& e, const fs::path& home) {
  e.set("HOME", home.string());
  e.set("XDG_CONFIG_HOME", (home / ".config").string());
  e.set("XDG_DATA_HOME", (home / ".local" / "share").string());
  e.set("XDG_CACHE_HOME", (home / ".cache").string());
  e.set("XDG_STATE_HOME", (home / ".local" / "state").string());
}

Env wine_env(const Env& base, const fs::path& prefix, const fs::path& home) {
  Env we = base;
  we.set("WINEPREFIX", prefix.string());
  confine_home(we, home);
  return we;
}

}  // namespace kg::rt
