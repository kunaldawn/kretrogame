#include "nvidia.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "files.h"
#include "../util/text.h"

namespace kg::gpu {
namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Where distributions put the driver. Looked through rather than trusted to
// be any one layout: Debian's multiarch, Fedora's lib64 and RPM Fusion's
// nvidia subdirectory, Arch's /usr/lib, NixOS's /run/opengl-driver.
const std::vector<fs::path>& lib_dirs() {
  static const std::vector<fs::path> d = {
      "/usr/lib/x86_64-linux-gnu",
      "/usr/lib/x86_64-linux-gnu/nvidia/current",
      "/usr/lib/x86_64-linux-gnu/nvidia",
      "/usr/lib64",
      "/usr/lib64/nvidia",
      "/usr/lib",
      "/usr/lib/nvidia",
      "/lib/x86_64-linux-gnu",
      "/lib64",
      "/lib",
      "/run/opengl-driver/lib",
  };
  return d;
}

const std::vector<fs::path>& icd_dirs() {
  static const std::vector<fs::path> d = {
      "/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d", "/usr/local/share/vulkan/icd.d",
      "/run/opengl-driver/share/vulkan/icd.d"};
  return d;
}

const std::vector<fs::path>& egl_vendor_dirs() {
  static const std::vector<fs::path> d = {"/usr/share/glvnd/egl_vendor.d",
                                          "/etc/glvnd/egl_vendor.d",
                                          "/run/opengl-driver/share/glvnd/egl_vendor.d"};
  return d;
}

const std::vector<fs::path>& egl_platform_dirs() {
  static const std::vector<fs::path> d = {"/usr/share/egl/egl_external_platform.d",
                                          "/etc/egl/egl_external_platform.d",
                                          "/run/opengl-driver/share/egl/egl_external_platform.d"};
  return d;
}

// The set, by stem. `versioned` libraries carry the driver release as their
// suffix and must match the kernel exactly. The EGL platform libraries are
// NVIDIA's open-source projects with their own numbering, and any copy works;
// the runtime carries its own egl-wayland besides.
//
// Required is what OpenGL and EGL need to start at all. gpucomp is newer than
// some drivers still in use and glvkspirv is Vulkan's shader compiler; either
// is taken when it is there, and either found at the wrong release is as much
// a mismatch as glcore would be.
struct Want {
  const char* stem;
  bool versioned;
  bool required;
  const char* soname;  // the link a loader asks for, when it is not the file itself
};

const std::vector<Want>& wanted() {
  static const std::vector<Want> w = {
      {"libGLX_nvidia.so", true, true, "libGLX_nvidia.so.0"},
      {"libEGL_nvidia.so", true, true, "libEGL_nvidia.so.0"},
      {"libnvidia-glcore.so", true, true, nullptr},
      {"libnvidia-glsi.so", true, true, nullptr},
      {"libnvidia-tls.so", true, true, nullptr},
      {"libnvidia-eglcore.so", true, true, nullptr},
      {"libnvidia-gpucomp.so", true, false, nullptr},
      {"libnvidia-glvkspirv.so", true, false, nullptr},
      // Loaded by glcore and eglcore through its SONAME, for buffer
      // allocation. It is built with the driver, so it is held to the
      // driver's release like the rest.
      {"libnvidia-allocator.so", true, false, "libnvidia-allocator.so.1"},
      {"libnvidia-egl-wayland.so", false, false, "libnvidia-egl-wayland.so.1"},
      {"libnvidia-egl-gbm.so", false, false, "libnvidia-egl-gbm.so.1"},
      // The newer platform libraries. The driver installs a manifest for each
      // (09_nvidia_wayland2, 20_nvidia_xcb, 20_nvidia_xlib), and every NVIDIA
      // manifest is adopted below; one whose library was left behind names a
      // file nothing can find. On a current driver xcb and xlib are how EGL
      // reaches an X server, which is what a nested Weston on an X11 desktop
      // does.
      {"libnvidia-egl-wayland2.so", false, false, "libnvidia-egl-wayland2.so.1"},
      {"libnvidia-egl-xcb.so", false, false, "libnvidia-egl-xcb.so.1"},
      {"libnvidia-egl-xlib.so", false, false, "libnvidia-egl-xlib.so.1"},
  };
  return w;
}

// "595.84" or "590.48.01": digits and dots, at least one dot. A SONAME
// suffix such as ".0" is not a release.
bool release_shaped(const std::string& s) {
  if (s.empty() || s.find('.') == std::string::npos) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

std::vector<fs::path> manifests(const Host& h, const std::vector<fs::path>& dirs) {
  std::vector<fs::path> out;
  // One manifest per name. Some systems carry the same nvidia_icd.json in both
  // /usr/share and /etc. Both would be copied into one directory under one
  // name, and listing that copy twice loads the driver twice, so every GPU
  // appears as two.
  std::set<std::string> names;
  std::error_code ec;
  for (const fs::path& d : dirs) {
    fs::path real = h.at(d);
    if (!fs::is_directory(real, ec)) continue;
    std::vector<fs::path> here;
    for (const fs::directory_entry& de : fs::directory_iterator(real, ec)) {
      std::string name = de.path().filename().string();
      if (name.size() < 5 || name.substr(name.size() - 5) != ".json") continue;
      // 32-bit manifests are for a 32-bit loader, which new WoW64 has none of.
      if (name.find("i686") != std::string::npos || name.find("i386") != std::string::npos) continue;
      std::string json = read_all(de.path());
      if (name.find("nvidia") == std::string::npos && json.find("nvidia") == std::string::npos) continue;
      if (!names.insert(name).second) continue;
      here.push_back(de.path());
    }
    // Directory order is whatever the filesystem says; manifests are named to
    // be read in order, 10_ before 15_.
    std::sort(here.begin(), here.end());
    out.insert(out.end(), here.begin(), here.end());
  }
  return out;
}

// The version string the kernel module reports, or empty with no module.
// /sys/module/nvidia/version first; /proc/driver/nvidia/version on the
// oldest drivers, which do not publish the sysfs file.
std::string nvidia_kernel_version(const Host& h) {
  std::ifstream sys(h.at("/sys/module/nvidia/version"));
  std::string v;
  if (sys && std::getline(sys, v) && release_shaped(trim(v))) return trim(v);

  // "NVRM version: NVIDIA UNIX x86_64 Kernel Module  595.84  Tue ..."
  std::ifstream f(h.at("/proc/driver/nvidia/version"));
  std::string line;
  if (!f || !std::getline(f, line)) return "";
  std::smatch m;
  static const std::regex re(R"(Kernel Module\s+([0-9]+\.[0-9.]+))");
  if (std::regex_search(line, m, re)) return m[1];
  // Open kernel module builds phrase the line differently; the first
  // release-shaped token on it is the version.
  static const std::regex any(R"(([0-9]+\.[0-9]{2,}(\.[0-9]+)?))");
  if (std::regex_search(line, m, any)) return m[1];
  return "";
}

}  // namespace

Nvidia capture_nvidia(const Host& h) {
  Nvidia n;
  n.kernel_version = nvidia_kernel_version(h);
  if (n.kernel_version.empty()) return n;
  const std::string& v = n.kernel_version;

  // Every file in every library directory, once. Two directories can hold the
  // same name - /usr/lib64 is /usr/lib on some distributions - and the first
  // one found is the one taken, which is the loader's rule too.
  std::vector<std::pair<std::string, fs::path>> files;
  std::set<std::string> seen;
  std::error_code ec;
  for (const fs::path& d : lib_dirs()) {
    fs::path real = h.at(d);
    if (!fs::is_directory(real, ec)) continue;
    std::vector<std::pair<std::string, fs::path>> here;
    for (const fs::directory_entry& de : fs::directory_iterator(real, ec)) {
      std::string name = de.path().filename().string();
      if (!name.starts_with("libnvidia-") && !name.starts_with("libGLX_nvidia.") &&
          !name.starts_with("libEGL_nvidia.")) {
        continue;
      }
      if (seen.insert(name).second) here.emplace_back(name, de.path());
    }
    std::sort(here.begin(), here.end());
    files.insert(files.end(), here.begin(), here.end());
  }

  std::set<std::string> other_versions;
  for (const Want& w : wanted()) {
    const std::string prefix = std::string(w.stem) + ".";
    fs::path exact;
    std::string other;
    for (const auto& [name, path] : files) {
      if (!name.starts_with(prefix)) continue;
      std::string suffix = name.substr(prefix.size());
      if (w.versioned) {
        if (suffix == v) {
          exact = path;
        } else if (release_shaped(suffix)) {
          other = name;
          other_versions.insert(suffix);
        }
      } else if (release_shaped(suffix)) {
        // Its own numbering. Only the full release is release-shaped - the
        // ".so.1" link is not - so this is the file itself, or, on NixOS,
        // the link into the store that stands in for every file there. The
        // last in sorted order is the newest when more than one is installed.
        exact = path;
      }
    }
    if (!exact.empty()) {
      n.libs.push_back(exact);
    } else if (!other.empty()) {
      n.mismatched.push_back(other);
    } else if (w.required) {
      n.missing.push_back(std::string(w.stem) + "." + v);
    }
  }
  if (!other_versions.empty()) n.library_version = *other_versions.begin();

  n.icd = manifests(h, icd_dirs());
  n.egl_vendor = manifests(h, egl_vendor_dirs());
  n.egl_platform = manifests(h, egl_platform_dirs());

  const std::string no_gpu =
      "Until then games draw with software rendering, and a game that needs a GPU will not start";
  if (!n.mismatched.empty()) {
    n.state = NvidiaState::Mismatch;
    n.problems.push_back(
        {Problem::Severity::Warning,
         "The NVIDIA kernel module is " + v + " but the installed NVIDIA libraries are " +
             (n.library_version.empty() ? "another version" : n.library_version),
         "The driver was updated and the machine has not been restarted since: restart it. If "
         "that does not help, reinstall the NVIDIA driver so both are the same version. " +
             no_gpu});
  } else if (!n.missing.empty()) {
    n.state = NvidiaState::Missing;
    n.problems.push_back(
        {Problem::Severity::Warning,
         "The NVIDIA driver " + v + " is loaded but its libraries are not installed (" +
             join(n.missing, ", ") + ")",
         "Install your distribution's NVIDIA driver package for " + v +
             ", the one that provides libGLX_nvidia.so." + v + ". " + no_gpu});
  } else {
    n.state = NvidiaState::Ok;
    if (n.icd.empty()) {
      n.problems.push_back(
          {Problem::Severity::Warning,
           "The NVIDIA driver " + v + " has no Vulkan manifest (nvidia_icd.json)",
           "Install the Vulkan part of your distribution's NVIDIA driver package. Until then "
           "Direct3D 8 and 9 games run on OpenGL, which is slower"});
    }
  }
  if (!n.usable()) n.libs.clear();
  return n;
}

Routing route_nvidia(const Nvidia& n, const fs::path& dir) {
  Routing r;
  if (!n.usable()) return r;

  std::error_code ec;
  // A driver update changes the answer, and a stale link is worse than none,
  // so this is rebuilt every time rather than kept. Rebuilt in place, entry by
  // entry, and never removed first: a game started earlier is reading these
  // manifests and links now - a Vulkan loader reads them again at each
  // instance - and every start of the bundle, --doctor included, rebuilds
  // them. Each is replaced by a rename, so it is the old one or the new one
  // and never missing, and what is no longer wanted goes after.
  fs::create_directories(dir, ec);
  r.dir = dir;
  std::set<std::string> made = {"icd.d", "egl_vendor.d", "egl_external_platform.d"};

  for (const fs::path& src : n.libs) {
    std::string name = src.filename().string();
    replace_symlink(src, dir / name);
    made.insert(name);
    // The SONAME is made here, pointing at the matched file, and not copied
    // from the host: the host's libGLX_nvidia.so.0 is exactly what goes stale
    // across an update and still points at the old release.
    for (const Want& w : wanted()) {
      if (w.soname && name.starts_with(std::string(w.stem) + ".") && name != w.soname) {
        replace_symlink(name, dir / w.soname);
        made.insert(w.soname);
      }
    }
  }

  // Each manifest names a library, usually by SONAME and sometimes by a host
  // path. Either way it is rewritten to the absolute path of the link we just
  // made, so neither the host's search path nor ours is consulted. A library
  // we did not take keeps its bare name, which then resolves against the
  // runtime's own copy - egl-wayland is in the runtime for exactly this.
  auto adopt = [&](const std::vector<fs::path>& from, const fs::path& into,
                   std::vector<std::string>* out) {
    if (from.empty()) {
      made.erase(into.filename().string());
      return;
    }
    fs::create_directories(into, ec);
    std::set<std::string> written;
    for (const fs::path& m : from) {
      std::string json = read_all(m);
      static const std::regex re(R"RX("library_path"\s*:\s*"([^"]*)")RX");
      std::smatch sm;
      if (std::regex_search(json, sm, re)) {
        std::string lib = fs::path(sm[1].str()).filename().string();
        if (fs::exists(dir / lib, ec)) {
          json = std::regex_replace(json, re, "\"library_path\": \"" + (dir / lib).string() + "\"");
        }
      }
      fs::path dst = into / m.filename();
      replace_file(dst, json);
      written.insert(m.filename().string());
      if (out) out->push_back(dst.string());
    }
    keep_only(into, written);
  };
  adopt(n.icd, dir / "icd.d", &r.icd);
  adopt(n.egl_vendor, dir / "egl_vendor.d", &r.egl_vendor);
  adopt(n.egl_platform, dir / "egl_external_platform.d", nullptr);
  if (!n.egl_platform.empty()) r.egl_platform_dir = dir / "egl_external_platform.d";
  keep_only(dir, made);

  r.env.emplace_back("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
  if (!r.egl_vendor.empty()) r.env.emplace_back("__EGL_VENDOR_LIBRARY_FILENAMES", join(r.egl_vendor, ":"));
  if (!r.icd.empty()) r.env.emplace_back("VK_DRIVER_FILES", join(r.icd, ":"));
  if (!r.egl_platform_dir.empty()) {
    r.env.emplace_back("__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS", r.egl_platform_dir.string());
  }
  return r;
}

}  // namespace kg::gpu
