#include "caps.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <tuple>

namespace kg::gpu {
namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// "key = value" or "key: value", as vulkaninfo and glxinfo print them.
bool field(const std::string& line, const std::string& key, char sep, std::string* value) {
  std::string t = trim(line);
  if (t.rfind(key, 0) != 0) return false;
  size_t p = t.find(sep, key.size());
  if (p == std::string::npos) return false;
  // Only whitespace may sit between the key and its separator, or
  // "deviceNameOverride" would be read as deviceName.
  for (size_t i = key.size(); i < p; ++i) {
    if (t[i] != ' ' && t[i] != '\t') return false;
  }
  *value = trim(t.substr(p + 1));
  return true;
}

struct Ver {
  int major = 0, minor = 0, patch = 0;
  bool ok = false;
  bool operator<(const Ver& o) const {
    return std::tie(major, minor, patch) < std::tie(o.major, o.minor, o.patch);
  }
};

// The first x.y.z on the line. Older vulkaninfo prints "4206847 (1.3.255)",
// the packed integer and then the version; the packed one has no dots.
Ver version_in(const std::string& s) {
  static const std::regex re(R"((\d+)\.(\d+)\.(\d+))");
  std::smatch m;
  Ver v;
  if (std::regex_search(s, m, re)) {
    v.major = std::stoi(m[1]);
    v.minor = std::stoi(m[2]);
    v.patch = std::stoi(m[3]);
    v.ok = true;
  }
  return v;
}

VulkanLevel level_of(const Ver& v) {
  if (!v.ok || v.major < 1) return VulkanLevel::None;
  if (v.major > 1 || v.minor >= 4) return VulkanLevel::V1_4;
  if (v.minor == 3) return VulkanLevel::V1_3;
  return VulkanLevel::Legacy;
}

bool readable(const fs::path& p, int mode) { return ::access(p.c_str(), mode) == 0; }

}  // namespace

const char* level_name(VulkanLevel l) {
  switch (l) {
    case VulkanLevel::V1_4: return "1.4";
    case VulkanLevel::V1_3: return "1.3";
    case VulkanLevel::Legacy: return "below 1.3";
    default: return "none";
  }
}

const char* session_name(SessionType s) {
  switch (s) {
    case SessionType::Wayland: return "wayland";
    case SessionType::X11: return "x11";
    default: return "none";
  }
}

VulkanInfo parse_vulkaninfo_summary(const std::string& text) {
  VulkanInfo out;
  if (trim(text).empty()) return out;
  out.ran = true;

  Ver instance;
  struct Dev {
    Ver api;
    std::string name, driver;
    bool cpu = false;
  };
  std::vector<Dev> devs;
  static const std::regex gpu_head(R"(^GPU\d+:\s*$)");

  std::istringstream in(text);
  std::string line, v;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (t.rfind("Vulkan Instance Version", 0) == 0) {
      instance = version_in(t);
    } else if (std::regex_match(t, gpu_head)) {
      devs.emplace_back();
    } else if (devs.empty()) {
      continue;
    } else if (field(t, "apiVersion", '=', &v)) {
      devs.back().api = version_in(v);
    } else if (field(t, "deviceType", '=', &v)) {
      devs.back().cpu = v.find("CPU") != std::string::npos;
    } else if (field(t, "deviceName", '=', &v)) {
      devs.back().name = v;
    } else if (field(t, "driverName", '=', &v)) {
      devs.back().driver = v;
    }
  }

  const Dev* best = nullptr;
  Ver best_v;
  for (const Dev& d : devs) {
    if (d.cpu || !d.api.ok) continue;
    // A device can speak no newer Vulkan than the loader offers; the loader
    // here is the runtime's, so this only ever lowers an answer.
    Ver eff = (instance.ok && instance < d.api) ? instance : d.api;
    if (!best || best_v < eff) {
      best = &d;
      best_v = eff;
    }
  }
  if (best) {
    out.level = level_of(best_v);
    out.api_version = std::to_string(best_v.major) + "." + std::to_string(best_v.minor) + "." +
                      std::to_string(best_v.patch);
    out.device = best->name;
    out.driver = best->driver;
  } else {
    out.software_only = !devs.empty();
  }
  return out;
}

