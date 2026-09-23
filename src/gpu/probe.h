// The graphics bridge: the only component that reads anything from the host.
//
// Bundled Mesa covers AMD and Intel completely, because the kernel's DRM uAPI
// is stable and Mesa needs nothing else. NVIDIA's proprietary userspace must
// match the running kernel module exactly, so it can only come from the host;
// nvidia.h finds the version-matched set and routes the loaders to it by name.
//
// Bundling glibc is what makes that work on a musl distribution at all:
// NVIDIA's blobs are glibc-linked ELFs with no libc to load against on stock
// Alpine. Ours.
//
// Every read goes through a Host, so a test can point the whole probe at a
// fixture tree instead of this machine.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "host.h"
#include "nvidia.h"

namespace kg::gpu {

enum class Vendor { Unknown, Nvidia, Amd, Intel, Software };

struct Device {
  std::string node;       // /dev/dri/renderD128
  std::string card;       // /sys/class/drm/card1
  Vendor vendor = Vendor::Unknown;
  uint32_t pci_vendor = 0;
  std::string driver;     // the kernel driver bound to it: amdgpu, i915, nvidia, nouveau
  std::string name;       // best-effort human name
  bool readable = false;  // can this user actually open it?
};

struct Report {
  std::vector<Device> devices;

  // NVIDIA proprietary, when present. The first four are the capture's own
  // answers, kept as fields because the shelf's doctor page reads them.
  Nvidia nvidia;
  bool nvidia_present = false;
  std::string nvidia_version;               // e.g. "595.84"
  std::vector<std::string> nvidia_libs;     // absolute host paths we linked
  std::filesystem::path nvidia_link_dir;    // <state>/gl/nvidia-<version>

  // What the runtime should be told.
  std::string library_path;                 // to append after the runtime's own
  std::vector<std::pair<std::string, std::string>> env;

  // Display.
  bool wayland = false, x11 = false;
  std::string wayland_display, x_display;

  std::vector<Problem> issues;              // what is wrong, and what to do
  std::vector<std::string> problems;        // the same, one line each; empty means fine
};

// Reads the host. Does not modify anything.
Report probe(const Host& h);
Report probe();

// Points the runtime's loaders at drivers, and only at drivers we chose.
//
// With a usable NVIDIA driver: <state>/gl/nvidia-<version>/ holds links to
// exactly the captured set and rewritten manifests, and the environment names
// them. Always, when there is a runtime: the runtime's own Mesa Vulkan and EGL
// manifests are named explicitly as well, with any absolute host path in them
// moved into the runtime - otherwise the Vulkan loader's default search finds
// the host's /usr/share/vulkan/icd.d and loads the host's Mesa, which was
// built against whatever glibc the host has.
//
// Rebuilt on every run, because a driver update changes the answer and a
// stale link is worse than none.
void materialize(Report& r);
void materialize(Report& r, const std::filesystem::path& gl_root,
                 const std::filesystem::path& runtime_root);

const char* vendor_name(Vendor v);

// The plain-language GPU section of `kretro doctor`.
void print_report(const Report& r);

}  // namespace kg::gpu
