#include "policy.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "../util/text.h"

namespace kg::backend {

using gpu::VulkanLevel;

namespace {

// Why nothing but software can draw here, in the words the doctor uses.
std::string why_no_gpu(const gpu::HostCaps& caps) {
  switch (caps.nvidia) {
    case gpu::NvidiaState::Mismatch:
      return "the NVIDIA libraries do not match the running kernel module (restarting usually fixes it)";
    case gpu::NvidiaState::Missing:
      return "the NVIDIA driver is loaded but its libraries are not installed";
    default:
      return "no graphics driver answered";
  }
}

Decision make(Backend b, std::string reason) { return Decision{b, std::move(reason)}; }

}  // namespace

AuthorBackend parse_author_backend(const std::string& s) {
  std::string v = to_lower(s);
  if (v == "dxvk") return AuthorBackend::Dxvk;
  if (v == "wined3d-vk") return AuthorBackend::WineD3DVulkan;
  if (v == "wined3d-gl") return AuthorBackend::WineD3DGL;
  if (v == "cnc-ddraw") return AuthorBackend::CncDdraw;
  return AuthorBackend::Auto;
}

const char* author_backend_name(AuthorBackend b) {
  switch (b) {
    case AuthorBackend::Dxvk: return "dxvk";
    case AuthorBackend::WineD3DVulkan: return "wined3d-vk";
    case AuthorBackend::WineD3DGL: return "wined3d-gl";
    case AuthorBackend::CncDdraw: return "cnc-ddraw";
    default: return "auto";
  }
}

const char* backend_name(Backend b) {
  switch (b) {
    case Backend::Dxvk3: return "dxvk3";
    case Backend::Dxvk2: return "dxvk2";
    case Backend::WineD3DVulkan: return "wined3d-vk";
    case Backend::WineD3DGL: return "wined3d-gl";
    case Backend::CncDdraw: return "cnc-ddraw";
    case Backend::NativeGL: return "native-gl";
    case Backend::Software: return "software";
    default: return "refuse";
  }
}

const char* display_path_name(DisplayPath d) {
  return d == DisplayPath::GamescopeDirect ? "gamescope-direct" : "nested-weston";
}

Decision choose_backend(AuthorBackend author, const pe::Imports& exe, const gpu::HostCaps& caps,
                        bool needs_gpu) {
  if (!caps.gpu_usable()) {
    std::string why = why_no_gpu(caps);
    if (needs_gpu) {
      return make(Backend::Refuse, "This game needs a GPU and none can be used here: " + why);
    }
    return make(Backend::Software, "No GPU can be used here (" + why + "), so this game draws "
                                   "with software rendering and may be slow");
  }

  const VulkanLevel vk = caps.vulkan.level;
  const bool vk_any = vk != VulkanLevel::None;
  const std::string vk_said = caps.vulkan.ran ? "" : " (Vulkan could not be checked)";

  // Direct3D 8 and 9, and the rare later one, go to DXVK when the GPU speaks
  // enough Vulkan, and fall down the table when it does not.
  auto dxvk = [&](const std::string& what) {
    if (vk == VulkanLevel::V1_4) return make(Backend::Dxvk3, what + ", Vulkan 1.4: DXVK 3");
    if (vk == VulkanLevel::V1_3) return make(Backend::Dxvk2, what + ", Vulkan 1.3: DXVK 2.7");
    if (vk == VulkanLevel::Legacy) {
      return make(Backend::WineD3DVulkan,
                  what + ", but Vulkan " + caps.vulkan.api_version +
                      " is too old for DXVK: WineD3D on Vulkan");
    }
    return make(Backend::WineD3DGL, what + ", but no Vulkan" + vk_said + ": WineD3D on OpenGL");
  };
  auto wined3d = [&](const std::string& what) {
    if (vk_any) return make(Backend::WineD3DVulkan, what + ": WineD3D on Vulkan");
    return make(Backend::WineD3DGL, what + ", no Vulkan" + vk_said + ": WineD3D on OpenGL");
  };

  switch (author) {
    case AuthorBackend::Dxvk: return dxvk("The author chose DXVK");
    case AuthorBackend::WineD3DVulkan:
      if (vk_any) return make(Backend::WineD3DVulkan, "The author chose WineD3D on Vulkan");
      return make(Backend::WineD3DGL, "The author chose WineD3D on Vulkan, but there is no Vulkan" +
                                          vk_said + ": WineD3D on OpenGL");
    case AuthorBackend::WineD3DGL: return make(Backend::WineD3DGL, "The author chose WineD3D on OpenGL");
    case AuthorBackend::CncDdraw: return make(Backend::CncDdraw, "The author chose cnc-ddraw");
    case AuthorBackend::Auto: break;
  }

  if (!exe.ok) return wined3d("The executable's imports could not be read");

  for (const char* d : {"d3d9", "d3d8", "d3d11", "d3d10", "d3d10_1", "dxgi"}) {
    if (exe.imports(d)) return dxvk(std::string("The game imports ") + d);
  }
  // Before DirectDraw: a game that links both usually has a DirectDraw
  // software renderer and an OpenGL one, and the OpenGL one is the one worth
  // having.
  if (exe.imports("opengl32")) return make(Backend::NativeGL, "The game draws with OpenGL: native");
  if (exe.imports("ddraw")) {
    if (exe.direct3d_im || exe.imports("d3dim") || exe.imports("d3dim700") || exe.imports("d3drm")) {
      return wined3d("The game uses Direct3D 5-7 through DirectDraw");
    }
    return make(Backend::CncDdraw, "The game draws 2D with DirectDraw: cnc-ddraw");
  }
  if (exe.imports("d3drm")) return wined3d("The game uses Direct3D retained mode");
  if (draws_with_opengl(exe)) return make(Backend::NativeGL, "The game loads OpenGL itself: native");
  return wined3d("The game imports no Direct3D, DirectDraw or OpenGL");
}

bool draws_with_opengl(const pe::Imports& exe) {
  if (!exe.ok) return false;
  if (exe.imports("opengl32")) return true;
  if (!exe.names_opengl) return false;
  for (const char* d : {"d3d9", "d3d8", "d3d11", "d3d10", "d3d10_1", "dxgi", "ddraw", "d3dim",
                        "d3dim700", "d3drm"}) {
    if (exe.imports(d)) return false;
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> gl_extension_cap() {
  return {{"__GL_ExtensionStringVersion", "17700"}, {"MESA_EXTENSION_MAX_YEAR", "2003"}};
}

DisplayPath display_path(const gpu::HostCaps& caps) {
  // Under gamescope there already is a compositor that scales and owns the
  // screen, and a nested Weston inside it would be a window in a window.
  return caps.gamescope ? DisplayPath::GamescopeDirect : DisplayPath::NestedWeston;
}

std::string audio_drivers() { return "pulse,alsa"; }

Plan settings_for(const Decision& d, const gpu::HostCaps& caps) {
  Plan s;
  s.decision = d;
  s.display = display_path(caps);
  if (d.backend == Backend::Refuse) return s;

  const std::string d3d = "HKEY_CURRENT_USER\\Software\\Wine\\Direct3D";
  switch (d.backend) {
    case Backend::Dxvk3:
    case Backend::Dxvk2:
      s.dll_dirs = d.backend == Backend::Dxvk3 ? std::vector<std::string>{"opt/dxvk-3"}
                                               : std::vector<std::string>{"opt/dxvk-2", "opt/dxvk"};
      s.dlls = {"d3d8", "d3d9", "d3d10core", "d3d11", "dxgi"};
      s.dll_overrides = "d3d8,d3d9,d3d10core,d3d11,dxgi=n,b";
      // DXVK writes a log beside the game by default. The game directory is
      // the saves overlay, so every session would record a log file as
      // something the game wrote, and snapshot it.
      s.env.emplace_back("DXVK_LOG_LEVEL", "none");
      break;
    case Backend::WineD3DVulkan: s.registry.push_back({d3d, "renderer", false, "vulkan"}); break;
    case Backend::WineD3DGL: s.registry.push_back({d3d, "renderer", false, "gl"}); break;
    case Backend::CncDdraw:
      s.dll_dirs = {"opt/cnc-ddraw"};
      s.dlls = {"ddraw"};
      s.dll_overrides = "ddraw=n,b";
      break;
    case Backend::NativeGL: break;
    case Backend::Software:
      s.registry.push_back({d3d, "renderer", false, "gl"});
      // Mesa's own, and never the host's: llvmpipe is in the runtime.
      s.env.emplace_back("LIBGL_ALWAYS_SOFTWARE", "1");
      s.env.emplace_back("GALLIUM_DRIVER", "llvmpipe");
      s.env.emplace_back("__GLX_VENDOR_LIBRARY_NAME", "mesa");
      break;
    case Backend::Refuse: break;
  }

  s.registry.push_back({"HKEY_CURRENT_USER\\Software\\Wine\\Drivers", "Audio", false, audio_drivers()});
  // As Proton sets them: gamepads through SDL, which knows every pad's
  // mapping, and hidraw off, or a pad SDL already reported appears a second
  // time as a raw HID device and the game binds whichever it saw first.
  const std::string bus = "HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\WineBus";
  s.registry.push_back({bus, "DisableHidraw", true, "1"});
  s.registry.push_back({bus, "Enable SDL", true, "1"});
  return s;
}

Plan plan(AuthorBackend author, const pe::Imports& exe, const gpu::HostCaps& caps,
          bool needs_gpu) {
  return settings_for(choose_backend(author, exe, caps, needs_gpu), caps);
}

std::string registry_file(const std::vector<RegValue>& values) {
  auto quote = [](const std::string& v) {
    std::string o = "\"";
    for (char c : v) {
      if (c == '\\' || c == '"') o += '\\';
      o += c;
    }
    return o + "\"";
  };
  std::string out = "REGEDIT4\n";
  std::string key;
  for (const RegValue& v : values) {
    if (v.key != key) {
      key = v.key;
      out += "\n[" + key + "]\n";
    }
    out += quote(v.name) + "=";
    if (v.dword) {
      char buf[16];
      std::snprintf(buf, sizeof buf, "dword:%08lx", std::strtoul(v.data.c_str(), nullptr, 0));
      out += buf;
    } else {
      out += quote(v.data);
    }
    out += "\n";
  }
  return out;
}

}  // namespace kg::backend