GlInfo parse_glxinfo(const std::string& text) {
  GlInfo g;
  std::istringstream in(text);
  std::string line, v;
  while (std::getline(in, line)) {
    if (field(line, "OpenGL renderer string", ':', &v)) g.renderer = v;
    else if (field(line, "OpenGL version string", ':', &v)) g.version = v;
    else if (g.version.empty() && field(line, "OpenGL core profile version string", ':', &v)) g.version = v;
  }
  if (g.renderer.empty()) return g;
  g.ran = true;
  std::string r = lower(g.renderer);
  g.hardware = r.find("llvmpipe") == std::string::npos && r.find("softpipe") == std::string::npos &&
               r.find("swrast") == std::string::npos && r.find("software rasterizer") == std::string::npos;
  return g;
}

bool HostCaps::gpu_usable() const {
  const bool nvidia_broken = nvidia == NvidiaState::Missing || nvidia == NvidiaState::Mismatch;
  if (nvidia_broken && !other_gpu) return false;
  if (vulkan.ran || gl.ran) return vulkan.level != VulkanLevel::None || gl.hardware;
  // Neither probe could run - no runtime, no display. The device nodes are
  // the best evidence left, and a false "no GPU" would refuse games that
  // would have played.
  return render_device;
}

bool detect_gamescope(const Host& h) {
  if (!h.env("GAMESCOPE_WAYLAND_DISPLAY").empty()) return true;
  // A colon-separated list, as the desktop entry spec defines it.
  std::stringstream ss(h.env("XDG_CURRENT_DESKTOP"));
  std::string part;
  while (std::getline(ss, part, ':')) {
    if (lower(trim(part)) == "gamescope") return true;
  }
  return false;
}

SessionType detect_session(const Host& h) {
  std::string t = lower(h.env("XDG_SESSION_TYPE"));
  if (t == "wayland") return SessionType::Wayland;
  if (t == "x11") return SessionType::X11;
  // Unset, or "tty" from a terminal that still has a display forwarded to it:
  // what is reachable is what counts.
  if (!h.env("WAYLAND_DISPLAY").empty()) return SessionType::Wayland;
  if (!h.env("DISPLAY").empty()) return SessionType::X11;
  return SessionType::None;
}

