// What this machine can do for a game, as facts a policy can decide on.
//
// probe.h finds the hardware. This answers the questions the choice of
// backend and display actually turns on: which Vulkan the GPU speaks, whether
// OpenGL is hardware or llvmpipe, whether we are inside gamescope, whether
// there is a sound server to talk to, whether a gamepad can be read, whether
// FUSE is there to mount with.
//
// The two questions only a driver can answer - Vulkan and OpenGL - are asked
// of the runtime's own vulkaninfo and glxinfo, run under the runtime's loader
// with the same routing a game gets. Asking our own process instead would
// answer for a different library path than the one the game will have, and a
// host vulkaninfo would answer for the host's Mesa, which a game never loads.
// Both are injected, so tests hand in the text a real one prints.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "host.h"
#include "nvidia.h"
#include "probe.h"

namespace kg::gpu {

// The Vulkan a GPU offers, as far as the backends care. DXVK 3 wants 1.4 and
// DXVK 2.7 wants 1.3; below that WineD3D's Vulkan renderer still works.
enum class VulkanLevel { None, Legacy, V1_3, V1_4 };
const char* level_name(VulkanLevel l);

struct VulkanInfo {
  bool ran = false;               // vulkaninfo answered at all
  VulkanLevel level = VulkanLevel::None;  // best hardware device; CPU devices do not count
  std::string api_version;        // "1.4.305" of that device
  std::string device;             // "AMD Radeon RX 6800 (RADV NAVI21)"
  std::string driver;             // "radv"
  bool software_only = false;     // only lavapipe answered
};

struct GlInfo {
  bool ran = false;
  bool hardware = false;          // not llvmpipe, softpipe or swrast
  std::string renderer;
  std::string version;
};

VulkanInfo parse_vulkaninfo_summary(const std::string& text);
GlInfo parse_glxinfo(const std::string& text);

// The text of `vulkaninfo --summary` and `glxinfo -B`, or nothing when the
// program could not be run.
struct Probes {
  std::function<std::optional<std::string>()> vulkaninfo;
  std::function<std::optional<std::string>()> glxinfo;
};

enum class SessionType { None, X11, Wayland };
const char* session_name(SessionType s);

struct HostCaps {
  VulkanInfo vulkan;
  GlInfo gl;

  NvidiaState nvidia = NvidiaState::Absent;
  bool render_device = false;   // a /dev/dri render node this user can open
  bool other_gpu = false;       // a usable GPU that is not NVIDIA's: Intel, AMD, nouveau

  SessionType session = SessionType::None;
  std::string desktop;          // XDG_CURRENT_DESKTOP
  bool gamescope = false;

  bool pulse = false;           // a PulseAudio protocol socket is there
  std::string audio_server;     // "pipewire-pulse", "pulseaudio", "none"

  int input_nodes = 0, input_readable = 0;   // gamepads: by-id/*-event-joystick
  int hidraw_nodes = 0, hidraw_usable = 0;   // /dev/hidraw*

  bool fuse_device = false;     // /dev/fuse, readable and writable
  bool fusermount = false;      // a fusermount3 or fusermount on the host
  std::string mount_mode;       // KRETRO_MOUNT_MODE: how the bootstrap got here

  std::vector<Problem> problems;

  bool fuse() const { return fuse_device && fusermount; }
  // Whether a game can have a GPU at all. NVIDIA's driver unusable and nothing
  // else in the machine means no; a laptop whose Intel half still works means
  // yes, and the probes then answer for the Intel half.
  bool gpu_usable() const;
};

// True under gamescope: Steam Deck's Game Mode, or gamescope run as a session.
bool detect_gamescope(const Host& h);
SessionType detect_session(const Host& h);

HostCaps probe_caps(const Host& h, const Report& gpu, const Probes& probes);

}  // namespace kg::gpu
