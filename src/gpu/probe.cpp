#include "probe.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include <unistd.h>

#include "../util/paths.h"

namespace kg::gpu {
namespace fs = std::filesystem;

namespace {

std::string read_first_line(const fs::path& p) {
  std::ifstream f(p);
  std::string s;
  std::getline(f, s);
  return s;
}

std::string trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

Vendor vendor_from_pci(uint32_t id) {
  switch (id) {
    case 0x10de: return Vendor::Nvidia;
    case 0x1002: return Vendor::Amd;
    case 0x8086: return Vendor::Intel;
    default: return Vendor::Unknown;
  }
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const std::string& x : v) {
    if (!s.empty()) s += ":";
    s += x;
  }
  return s;
}

// Copies the runtime's own manifests for one loader into `into`, with any
// absolute library path moved inside the runtime. Debian writes Mesa's Vulkan
// manifests with /usr/lib/x86_64-linux-gnu/libvulkan_radeon.so in them, and
// read as-is that names the host's copy, not ours.
std::vector<std::string> adopt_runtime(const fs::path& runtime_root, const fs::path& from,
                                       const fs::path& into) {
  std::vector<std::string> out;
  std::error_code ec;
  fs::path dir = runtime_root / from;
  if (!fs::is_directory(dir, ec)) return out;
  std::vector<fs::path> files;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    std::string name = de.path().filename().string();
    if (name.size() < 5 || name.substr(name.size() - 5) != ".json") continue;
    if (name.find("i686") != std::string::npos || name.find("i386") != std::string::npos) continue;
    files.push_back(de.path());
  }
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    fs::remove_all(into, ec);
    return out;
  }
  fs::create_directories(into, ec);
  std::set<std::string> written;
  for (const fs::path& f : files) {
    std::ifstream in(f);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string json = ss.str();
    static const std::regex re(R"RX("library_path"\s*:\s*"([^"]*)")RX");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
      fs::path lib = m[1].str();
      if (lib.is_absolute()) {
        fs::path ours = runtime_root / lib.relative_path();
        // A path the runtime does not have becomes a bare name, which the
        // loader then looks for on our library path and nowhere else.
        std::string to = fs::exists(ours, ec) ? ours.string() : lib.filename().string();
        json = std::regex_replace(json, re, "\"library_path\": \"" + to + "\"");
      }
    }
    fs::path dst = into / f.filename();
    replace_file(dst, json);
    written.insert(f.filename().string());
    out.push_back(dst.string());
  }
  keep_only(into, written);
  return out;
}

}  // namespace

const char* vendor_name(Vendor v) {
  switch (v) {
    case Vendor::Nvidia: return "NVIDIA";
    case Vendor::Amd: return "AMD";
    case Vendor::Intel: return "Intel";
    case Vendor::Software: return "software";
    default: return "unknown";
  }
}

Report probe() { return probe(Host::system()); }

Report probe(const Host& h) {
  Report r;
  using S = Problem::Severity;
  std::error_code ec;

  // Display first: without one, nothing else matters.
  if (std::string w = h.env("WAYLAND_DISPLAY"); !w.empty()) {
    r.wayland_display = w;
    std::string xdg = h.env("XDG_RUNTIME_DIR");
    fs::path sock = fs::path(w).is_absolute()
                        ? fs::path(w)
                        : fs::path(xdg.empty() ? "/run/user/1000" : xdg) / w;
    r.wayland = fs::exists(h.at(sock), ec);
    if (!r.wayland) {
      r.issues.push_back({S::Warning, "WAYLAND_DISPLAY is set but there is no socket at " + sock.string(),
                          "Run this from inside your desktop session, not from a separate login"});
    }
  }
  if (std::string d = h.env("DISPLAY"); !d.empty()) {
    r.x_display = d;
    r.x11 = true;
  }
  if (!r.wayland && !r.x11) {
    r.issues.push_back({S::Blocking, "No Wayland or X11 display: games need one of them",
                        "Run this from a desktop session"});
  }

  // DRM devices.
  fs::path drm = h.at("/sys/class/drm");
  if (fs::is_directory(drm, ec)) {
    std::vector<fs::path> cards;
    for (const fs::directory_entry& de : fs::directory_iterator(drm, ec)) {
      std::string base = de.path().filename().string();
      if (starts_with(base, "card") && base.find('-') == std::string::npos) cards.push_back(de.path());
    }
    std::sort(cards.begin(), cards.end());
    for (const fs::path& card : cards) {
      std::string base = card.filename().string();
      Device d;
      d.card = (fs::path("/sys/class/drm") / base).string();
      std::string vend = trim(read_first_line(card / "device" / "vendor"));
      if (!vend.empty()) {
        d.pci_vendor = static_cast<uint32_t>(std::strtoul(vend.c_str(), nullptr, 16));
        d.vendor = vendor_from_pci(d.pci_vendor);
      }
      fs::path drv = fs::read_symlink(card / "device" / "driver", ec);
      if (!ec) d.driver = drv.filename().string();
      ec.clear();
      // The render node is what actually gets used, and what needs to be
      // openable by this user. Each card lists its own under device/drm; the
      // first renderD in /dev/dri would give every card the same one.
      for (const fs::directory_entry& sub : fs::directory_iterator(card / "device" / "drm", ec)) {
        std::string n = sub.path().filename().string();
        if (starts_with(n, "renderD") && d.node.empty()) d.node = "/dev/dri/" + n;
      }
      ec.clear();
      if (d.node.empty()) d.node = "/dev/dri/" + base;
      d.readable = ::access(h.at(d.node).c_str(), R_OK | W_OK) == 0;
      if (!d.readable) {
        r.issues.push_back({S::Warning, d.node + " cannot be opened by you",
                            "Add yourself to the render and video groups, then log in again"});
      }
      r.devices.push_back(std::move(d));
    }
  }
  if (r.devices.empty()) {
    r.issues.push_back({S::Warning, "No GPU found under /sys/class/drm: games draw with software rendering",
                        "Check that a graphics driver is installed for your GPU"});
  }

  // NVIDIA proprietary.
  r.nvidia = capture_nvidia(h);
  r.nvidia_present = r.nvidia.present();
  r.nvidia_version = r.nvidia.kernel_version;
  for (const fs::path& p : r.nvidia.libs) r.nvidia_libs.push_back(p.string());
  for (const Problem& p : r.nvidia.problems) r.issues.push_back(p);

  for (const Problem& p : r.issues) r.problems.push_back(p.line());
  return r;
}