HostCaps probe_caps(const Host& h, const Report& gpu, const Probes& probes) {
  HostCaps c;
  std::error_code ec;

  c.nvidia = gpu.nvidia.state;
  for (const Device& d : gpu.devices) {
    if (!d.readable) continue;
    c.render_device = true;
    // nouveau is Mesa's, and works whatever the proprietary libraries say.
    if (d.vendor != Vendor::Nvidia || d.driver == "nouveau") c.other_gpu = true;
  }

  if (probes.vulkaninfo) {
    if (auto t = probes.vulkaninfo()) c.vulkan = parse_vulkaninfo_summary(*t);
  }
  if (probes.glxinfo) {
    if (auto t = probes.glxinfo()) c.gl = parse_glxinfo(*t);
  }

  // Display.
  c.session = detect_session(h);
  c.desktop = h.env("XDG_CURRENT_DESKTOP");
  c.gamescope = detect_gamescope(h);

  // Sound. Wine's pulse driver talks the PulseAudio protocol, which
  // pipewire-pulse speaks as well; the socket is what says someone is
  // listening.
  const std::string xdg_runtime = h.env("XDG_RUNTIME_DIR");
  bool pipewire = false;
  if (std::string ps = h.env("PULSE_SERVER"); !ps.empty()) {
    if (ps.rfind("unix:", 0) == 0) c.pulse = fs::exists(h.at(ps.substr(5)), ec);
    else c.pulse = true;  // a network server; nothing local to check
  } else if (!xdg_runtime.empty()) {
    c.pulse = fs::exists(h.at(fs::path(xdg_runtime) / "pulse" / "native"), ec);
  }
  if (!xdg_runtime.empty()) pipewire = fs::exists(h.at(fs::path(xdg_runtime) / "pipewire-0"), ec);
  c.audio_server = c.pulse ? (pipewire ? "pipewire-pulse" : "pulseaudio")
                           : (pipewire ? "pipewire, without its pulse socket" : "none");

  // Input. Wine's winebus reads gamepads through SDL, which reads their
  // /dev/input/event node. Only gamepads count: a keyboard's or a mouse's
  // node is root-only on every desktop, and a report that found those
  // unreadable would be a warning on every machine there is. udev names a
  // gamepad's node *-event-joystick under by-id.
  {
    fs::path by_id = h.at("/dev/input/by-id");
    for (const fs::directory_entry& de : fs::directory_iterator(by_id, ec)) {
      std::string n = de.path().filename().string();
      const std::string tail = "-event-joystick";
      if (n.size() < tail.size() || n.compare(n.size() - tail.size(), tail.size(), tail) != 0) continue;
      ++c.input_nodes;
      if (readable(de.path(), R_OK)) ++c.input_readable;
    }
    ec.clear();
  }
  // hidraw is only a nicety with DisableHidraw set, and is root-only on most
  // machines, so it is reported and never a problem.
  for (const fs::directory_entry& de : fs::directory_iterator(h.at("/dev"), ec)) {
    if (de.path().filename().string().rfind("hidraw", 0) != 0) continue;
    ++c.hidraw_nodes;
    if (readable(de.path(), R_OK | W_OK)) ++c.hidraw_usable;
  }
  ec.clear();

  // FUSE.
  c.fuse_device = readable(h.at("/dev/fuse"), R_OK | W_OK);
  for (const char* f : {"/usr/bin/fusermount3", "/bin/fusermount3", "/usr/local/bin/fusermount3",
                        "/usr/bin/fusermount", "/bin/fusermount"}) {
    if (fs::exists(h.at(f), ec)) {
      c.fusermount = true;
      break;
    }
  }
  c.mount_mode = h.env("KRETRO_MOUNT_MODE");

  // What to say about it. The NVIDIA and display problems belong to the GPU
  // report and are said there; these are the rest.
  using S = Problem::Severity;
  if (c.gpu_usable() && c.vulkan.ran && c.vulkan.level < VulkanLevel::V1_3) {
    c.problems.push_back(
        {S::Warning,
         std::string("The graphics driver offers no Vulkan 1.3") +
             (c.vulkan.level == VulkanLevel::Legacy ? " (it has " + c.vulkan.api_version + ")" : ""),
         "Direct3D 8 and 9 games use WineD3D instead of DXVK, which is slower. A newer graphics "
         "driver may add it"});
  }
  if (!c.gpu_usable() && c.nvidia == NvidiaState::Absent) {
    c.problems.push_back({S::Warning, "No GPU can be used: games draw with software rendering",
                          "Check that a graphics driver is installed and that you are in the render "
                          "and video groups, then log in again"});
  }
  if (!c.pulse) {
    c.problems.push_back({S::Warning, "No PulseAudio or PipeWire sound server is running for you",
                          "Games will try the sound card directly and may be silent. Start "
                          "pipewire-pulse or pulseaudio (usually: systemctl --user start pipewire-pulse)"});
  }
  if (c.input_nodes > 0 && c.input_readable == 0) {
    c.problems.push_back({S::Warning, "A gamepad is connected but you cannot read it",
                          "Install your distribution's game controller udev rules (often called "
                          "steam-devices) or join the input group, then log in again"});
  }
  if (!c.fuse()) {
    c.problems.push_back(
        {S::Warning,
         std::string("FUSE is not available (") +
             (!c.fuse_device ? "/dev/fuse cannot be opened" : "no fusermount3 on this system") + ")",
         "Each game is unpacked to ~/.cache before it plays, which needs space. Installing fuse3 "
         "avoids that"});
  }
  return c;
}

}  // namespace kg::gpu
