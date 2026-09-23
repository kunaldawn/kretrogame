// NVIDIA's proprietary driver, taken from the host because it cannot come from
// anywhere else.
//
// The kernel module and the user-space libraries are one release: a
// libnvidia-glcore built for 590.48 talking to a 595.84 kernel module fails,
// and usually fails as a black window rather than an error. So the rule is
// exact. The kernel says which release is running, in /sys/module/nvidia/
// version, and every library taken must carry that release as its file name's
// suffix. Anything else is reported and nothing is taken.
//
// What is taken is a named set, never a directory. Appending the host's
// /usr/lib to our library path would put the host's Mesa, libstdc++ and
// libdrm in front of whatever of ours happens to be missing, and host Mesa is
// built against a newer glibc than ours may be: it is never loaded. NVIDIA's
// libraries are built against an old glibc on purpose, which is what lets
// them load under ours at all.
//
// The loaders are then told where the set is by name - GLVND by vendor and
// manifest, Vulkan by manifest, EGL's platform libraries by directory - so no
// search order anywhere decides it.
#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "host.h"

namespace kg::gpu {

enum class NvidiaState {
  Absent,    // no NVIDIA kernel module loaded: nothing to do
  Ok,        // every required library found at the kernel's version
  Missing,   // the module is loaded, the libraries are not installed
  Mismatch,  // the libraries are installed, at another version
};

struct Nvidia {
  NvidiaState state = NvidiaState::Absent;
  std::string kernel_version;               // "595.84"
  std::string library_version;              // what the host libraries carry, when mismatched
  std::vector<std::filesystem::path> libs;  // the captured set, host paths through the root
  std::vector<std::string> missing;         // file names wanted and not found
  std::vector<std::string> mismatched;      // file names found at another version
  std::vector<std::filesystem::path> icd;           // Vulkan ICD manifests
  std::vector<std::filesystem::path> egl_vendor;    // GLVND EGL vendor manifests
  std::vector<std::filesystem::path> egl_platform;  // EGL external platform manifests
  std::vector<Problem> problems;

  bool present() const { return state != NvidiaState::Absent; }
  bool usable() const { return state == NvidiaState::Ok; }
};

// The version string the kernel module reports, or empty with no module.
// /sys/module/nvidia/version first; /proc/driver/nvidia/version on the
// oldest drivers, which do not publish the sysfs file.
std::string nvidia_kernel_version(const Host& h);

// Looks. Modifies nothing.
Nvidia capture_nvidia(const Host& h);

// Where the loaders are pointed, once the set has been linked somewhere.
struct Routing {
  std::filesystem::path dir;       // the one directory added to the library path
  std::vector<std::string> icd;           // manifests we wrote, absolute
  std::vector<std::string> egl_vendor;
  std::filesystem::path egl_platform_dir;
  std::vector<std::pair<std::string, std::string>> env;
};

// Builds `dir`, in place: a symlink per captured library, the SONAME links
// the loaders ask for pointing at the version-matched file rather than at
// whatever the host's own links point to, and copies of the manifests with
// their library_path rewritten to an absolute path inside `dir`. Returns the
// variables that make the loaders use exactly that. A capture that is not
// usable routes nothing.
Routing route_nvidia(const Nvidia& n, const std::filesystem::path& dir);

// Writing what the loaders of a game already running read. Every start of a
// bundle rebuilds these directories - --doctor included - while a game
// started earlier from the same state reads them, and a Vulkan loader reads
// its manifests again at each instance. So nothing is removed and made
// again: each file or link is written beside itself and renamed over, and is
// the old one or the new one and never missing; then keep_only removes what
// `dir` holds that is not in `names`, other starts' temporaries aside.
void replace_file(const std::filesystem::path& dst, const std::string& bytes);
void replace_symlink(const std::filesystem::path& target, const std::filesystem::path& link);
void keep_only(const std::filesystem::path& dir, const std::set<std::string>& names);

}  // namespace kg::gpu