void materialize(Report& r) { materialize(r, gl_dir(), runtime_dir()); }

void materialize(Report& r, const fs::path& gl_root, const fs::path& runtime_root) {
  std::vector<std::string> icd, egl;

  if (r.nvidia.usable()) {
    Routing nv = route_nvidia(r.nvidia, gl_root / ("nvidia-" + r.nvidia.kernel_version));
    r.nvidia_link_dir = nv.dir;
    r.library_path = nv.dir.string();
    icd = nv.icd;
    egl = nv.egl_vendor;
    for (const auto& kv : nv.env) {
      if (kv.first != "VK_DRIVER_FILES" && kv.first != "__EGL_VENDOR_LIBRARY_FILENAMES") {
        r.env.push_back(kv);
      }
    }
  }

  // Ours after NVIDIA's, so a laptop's Intel half keeps working and lavapipe
  // is there as the last resort. Nothing of the host's Mesa is on either list.
  //
  // One directory per runtime, and rewritten in place rather than removed and
  // made again. A game started earlier from this state is reading it: its
  // loaders were pointed here, and Vulkan's reads its manifests again at each
  // instance. A --doctor, or a second launcher, started meanwhile removed
  // them from under it; and a newer build of the bundle, whose runtime is
  // mounted somewhere else, pointed them at its own Mesa - loaded, from then
  // on, against the running game's libc.
  if (!runtime_root.empty()) {
    fs::path mesa = gl_root / ("mesa-" + state_key(runtime_root));
    for (const std::string& s : adopt_runtime(runtime_root, "usr/share/vulkan/icd.d", mesa / "icd.d")) {
      icd.push_back(s);
    }
    for (const std::string& s :
         adopt_runtime(runtime_root, "usr/share/glvnd/egl_vendor.d", mesa / "egl_vendor.d")) {
      egl.push_back(s);
    }
  }
  if (!icd.empty()) r.env.emplace_back("VK_DRIVER_FILES", join(icd));
  if (!egl.empty()) r.env.emplace_back("__EGL_VENDOR_LIBRARY_FILENAMES", join(egl));
}

void print_report(const Report& r) {
  std::printf("display\n");
  if (r.wayland) std::printf("  wayland     %s\n", r.wayland_display.c_str());
  if (r.x11) std::printf("  x11         %s\n", r.x_display.c_str());
  if (!r.wayland && !r.x11) std::printf("  none\n");

  std::printf("\ngraphics\n");
  if (r.devices.empty()) {
    std::printf("  no DRM devices\n");
  }
  for (const Device& d : r.devices) {
    std::printf("  %-22s %-8s %-9s %s\n", d.node.c_str(), vendor_name(d.vendor),
                d.driver.empty() ? "-" : d.driver.c_str(), d.readable ? "accessible" : "NOT ACCESSIBLE");
  }

  std::printf("\nnvidia\n");
  switch (r.nvidia.state) {
    case NvidiaState::Absent: std::printf("  not present - bundled Mesa will be used\n"); break;
    case NvidiaState::Ok: std::printf("  kernel module %s, libraries match\n", r.nvidia_version.c_str()); break;
    case NvidiaState::Missing:
      std::printf("  kernel module %s, libraries NOT INSTALLED\n", r.nvidia_version.c_str());
      break;
    case NvidiaState::Mismatch:
      std::printf("  kernel module %s, libraries are %s - MISMATCH\n", r.nvidia_version.c_str(),
                  r.nvidia.library_version.c_str());
      break;
  }
  if (r.nvidia.usable()) {
    std::printf("  libraries     %zu taken\n", r.nvidia_libs.size());
    if (!r.nvidia_link_dir.empty()) std::printf("  linked into   %s\n", r.nvidia_link_dir.c_str());
  }
  for (const auto& [k, v] : r.env) std::printf("  %s=%s\n", k.c_str(), v.c_str());

  std::printf("\n");
  if (r.problems.empty()) {
    std::printf("no problems found\n");
  } else {
    for (const std::string& p : r.problems) std::printf("problem: %s\n", p.c_str());
  }
}

}  // namespace kg::gpu
