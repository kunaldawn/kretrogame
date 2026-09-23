// Tier 1 unit tests for how a game is drawn: PE imports, the NVIDIA capture,
// the host capability probe, the backend table and the doctor's report.
//
// No GPU, no display, no Wine. Every host this runs "on" is a fixture tree
// under a temporary directory, reached through gpu::Host, and every Vulkan
// and OpenGL answer is text a real vulkaninfo or glxinfo prints.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "gpu/caps.h"
#include "gpu/nvidia.h"
#include "gpu/probe.h"
#include "player/doctor.h"
#include "player/policy.h"
#include "session/session.h"
#include "util/paths.h"
#include "util/pe.h"

namespace fs = std::filesystem;
using namespace kg;
using player::AuthorBackend;
using player::Backend;
using gpu::NvidiaState;
using gpu::VulkanLevel;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++checks;                                                                \
    if (!(cond)) {                                                           \
      ++failures;                                                            \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    ++checks;                                                                            \
    auto va_ = (a);                                                                      \
    auto vb_ = (b);                                                                      \
    if (!(va_ == vb_)) {                                                                 \
      ++failures;                                                                        \
      std::ostringstream os_;                                                            \
      os_ << va_ << " != " << vb_;                                                       \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, os_.str().c_str()); \
    }                                                                                    \
  } while (0)

static bool contains(const std::string& s, const std::string& what) {
  return s.find(what) != std::string::npos;
}

static void write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << s;
}

static std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::string env_of(const std::vector<std::pair<std::string, std::string>>& env,
                          const std::string& key) {
  for (const auto& [k, v] : env) {
    if (k == key) return v;
  }
  return "";
}

// ---- PE fixtures ------------------------------------------------------------
//
// The smallest executable the parser will accept: a DOS header, the PE
// signature, a COFF header, an optional header with sixteen data directories,
// one section at file offset 0x200 mapped at RVA 0x1000, and in that section
// the import descriptors followed by the DLL names. 0x400 bytes in all.

static void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = uint8_t(v);
  b[at + 1] = uint8_t(v >> 8);
}
static void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i));
}

struct PeLayout {
  size_t opt, dirs, sections, idata;
};

static PeLayout layout(bool is64) {
  PeLayout l;
  l.opt = 0x40 + 24;
  l.dirs = l.opt + (is64 ? 112 : 96);
  l.sections = l.opt + (is64 ? 240 : 224);
  l.idata = 0x200;
  return l;
}

static std::vector<uint8_t> make_pe(const std::vector<std::string>& dlls, bool is64 = false) {
  std::vector<uint8_t> b(0x400, 0);
  PeLayout l = layout(is64);
  b[0] = 'M';
  b[1] = 'Z';
  put32(b, 0x3c, 0x40);
  std::memcpy(&b[0x40], "PE\0\0", 4);
  put16(b, 0x44, is64 ? 0x8664 : 0x14c);
  put16(b, 0x46, 1);                          // one section
  put16(b, 0x54, is64 ? 240 : 224);           // optional header size
  put16(b, l.opt, is64 ? 0x20b : 0x10b);
  put32(b, l.opt + 60, 0x200);                // SizeOfHeaders
  put32(b, l.opt + (is64 ? 108 : 92), 16);    // NumberOfRvaAndSizes
  put32(b, l.dirs + 8, 0x1000);               // import directory RVA
  put32(b, l.dirs + 12, uint32_t(20 * (dlls.size() + 1)));

  std::memcpy(&b[l.sections], ".idata\0\0", 8);
  put32(b, l.sections + 8, 0x1000);           // VirtualSize
  put32(b, l.sections + 12, 0x1000);          // VirtualAddress
  put32(b, l.sections + 16, 0x200);           // SizeOfRawData
  put32(b, l.sections + 20, 0x200);           // PointerToRawData

  uint32_t name_rva = 0x1100;
  for (size_t i = 0; i < dlls.size(); ++i) {
    size_t d = l.idata + i * 20;
    put32(b, d + 12, name_rva);
    size_t at = 0x200 + (name_rva - 0x1000);
    std::memcpy(&b[at], dlls[i].c_str(), dlls[i].size() + 1);
    name_rva += uint32_t(dlls[i].size() + 1);
  }
  return b;
}

static void test_pe_valid() {
  pe::Imports r = pe::parse(make_pe({"KERNEL32.dll", "DDRAW.dll", "d3d9.dll", "ddraw.dll"}));
  CHECK(r.ok);
  CHECK(!r.is64);
  CHECK_EQ(r.dlls.size(), 3u);  // DDRAW and ddraw are one DLL
  if (r.dlls.size() == 3) {
    CHECK_EQ(r.dlls[0], std::string("kernel32.dll"));
    CHECK_EQ(r.dlls[1], std::string("ddraw.dll"));
    CHECK_EQ(r.dlls[2], std::string("d3d9.dll"));
  }
  CHECK(r.imports("d3d9"));
  CHECK(r.imports("D3D9.DLL"));
  CHECK(!r.imports("opengl32"));
  CHECK(!r.imports("d3d"));  // a prefix is not a match
  CHECK(!r.direct3d_im);

  pe::Imports r64 = pe::parse(make_pe({"opengl32.dll", "USER32.dll"}, true));
  CHECK(r64.ok);
  CHECK(r64.is64);
  CHECK(r64.imports("opengl32"));
  CHECK_EQ(r64.dlls.size(), 2u);

  // Headers and no import directory: an executable that imports nothing.
  std::vector<uint8_t> none = make_pe({});
  put32(none, layout(false).dirs + 8, 0);
  pe::Imports rn = pe::parse(none);
  CHECK(rn.ok);
  CHECK(rn.dlls.empty());
}

static void test_pe_direct3d_iid() {
  // IID_IDirect3D7 {f5049e77-4861-11d2-a407-00a0c90629a8}, as it sits in a
  // binary linked against dxguid.lib, somewhere after the imports.
  std::vector<uint8_t> b = make_pe({"DDRAW.dll"});
  const uint8_t iid7[16] = {0x77, 0x9e, 0x04, 0xf5, 0x61, 0x48, 0xd2, 0x11,
                            0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8};
  std::memcpy(&b[0x380], iid7, 16);
  pe::Imports r = pe::parse(b);
  CHECK(r.ok);
  CHECK(r.direct3d_im);

  // IID_IDirect3D2 {6aae1ec1-662a-11d0-889d-00aa00bbb76a}: Direct3D 5.
  std::vector<uint8_t> b2 = make_pe({"DDRAW.dll"});
  const uint8_t iid2[16] = {0xc1, 0x1e, 0xae, 0x6a, 0x2a, 0x66, 0xd0, 0x11,
                            0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a};
  std::memcpy(&b2[0x3a0], iid2, 16);
  CHECK(pe::parse(b2).direct3d_im);
}

static void test_pe_truncated() {
  std::vector<std::string> want = {"kernel32.dll", "ddraw.dll", "winmm.dll"};
  std::vector<uint8_t> full = make_pe({"KERNEL32.dll", "DDRAW.dll", "WINMM.dll"});
  // Cut at every length. Each cut either fails with a reason or - once
  // everything the parser needs is inside the cut - gives the whole answer.
  // A partial list would be a wrong answer that looks like a right one.
  int failed = 0, whole = 0, partial = 0;
  for (size_t n = 0; n <= full.size(); ++n) {
    pe::Imports r = pe::parse(full.data(), n);
    if (!r.ok) {
      ++failed;
      if (r.error.empty()) ++partial;
    } else if (r.dlls == want) {
      ++whole;
    } else {
      ++partial;
    }
  }
  CHECK_EQ(partial, 0);
  CHECK(failed > 0x300);  // everything short of the last name fails
  CHECK(whole > 0);
  CHECK(!pe::parse(nullptr, 0).ok);
}

static void test_pe_malicious() {
  auto fails = [](std::vector<uint8_t> b) {
    pe::Imports r = pe::parse(b);
    return !r.ok && !r.error.empty();
  };
  PeLayout l = layout(false);
  std::vector<uint8_t> base = make_pe({"DDRAW.dll"});

  // Not an executable at all.
  CHECK(fails(std::vector<uint8_t>(0x400, 0)));
  // A DOS program: MZ and no PE header where e_lfanew says.
  {
    std::vector<uint8_t> b = base;
    b[0x40] = 'X';
    CHECK(fails(b));
  }
  // e_lfanew far outside the file, and at the very edge of 32 bits, where a
  // careless sum would wrap round to a small offset.
  for (uint32_t off : {0x7fffffffu, 0xfffffff0u, 0xffffffffu, 0x3f0u}) {
    std::vector<uint8_t> b = base;
    put32(b, 0x3c, off);
    CHECK(fails(b));
  }
  // More sections than exist, and more than any real executable has.
  {
    std::vector<uint8_t> b = base;
    put16(b, 0x46, 0xffff);
    CHECK(fails(b));
    put16(b, 0x46, 40);
    CHECK(fails(b));  // 40 sections run off the end of the file
  }
  // An optional header that claims to be larger than the file.
  {
    std::vector<uint8_t> b = base;
    put16(b, 0x54, 0xfff0);
    CHECK(fails(b));
  }
  // An import table outside every section.
  {
    std::vector<uint8_t> b = base;
    put32(b, l.dirs + 8, 0x7fff0000);
    CHECK(fails(b));
  }
  // A section whose file offset is past the end of the file.
  {
    std::vector<uint8_t> b = base;
    put32(b, l.sections + 20, 0xffffff00);
    CHECK(fails(b));
  }
  // A section whose raw size would reach past 4 GiB from its offset.
  {
    std::vector<uint8_t> b = base;
    put32(b, l.sections + 16, 0xffffffff);
    put32(b, l.sections + 8, 0xffffffff);
    pe::Imports r = pe::parse(b);
    // Either refused or read correctly; never read past the buffer (the
    // sanitizer-free proof is that the name comes back exactly right).
    CHECK(!r.ok || (r.dlls.size() == 1 && r.dlls[0] == "ddraw.dll"));
  }
  // A name that points outside the file.
  {
    std::vector<uint8_t> b = base;
    put32(b, l.idata + 12, 0xfffffff0);
    CHECK(fails(b));
  }
  // A name that runs to the end of the file with no terminator.
  {
    std::vector<uint8_t> b = base;
    put32(b, l.idata + 12, 0x11f0);  // file offset 0x3f0
    for (size_t i = 0x3f0; i < b.size(); ++i) b[i] = 'A';
    CHECK(fails(b));
  }
  // A name that is not text.
  {
    std::vector<uint8_t> b = base;
    b[0x300] = 0x01;
    CHECK(fails(b));
  }
  // A descriptor table with no terminator, filling the section to its end.
  {
    std::vector<uint8_t> b = base;
    for (size_t d = l.idata; d + 20 <= b.size(); d += 20) {
      for (size_t k = 0; k < 20; ++k) b[d + k] = 0x11;
      put32(b, d + 12, 0x1100);
    }
    CHECK(fails(b));
  }
  // An unknown optional header.
  {
    std::vector<uint8_t> b = base;
    put16(b, l.opt, 0x107);
    CHECK(fails(b));
  }
}

static void test_pe_file(const fs::path& tmp) {
  fs::path p = tmp / "game.exe";
  std::vector<uint8_t> b = make_pe({"D3D8.dll"});
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  pe::Imports r = pe::parse_file(p);
  CHECK(r.ok);
  CHECK(r.imports("d3d8"));
  CHECK(!pe::parse_file(tmp / "no-such.exe").ok);
}

// ---- NVIDIA -------------------------------------------------------------------

static const char* kLibs[] = {"libGLX_nvidia.so", "libEGL_nvidia.so", "libnvidia-glcore.so",
                              "libnvidia-glsi.so", "libnvidia-tls.so", "libnvidia-eglcore.so",
                              "libnvidia-gpucomp.so"};

// A host with the driver installed at `libver` and the kernel module at
// `kver`, with the host's Mesa beside it to prove it is never taken.
static fs::path nvidia_host(const fs::path& tmp, const std::string& name, const std::string& kver,
                            const std::string& libver, bool manifests = true) {
  fs::path root = tmp / name;
  fs::remove_all(root);
  if (!kver.empty()) write(root / "sys/module/nvidia/version", kver + "\n");
  fs::path lib = root / "usr/lib/x86_64-linux-gnu";
  fs::create_directories(lib);
  if (!libver.empty()) {
    for (const char* l : kLibs) write(lib / (std::string(l) + "." + libver), "elf");
  }
  write(lib / "libnvidia-egl-wayland.so.1.1.20", "elf");
  fs::create_symlink("libnvidia-egl-wayland.so.1.1.20", lib / "libnvidia-egl-wayland.so.1");
  // A stale SONAME link from the last driver, which must not be followed.
  fs::create_symlink("libGLX_nvidia.so.1.0", lib / "libGLX_nvidia.so.0");
  write(lib / "libGLX_mesa.so.0", "host mesa");
  write(lib / "libvulkan_radeon.so", "host mesa");
  if (manifests) {
    write(root / "usr/share/vulkan/icd.d/nvidia_icd.json",
          R"({"file_format_version":"1.0.1","ICD":{"library_path":"libGLX_nvidia.so.0","api_version":"1.4.303"}})");
    write(root / "usr/share/vulkan/icd.d/radeon_icd.x86_64.json",
          R"({"ICD":{"library_path":"/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so"}})");
    write(root / "usr/share/glvnd/egl_vendor.d/10_nvidia.json",
          R"({"ICD":{"library_path":"libEGL_nvidia.so.0"}})");
    write(root / "usr/share/glvnd/egl_vendor.d/50_mesa.json", R"({"ICD":{"library_path":"libEGL_mesa.so.0"}})");
    write(root / "usr/share/egl/egl_external_platform.d/10_nvidia_wayland.json",
          R"({"ICD":{"library_path":"libnvidia-egl-wayland.so.1"}})");
  }
  return root;
}

static void test_nvidia_matched(const fs::path& tmp) {
  fs::path root = nvidia_host(tmp, "nv-ok", "595.84", "595.84");
  gpu::Nvidia n = gpu::capture_nvidia(gpu::Host::fixture(root));
  CHECK(n.state == NvidiaState::Ok);
  CHECK_EQ(n.kernel_version, std::string("595.84"));
  CHECK_EQ(n.libs.size(), 8u);  // seven at the release, and egl-wayland
  for (const fs::path& p : n.libs) {
    std::string f = p.filename().string();
    CHECK(!contains(f, "mesa") && !contains(f, "radeon"));
    CHECK(f == "libnvidia-egl-wayland.so.1.1.20" || contains(f, ".so.595.84"));
  }
  CHECK(n.problems.empty());
  CHECK_EQ(n.icd.size(), 1u);
  CHECK_EQ(n.egl_vendor.size(), 1u);
  CHECK_EQ(n.egl_platform.size(), 1u);

  fs::path dir = tmp / "gl" / "nvidia-595.84";
  gpu::Routing rt = gpu::route_nvidia(n, dir);
  CHECK_EQ(env_of(rt.env, "__GLX_VENDOR_LIBRARY_NAME"), std::string("nvidia"));
  CHECK_EQ(env_of(rt.env, "VK_DRIVER_FILES"), (dir / "icd.d" / "nvidia_icd.json").string());
  CHECK_EQ(env_of(rt.env, "__EGL_VENDOR_LIBRARY_FILENAMES"),
           (dir / "egl_vendor.d" / "10_nvidia.json").string());
  CHECK_EQ(env_of(rt.env, "__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS"),
           (dir / "egl_external_platform.d").string());
  // The SONAME is ours and points at the release that matches, not at the
  // host's stale link.
  CHECK_EQ(fs::read_symlink(dir / "libGLX_nvidia.so.0").string(), std::string("libGLX_nvidia.so.595.84"));
  CHECK_EQ(fs::read_symlink(dir / "libnvidia-egl-wayland.so.1").string(),
           std::string("libnvidia-egl-wayland.so.1.1.20"));
  CHECK(fs::exists(dir / "libnvidia-glcore.so.595.84"));
  CHECK(!fs::exists(dir / "libGLX_mesa.so.0"));
  CHECK(contains(slurp(dir / "icd.d" / "nvidia_icd.json"), (dir / "libGLX_nvidia.so.0").string()));
  CHECK(contains(slurp(dir / "egl_external_platform.d" / "10_nvidia_wayland.json"),
                 (dir / "libnvidia-egl-wayland.so.1").string()));

  // A three-part release, as the production branch numbers them.
  fs::path root3 = nvidia_host(tmp, "nv-ok3", "580.65.06", "580.65.06");
  CHECK(gpu::capture_nvidia(gpu::Host::fixture(root3)).state == NvidiaState::Ok);
}

static void test_nvidia_mismatch(const fs::path& tmp) {
  // Updated and not restarted: the module is still the old one's successor.
  fs::path root = nvidia_host(tmp, "nv-mismatch", "595.84", "590.48");
  gpu::Nvidia n = gpu::capture_nvidia(gpu::Host::fixture(root));
  CHECK(n.state == NvidiaState::Mismatch);
  CHECK_EQ(n.library_version, std::string("590.48"));
  CHECK(n.libs.empty());
  CHECK_EQ(n.problems.size(), 1u);
  if (!n.problems.empty()) {
    CHECK(contains(n.problems[0].what, "595.84"));
    CHECK(contains(n.problems[0].what, "590.48"));
    CHECK(contains(n.problems[0].fix, "restart"));
    CHECK(contains(n.problems[0].fix, "software rendering"));
  }
  CHECK(gpu::route_nvidia(n, tmp / "gl" / "nope").env.empty());
  CHECK(!fs::exists(tmp / "gl" / "nope"));

  // One library left behind at the old release is as much a mismatch.
  fs::path mixed = nvidia_host(tmp, "nv-mixed", "595.84", "595.84");
  fs::remove(mixed / "usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.595.84");
  write(mixed / "usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.590.48", "elf");
  CHECK(gpu::capture_nvidia(gpu::Host::fixture(mixed)).state == NvidiaState::Mismatch);
}

static void test_nvidia_missing_and_absent(const fs::path& tmp) {
  fs::path root = nvidia_host(tmp, "nv-missing", "595.84", "");
  gpu::Nvidia n = gpu::capture_nvidia(gpu::Host::fixture(root));
  CHECK(n.state == NvidiaState::Missing);
  CHECK_EQ(n.problems.size(), 1u);
  if (!n.problems.empty()) {
    CHECK(contains(n.problems[0].what, "libGLX_nvidia.so.595.84"));
    CHECK(contains(n.problems[0].fix, "Install"));
  }

  fs::path none = nvidia_host(tmp, "nv-absent", "", "595.84");
  gpu::Nvidia a = gpu::capture_nvidia(gpu::Host::fixture(none));
  CHECK(a.state == NvidiaState::Absent);
  CHECK(a.problems.empty());
  CHECK(a.libs.empty());

  // The oldest drivers publish only /proc/driver/nvidia/version.
  fs::path proc = nvidia_host(tmp, "nv-proc", "", "470.256.02");
  write(proc / "proc/driver/nvidia/version",
        "NVRM version: NVIDIA UNIX x86_64 Kernel Module  470.256.02  Thu May  2 14:37:44 UTC 2024\n");
  gpu::Nvidia p = gpu::capture_nvidia(gpu::Host::fixture(proc));
  CHECK_EQ(p.kernel_version, std::string("470.256.02"));
  CHECK(p.state == NvidiaState::Ok);

  // Matched libraries but no Vulkan manifest: usable, and said.
  fs::path novk = nvidia_host(tmp, "nv-novk", "595.84", "595.84", false);
  gpu::Nvidia v = gpu::capture_nvidia(gpu::Host::fixture(novk));
  CHECK(v.state == NvidiaState::Ok);
  CHECK_EQ(v.problems.size(), 1u);
  if (!v.problems.empty()) CHECK(contains(v.problems[0].what, "Vulkan"));
}

// A current driver, as 595 installs it: the X11 and wayland2 EGL platforms
// have manifests of their own, and the allocator is loaded by SONAME. Each
// manifest adopted must name a library that was taken, or it names nothing.
static void test_nvidia_current_driver(const fs::path& tmp) {
  fs::path root = nvidia_host(tmp, "nv-595", "595.84", "595.84");
  fs::path lib = root / "usr/lib/x86_64-linux-gnu";
  write(lib / "libnvidia-allocator.so.595.84", "elf");
  fs::create_symlink("libnvidia-allocator.so.595.84", lib / "libnvidia-allocator.so.1");
  for (const char* p : {"libnvidia-egl-xcb.so", "libnvidia-egl-xlib.so", "libnvidia-egl-wayland2.so"}) {
    std::string real = std::string(p) + (std::string(p) == "libnvidia-egl-wayland2.so" ? ".1.0.1" : ".1.0.5");
    write(lib / real, "elf");
    fs::create_symlink(real, lib / (std::string(p) + ".1"));
  }
  write(root / "usr/share/egl/egl_external_platform.d/20_nvidia_xcb.json",
        R"({"ICD":{"library_path":"libnvidia-egl-xcb.so.1"}})");
  write(root / "usr/share/egl/egl_external_platform.d/09_nvidia_wayland2.json",
        R"({"ICD":{"library_path":"libnvidia-egl-wayland2.so.1"}})");
  // The same Vulkan manifest in /etc as well as /usr/share.
  write(root / "etc/vulkan/icd.d/nvidia_icd.json",
        R"({"ICD":{"library_path":"libGLX_nvidia.so.0","api_version":"1.4.303"}})");

  gpu::Nvidia n = gpu::capture_nvidia(gpu::Host::fixture(root));
  CHECK(n.state == NvidiaState::Ok);
  CHECK_EQ(n.libs.size(), 12u);
  CHECK_EQ(n.icd.size(), 1u);

  fs::path dir = tmp / "gl" / "nvidia-595-current";
  gpu::Routing rt = gpu::route_nvidia(n, dir);
  CHECK_EQ(env_of(rt.env, "VK_DRIVER_FILES"), (dir / "icd.d" / "nvidia_icd.json").string());
  CHECK_EQ(fs::read_symlink(dir / "libnvidia-allocator.so.1").string(),
           std::string("libnvidia-allocator.so.595.84"));
  CHECK_EQ(fs::read_symlink(dir / "libnvidia-egl-xcb.so.1").string(), std::string("libnvidia-egl-xcb.so.1.0.5"));
  CHECK(contains(slurp(dir / "egl_external_platform.d" / "20_nvidia_xcb.json"),
                 (dir / "libnvidia-egl-xcb.so.1").string()));
  CHECK(contains(slurp(dir / "egl_external_platform.d" / "09_nvidia_wayland2.json"),
                 (dir / "libnvidia-egl-wayland2.so.1").string()));

  // An allocator left at the last release is a mismatch like any other.
  fs::path stale = nvidia_host(tmp, "nv-595-stale", "595.84", "595.84");
  write(stale / "usr/lib/x86_64-linux-gnu/libnvidia-allocator.so.590.48", "elf");
  CHECK(gpu::capture_nvidia(gpu::Host::fixture(stale)).state == NvidiaState::Mismatch);

  // NixOS: every file in /run/opengl-driver/lib is a link into the store,
  // and egl-wayland is still taken.
  fs::path nix = tmp / "nv-nix";
  fs::remove_all(nix);
  write(nix / "sys/module/nvidia/version", "595.84\n");
  fs::path store = nix / "nix/store/abc-nvidia-x11-595.84/lib";
  fs::path drv = nix / "run/opengl-driver/lib";
  fs::create_directories(drv);
  for (const char* l : kLibs) {
    std::string f = std::string(l) + ".595.84";
    write(store / f, "elf");
    fs::create_symlink(store / f, drv / f);
  }
  write(store / "libnvidia-egl-wayland.so.1.1.21", "elf");
  fs::create_symlink(store / "libnvidia-egl-wayland.so.1.1.21", drv / "libnvidia-egl-wayland.so.1.1.21");
  gpu::Nvidia nn = gpu::capture_nvidia(gpu::Host::fixture(nix));
  CHECK(nn.state == NvidiaState::Ok);
  bool wayland = false;
  for (const fs::path& p : nn.libs) wayland = wayland || p.filename() == "libnvidia-egl-wayland.so.1.1.21";
  CHECK(wayland);
}

// The runtime's own Mesa manifests are named explicitly, and a host path in
// one of them is moved inside the runtime - the host's Mesa is never loaded.
static void test_materialize_routes_mesa(const fs::path& tmp) {
  fs::path root = nvidia_host(tmp, "nv-route", "595.84", "595.84");
  fs::path runtime = tmp / "runtime";
  write(runtime / "usr/lib/x86_64-linux-gnu/libvulkan_radeon.so", "ours");
  write(runtime / "usr/share/vulkan/icd.d/radeon_icd.x86_64.json",
        R"({"ICD":{"library_path":"/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so"}})");
  write(runtime / "usr/share/vulkan/icd.d/lvp_icd.x86_64.json",
        R"({"ICD":{"library_path":"/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so"}})");
  write(runtime / "usr/share/vulkan/icd.d/radeon_icd.i686.json", R"({"ICD":{"library_path":"x"}})");
  write(runtime / "usr/share/glvnd/egl_vendor.d/50_mesa.json", R"({"ICD":{"library_path":"libEGL_mesa.so.0"}})");

  gpu::Report r = gpu::probe(gpu::Host::fixture(root, {{"DISPLAY", ":0"}}));
  CHECK(r.nvidia.usable());
  gpu::materialize(r, tmp / "gl-route", runtime);
  std::string vk = env_of(r.env, "VK_DRIVER_FILES");
  fs::path mesa = tmp / "gl-route" / ("mesa-" + state_key(runtime)) / "icd.d";
  CHECK_EQ(vk, (tmp / "gl-route/nvidia-595.84/icd.d/nvidia_icd.json").string() + ":" +
                   (mesa / "lvp_icd.x86_64.json").string() + ":" + (mesa / "radeon_icd.x86_64.json").string());
  CHECK(contains(slurp(mesa / "radeon_icd.x86_64.json"),
                 (runtime / "usr/lib/x86_64-linux-gnu/libvulkan_radeon.so").string()));
  // Not in the runtime: a bare name, which only our library path can answer.
  CHECK(contains(slurp(mesa / "lvp_icd.x86_64.json"), R"("library_path": "libvulkan_lvp.so")"));
  CHECK(!contains(vk, "i686"));
  CHECK(!contains(vk, root.string()));  // nothing of the host's Mesa
  CHECK(contains(env_of(r.env, "__EGL_VENDOR_LIBRARY_FILENAMES"), "50_mesa.json"));
  CHECK_EQ(r.library_path, (tmp / "gl-route/nvidia-595.84").string());

  // Every start of a bundle does this - --doctor too - while a game started
  // earlier from the same state reads what it wrote. A newer build's runtime
  // is mounted elsewhere, and its manifests go beside this runtime's rather
  // than over them: the running game's still name its own Mesa.
  fs::path newer = tmp / "runtime-1.1";
  write(newer / "usr/lib/x86_64-linux-gnu/libvulkan_radeon.so", "theirs");
  write(newer / "usr/share/vulkan/icd.d/radeon_icd.x86_64.json",
        R"({"ICD":{"library_path":"/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so"}})");
  gpu::Report r2 = gpu::probe(gpu::Host::fixture(root, {{"DISPLAY", ":0"}}));
  gpu::materialize(r2, tmp / "gl-route", newer);
  CHECK(contains(slurp(mesa / "radeon_icd.x86_64.json"),
                 (runtime / "usr/lib/x86_64-linux-gnu/libvulkan_radeon.so").string()));
  CHECK(contains(env_of(r2.env, "VK_DRIVER_FILES"), "mesa-" + state_key(newer)));
  CHECK(!contains(env_of(r2.env, "VK_DRIVER_FILES"), "mesa-" + state_key(runtime)));
  // The same runtime again leaves each manifest in place, whole, and takes
  // away one the runtime no longer has.
  write(mesa / "gone_icd.x86_64.json", "{}");
  gpu::Report r3 = gpu::probe(gpu::Host::fixture(root, {{"DISPLAY", ":0"}}));
  gpu::materialize(r3, tmp / "gl-route", runtime);
  CHECK_EQ(env_of(r3.env, "VK_DRIVER_FILES"), vk);
  CHECK(fs::exists(mesa / "radeon_icd.x86_64.json"));
  CHECK(!fs::exists(mesa / "gone_icd.x86_64.json"));
  size_t entries = 0;
  for (const fs::directory_entry& de : fs::directory_iterator(mesa)) {
    (void)de;
    ++entries;
  }
  CHECK_EQ(entries, size_t{2});
  CHECK(fs::exists(tmp / "gl-route/nvidia-595.84/icd.d/nvidia_icd.json"));

  // With the NVIDIA driver mismatched nothing of it is routed, and Mesa is.
  fs::path bad = nvidia_host(tmp, "nv-route-bad", "595.84", "590.48");
  gpu::Report rb = gpu::probe(gpu::Host::fixture(bad, {{"DISPLAY", ":0"}}));
  gpu::materialize(rb, tmp / "gl-route-bad", runtime);
  CHECK(env_of(rb.env, "__GLX_VENDOR_LIBRARY_NAME").empty());
  CHECK(!contains(env_of(rb.env, "VK_DRIVER_FILES"), "nvidia"));
  CHECK(rb.library_path.empty());
}

// ---- host capabilities --------------------------------------------------------

static const char* kVulkanRadv14 = R"(==========
VULKANINFO
==========

Vulkan Instance Version: 1.4.304

Devices:
========
GPU0:
	apiVersion         = 1.4.305
	driverVersion      = 25.0.7
	vendorID           = 0x1002
	deviceID           = 0x73bf
	deviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
	deviceName         = AMD Radeon RX 6800 (RADV NAVI21)
	driverID           = DRIVER_ID_MESA_RADV
	driverName         = radv
GPU1:
	apiVersion         = 1.4.305
	deviceType         = PHYSICAL_DEVICE_TYPE_CPU
	deviceName         = llvmpipe (LLVM 19.1.1, 256 bits)
	driverName         = llvmpipe
)";

static void test_vulkaninfo_parse() {
  gpu::VulkanInfo v = gpu::parse_vulkaninfo_summary(kVulkanRadv14);
  CHECK(v.ran);
  CHECK(v.level == VulkanLevel::V1_4);
  // The loader's 1.4.304 caps the device's 1.4.305.
  CHECK_EQ(v.api_version, std::string("1.4.304"));
  CHECK_EQ(v.device, std::string("AMD Radeon RX 6800 (RADV NAVI21)"));
  CHECK_EQ(v.driver, std::string("radv"));

  // The older summary format: the packed number, then the version.
  gpu::VulkanInfo old = gpu::parse_vulkaninfo_summary(
      "Devices:\nGPU0:\n\tapiVersion     = 4206847 (1.3.255)\n\tdeviceType     = "
      "PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU\n\tdeviceName     = Intel(R) UHD Graphics 620\n");
  CHECK(old.level == VulkanLevel::V1_3);
  CHECK_EQ(old.api_version, std::string("1.3.255"));

  // An old loader holds a new device to what it can offer.
  gpu::VulkanInfo capped = gpu::parse_vulkaninfo_summary(
      "Vulkan Instance Version: 1.3.280\nGPU0:\n\tapiVersion = 1.4.305\n\tdeviceType = "
      "PHYSICAL_DEVICE_TYPE_DISCRETE_GPU\n");
  CHECK(capped.level == VulkanLevel::V1_3);

  gpu::VulkanInfo legacy = gpu::parse_vulkaninfo_summary(
      "GPU0:\n\tapiVersion = 1.2.131\n\tdeviceType = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU\n");
  CHECK(legacy.level == VulkanLevel::Legacy);

  gpu::VulkanInfo cpu = gpu::parse_vulkaninfo_summary(
      "GPU0:\n\tapiVersion = 1.4.305\n\tdeviceType = PHYSICAL_DEVICE_TYPE_CPU\n\tdeviceName = llvmpipe\n");
  CHECK(cpu.ran);
  CHECK(cpu.level == VulkanLevel::None);
  CHECK(cpu.software_only);

  gpu::VulkanInfo nothing = gpu::parse_vulkaninfo_summary("");
  CHECK(!nothing.ran);
  CHECK(nothing.level == VulkanLevel::None);
}

static void test_glxinfo_parse() {
  gpu::GlInfo hw = gpu::parse_glxinfo(
      "name of display: :0\ndirect rendering: Yes\nOpenGL renderer string: AMD Radeon RX 6800 "
      "(radeonsi, navi21, LLVM 19.1.1, DRM 3.61)\nOpenGL version string: 4.6 (Compatibility Profile) "
      "Mesa 25.0.7\n");
  CHECK(hw.ran);
  CHECK(hw.hardware);
  CHECK(contains(hw.version, "4.6"));
  gpu::GlInfo sw = gpu::parse_glxinfo("OpenGL renderer string: llvmpipe (LLVM 19.1.1, 256 bits)\n");
  CHECK(sw.ran);
  CHECK(!sw.hardware);
  CHECK(!gpu::parse_glxinfo("Error: unable to open display\n").ran);
}

static void test_gamescope_and_session(const fs::path& tmp) {
  auto gs = [&](std::map<std::string, std::string> env) {
    return gpu::detect_gamescope(gpu::Host::fixture(tmp, std::move(env)));
  };
  CHECK(gs({{"XDG_CURRENT_DESKTOP", "gamescope"}}));
  CHECK(gs({{"XDG_CURRENT_DESKTOP", "GameScope"}}));
  CHECK(gs({{"XDG_CURRENT_DESKTOP", "KDE:gamescope"}}));
  CHECK(gs({{"GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0"}}));
  CHECK(!gs({{"XDG_CURRENT_DESKTOP", "KDE"}}));
  CHECK(!gs({{"XDG_CURRENT_DESKTOP", "gamescopey"}}));
  CHECK(!gs({}));

  auto ss = [&](std::map<std::string, std::string> env) {
    return gpu::detect_session(gpu::Host::fixture(tmp, std::move(env)));
  };
  CHECK(ss({{"XDG_SESSION_TYPE", "wayland"}}) == gpu::SessionType::Wayland);
  CHECK(ss({{"XDG_SESSION_TYPE", "x11"}, {"WAYLAND_DISPLAY", "wayland-0"}}) == gpu::SessionType::X11);
  CHECK(ss({{"XDG_SESSION_TYPE", "tty"}, {"DISPLAY", ":0"}}) == gpu::SessionType::X11);
  CHECK(ss({{"WAYLAND_DISPLAY", "wayland-0"}}) == gpu::SessionType::Wayland);
  CHECK(ss({}) == gpu::SessionType::None);

  gpu::HostCaps c;
  c.gamescope = true;
  CHECK(player::display_path(c) == player::DisplayPath::GamescopeDirect);
  c.gamescope = false;
  CHECK(player::display_path(c) == player::DisplayPath::NestedWeston);
}

// A whole machine: DRM, sound, input and FUSE, all from a fixture tree.
static void test_probe_caps(const fs::path& tmp) {
  fs::path root = tmp / "host-amd";
  fs::remove_all(root);
  write(root / "sys/class/drm/card0/device/vendor", "0x1002\n");
  fs::create_directories(root / "sys/bus/pci/drivers/amdgpu");
  fs::create_symlink("../../../../bus/pci/drivers/amdgpu", root / "sys/class/drm/card0/device/driver");
  fs::create_directories(root / "sys/class/drm/card0/device/drm/renderD128");
  fs::create_directories(root / "sys/class/drm/card0-DP-1");  // a connector, not a card
  write(root / "dev/dri/renderD128", "");
  write(root / "run/user/1000/pulse/native", "");
  write(root / "run/user/1000/pipewire-0", "");
  write(root / "run/user/1000/wayland-0", "");
  write(root / "dev/input/event0", "");
  write(root / "dev/input/event1", "");
  write(root / "dev/input/event5", "");  // a keyboard: never counted
  if (geteuid() != 0) fs::permissions(root / "dev/input/event5", fs::perms::none);
  fs::create_directories(root / "dev/input/by-id");
  fs::create_symlink("../event0", root / "dev/input/by-id/usb-Pad_One-event-joystick");
  fs::create_symlink("../event1", root / "dev/input/by-id/usb-Pad_Two-event-joystick");
  fs::create_symlink("../event5", root / "dev/input/by-id/usb-Some_Keyboard-event-kbd");
  write(root / "dev/fuse", "");
  write(root / "usr/bin/fusermount3", "");

  gpu::Host h = gpu::Host::fixture(root, {{"XDG_RUNTIME_DIR", "/run/user/1000"},
                                          {"WAYLAND_DISPLAY", "wayland-0"},
                                          {"XDG_SESSION_TYPE", "wayland"},
                                          {"XDG_CURRENT_DESKTOP", "GNOME"},
                                          {"KRETRO_MOUNT_MODE", "fusermount"}});
  gpu::Report g = gpu::probe(h);
  CHECK_EQ(g.devices.size(), 1u);
  if (!g.devices.empty()) {
    CHECK(g.devices[0].vendor == gpu::Vendor::Amd);
    CHECK_EQ(g.devices[0].driver, std::string("amdgpu"));
    CHECK_EQ(g.devices[0].node, std::string("/dev/dri/renderD128"));
    CHECK(g.devices[0].readable);
  }
  CHECK(g.wayland);
  CHECK(g.issues.empty());

  gpu::Probes probes;
  probes.vulkaninfo = [] { return std::optional<std::string>(kVulkanRadv14); };
  probes.glxinfo = [] { return std::optional<std::string>(); };
  gpu::HostCaps c = gpu::probe_caps(h, g, probes);
  CHECK(c.vulkan.level == VulkanLevel::V1_4);
  CHECK(!c.gl.ran);
  CHECK(c.render_device);
  CHECK(c.other_gpu);
  CHECK(c.gpu_usable());
  CHECK(c.session == gpu::SessionType::Wayland);
  CHECK(!c.gamescope);
  CHECK(c.pulse);
  CHECK_EQ(c.audio_server, std::string("pipewire-pulse"));
  CHECK_EQ(c.input_nodes, 2);
  CHECK_EQ(c.input_readable, 2);
  CHECK(c.fuse());
  CHECK_EQ(c.mount_mode, std::string("fusermount"));
  CHECK(c.problems.empty());

  // Take the sound server, FUSE and the gamepads' permissions away.
  fs::remove_all(root / "run/user/1000/pulse");
  fs::remove(root / "usr/bin/fusermount3");
  if (geteuid() != 0) {
    fs::permissions(root / "dev/input/event0", fs::perms::none);
    fs::permissions(root / "dev/input/event1", fs::perms::none);
  }
  gpu::HostCaps d = gpu::probe_caps(h, g, probes);
  CHECK(!d.pulse);
  CHECK_EQ(d.audio_server, std::string("pipewire, without its pulse socket"));
  CHECK(!d.fuse());
  std::string all;
  for (const gpu::Problem& p : d.problems) all += p.line() + "\n";
  CHECK(contains(all, "sound server"));
  CHECK(contains(all, "FUSE"));
  if (geteuid() != 0) {
    CHECK_EQ(d.input_readable, 0);
    CHECK(contains(all, "gamepad is connected"));
  }
  for (const gpu::Problem& p : d.problems) CHECK(!p.blocking());

  // An NVIDIA machine whose driver does not match: no GPU to be had.
  fs::path nv = nvidia_host(tmp, "host-nv-bad", "595.84", "590.48");
  write(nv / "sys/class/drm/card0/device/vendor", "0x10de\n");
  fs::create_directories(nv / "sys/class/drm/card0/device/drm/renderD128");
  write(nv / "dev/dri/renderD128", "");
  gpu::Host hn = gpu::Host::fixture(nv, {{"DISPLAY", ":0"}});
  gpu::Report gn = gpu::probe(hn);
  CHECK(gn.nvidia.state == NvidiaState::Mismatch);
  CHECK_EQ(gn.issues.size(), 1u);
  gpu::Probes lvp;
  lvp.vulkaninfo = [] {
    return std::optional<std::string>("GPU0:\n\tapiVersion = 1.4.305\n\tdeviceType = PHYSICAL_DEVICE_TYPE_CPU\n");
  };
  gpu::HostCaps cn = gpu::probe_caps(hn, gn, lvp);
  CHECK(cn.render_device);
  CHECK(!cn.other_gpu);
  CHECK(!cn.gpu_usable());

  // The same, on a laptop whose Intel half still works.
  write(nv / "sys/class/drm/card1/device/vendor", "0x8086\n");
  fs::create_directories(nv / "sys/class/drm/card1/device/drm/renderD129");
  write(nv / "dev/dri/renderD129", "");
  gpu::Probes intel;
  intel.vulkaninfo = [] {
    return std::optional<std::string>(
        "GPU0:\n\tapiVersion = 1.3.289\n\tdeviceType = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU\n");
  };
  gpu::HostCaps ci = gpu::probe_caps(hn, gpu::probe(hn), intel);
  CHECK(ci.other_gpu);
  CHECK(ci.gpu_usable());
  CHECK(ci.vulkan.level == VulkanLevel::V1_3);
}

// ---- the backend table --------------------------------------------------------

static gpu::HostCaps caps(VulkanLevel vk, bool gl = true, NvidiaState nv = NvidiaState::Absent,
                          bool other_gpu = true) {
  gpu::HostCaps c;
  c.render_device = true;
  c.other_gpu = other_gpu;
  c.nvidia = nv;
  c.vulkan.ran = true;
  c.vulkan.level = vk;
  c.vulkan.api_version = vk == VulkanLevel::V1_4 ? "1.4.305" : vk == VulkanLevel::V1_3 ? "1.3.289"
                         : vk == VulkanLevel::Legacy ? "1.2.131" : "";
  c.gl.ran = true;
  c.gl.hardware = gl;
  return c;
}

static pe::Imports exe(std::vector<std::string> dlls, bool d3d_im = false) {
  pe::Imports i;
  i.ok = true;
  i.dlls = std::move(dlls);
  i.direct3d_im = d3d_im;
  return i;
}

static void test_backend_table() {
  struct Row {
    const char* what;
    AuthorBackend author;
    pe::Imports imports;
    gpu::HostCaps host;
    bool needs_gpu;
    Backend want;
  };
  const auto V14 = VulkanLevel::V1_4, V13 = VulkanLevel::V1_3, OLD = VulkanLevel::Legacy,
             NONE = VulkanLevel::None;
  const auto A = AuthorBackend::Auto;
  const pe::Imports d3d9 = exe({"kernel32.dll", "d3d9.dll"});
  const pe::Imports d3d8 = exe({"d3d8.dll", "user32.dll"});
  const pe::Imports ddraw2d = exe({"ddraw.dll", "winmm.dll"});
  const pe::Imports ddraw3d = exe({"ddraw.dll"}, true);
  const pe::Imports gl = exe({"opengl32.dll", "kernel32.dll"});
  const pe::Imports plain = exe({"kernel32.dll", "user32.dll", "gdi32.dll"});
  pe::Imports unreadable;
  unreadable.error = "the import table is truncated";

  gpu::HostCaps unknown_vk = caps(NONE);
  unknown_vk.vulkan.ran = false;
  unknown_vk.gl.ran = false;

  const std::vector<Row> rows = {
      // auto: Direct3D 8 and 9 go by the Vulkan the GPU speaks
      {"d3d9, Vulkan 1.4", A, d3d9, caps(V14), false, Backend::Dxvk3},
      {"d3d9, Vulkan 1.3", A, d3d9, caps(V13), false, Backend::Dxvk2},
      {"d3d8, Vulkan 1.4", A, d3d8, caps(V14), false, Backend::Dxvk3},
      {"d3d8, Vulkan 1.3", A, d3d8, caps(V13), false, Backend::Dxvk2},
      {"d3d9, Vulkan 1.2", A, d3d9, caps(OLD), false, Backend::WineD3DVulkan},
      {"d3d9, no Vulkan", A, d3d9, caps(NONE), false, Backend::WineD3DGL},
      {"d3d9, Vulkan unknown", A, d3d9, unknown_vk, false, Backend::WineD3DGL},
      {"d3d9 and opengl32", A, exe({"opengl32.dll", "d3d9.dll"}), caps(V14), false, Backend::Dxvk3},
      {"dxgi", A, exe({"dxgi.dll"}), caps(V13), false, Backend::Dxvk2},
      // auto: DirectDraw, 2D and 3D
      {"ddraw 2D", A, ddraw2d, caps(V14), false, Backend::CncDdraw},
      {"ddraw 2D, no Vulkan", A, ddraw2d, caps(NONE), false, Backend::CncDdraw},
      {"ddraw with Direct3D IID", A, ddraw3d, caps(V14), false, Backend::WineD3DVulkan},
      {"ddraw with Direct3D IID, Vulkan 1.2", A, ddraw3d, caps(OLD), false, Backend::WineD3DVulkan},
      {"ddraw with Direct3D IID, no Vulkan", A, ddraw3d, caps(NONE), false, Backend::WineD3DGL},
      {"ddraw and d3dim", A, exe({"ddraw.dll", "d3dim.dll"}), caps(V13), false, Backend::WineD3DVulkan},
      {"ddraw and d3dim700", A, exe({"ddraw.dll", "d3dim700.dll"}), caps(V13), false,
       Backend::WineD3DVulkan},
      {"ddraw and d3drm", A, exe({"ddraw.dll", "d3drm.dll"}), caps(V13), false, Backend::WineD3DVulkan},
      // auto: OpenGL, and nothing recognisable
      {"opengl32", A, gl, caps(V14), false, Backend::NativeGL},
      {"opengl32 and ddraw", A, exe({"ddraw.dll", "opengl32.dll"}), caps(V14), false, Backend::NativeGL},
      {"GDI only", A, plain, caps(V13), false, Backend::WineD3DVulkan},
      {"GDI only, no Vulkan", A, plain, caps(NONE), false, Backend::WineD3DGL},
      {"unreadable exe", A, unreadable, caps(V14), false, Backend::WineD3DVulkan},
      // the author's choice
      {"author dxvk, 1.4", AuthorBackend::Dxvk, plain, caps(V14), false, Backend::Dxvk3},
      {"author dxvk, 1.3", AuthorBackend::Dxvk, plain, caps(V13), false, Backend::Dxvk2},
      {"author dxvk, 1.2", AuthorBackend::Dxvk, plain, caps(OLD), false, Backend::WineD3DVulkan},
      {"author dxvk, none", AuthorBackend::Dxvk, plain, caps(NONE), false, Backend::WineD3DGL},
      {"author wined3d-vk", AuthorBackend::WineD3DVulkan, d3d9, caps(V14), false, Backend::WineD3DVulkan},
      {"author wined3d-vk, none", AuthorBackend::WineD3DVulkan, d3d9, caps(NONE), false,
       Backend::WineD3DGL},
      {"author wined3d-gl", AuthorBackend::WineD3DGL, d3d9, caps(V14), false, Backend::WineD3DGL},
      {"author cnc-ddraw", AuthorBackend::CncDdraw, d3d9, caps(V14), false, Backend::CncDdraw},
      // NVIDIA's driver unusable, and nothing else to draw with
      {"nvidia mismatch", A, d3d9, caps(V14, true, NvidiaState::Mismatch, false), false,
       Backend::Software},
      {"nvidia mismatch, needs a GPU", A, d3d9, caps(V14, true, NvidiaState::Mismatch, false), true,
       Backend::Refuse},
      {"nvidia missing", A, ddraw2d, caps(NONE, false, NvidiaState::Missing, false), false,
       Backend::Software},
      {"nvidia missing, needs a GPU", A, ddraw2d, caps(NONE, false, NvidiaState::Missing, false), true,
       Backend::Refuse},
      {"nvidia mismatch, author dxvk", AuthorBackend::Dxvk, d3d9,
       caps(V14, true, NvidiaState::Mismatch, false), false, Backend::Software},
      {"nvidia mismatch on a laptop with Intel", A, d3d9, caps(V13, true, NvidiaState::Mismatch, true),
       true, Backend::Dxvk2},
      {"nvidia matched", A, d3d9, caps(V14, true, NvidiaState::Ok, false), true, Backend::Dxvk3},
      // no GPU at all
      {"no GPU", A, gl, caps(NONE, false), false, Backend::Software},
      {"no GPU, needs one", A, gl, caps(NONE, false), true, Backend::Refuse},
      {"needs a GPU and has one", A, gl, caps(V13), true, Backend::NativeGL},
  };

  for (const Row& r : rows) {
    player::Decision d = player::choose_backend(r.author, r.imports, r.host, r.needs_gpu);
    ++checks;
    if (d.backend != r.want) {
      ++failures;
      std::fprintf(stderr, "  FAIL backend table: %s -> %s, wanted %s (%s)\n", r.what,
                   player::backend_name(d.backend), player::backend_name(r.want), d.reason.c_str());
    }
    CHECK(!d.reason.empty());
  }

  player::Decision refused = player::choose_backend(
      A, d3d9, caps(V14, true, NvidiaState::Mismatch, false), true);
  CHECK(contains(refused.reason, "needs a GPU"));
  CHECK(contains(refused.reason, "NVIDIA"));
  player::Decision soft = player::choose_backend(
      A, d3d9, caps(V14, true, NvidiaState::Mismatch, false), false);
  CHECK(contains(soft.reason, "software rendering"));

  CHECK(player::parse_author_backend("dxvk") == AuthorBackend::Dxvk);
  CHECK(player::parse_author_backend("wined3d-vk") == AuthorBackend::WineD3DVulkan);
  CHECK(player::parse_author_backend("wined3d-gl") == AuthorBackend::WineD3DGL);
  CHECK(player::parse_author_backend("cnc-ddraw") == AuthorBackend::CncDdraw);
  CHECK(player::parse_author_backend("auto") == AuthorBackend::Auto);
  CHECK(player::parse_author_backend("dgvoodoo") == AuthorBackend::Auto);
}

static bool has_reg(const player::Settings& s, const std::string& name, const std::string& data,
                    bool dword) {
  for (const player::RegValue& v : s.registry) {
    if (v.name == name && v.data == data && v.dword == dword) return true;
  }
  return false;
}

static void test_settings() {
  gpu::HostCaps c = caps(VulkanLevel::V1_4);
  auto with = [&](Backend b) { return player::settings_for(player::Decision{b, "because"}, c); };

  player::Settings d3 = with(Backend::Dxvk3);
  CHECK(d3.dll_dirs == std::vector<std::string>{"opt/dxvk-3"});
  CHECK(std::find(d3.dlls.begin(), d3.dlls.end(), "d3d9") != d3.dlls.end());
  CHECK(std::find(d3.dlls.begin(), d3.dlls.end(), "d3d8") != d3.dlls.end());
  CHECK(contains(d3.dll_overrides, "d3d9"));
  CHECK(contains(d3.dll_overrides, "=n,b"));
  CHECK_EQ(env_of(d3.env, "DXVK_LOG_LEVEL"), std::string("none"));
  CHECK(!has_reg(d3, "renderer", "vulkan", false));

  player::Settings d2 = with(Backend::Dxvk2);
  CHECK(!d2.dll_dirs.empty() && d2.dll_dirs[0] == "opt/dxvk-2");

  CHECK(has_reg(with(Backend::WineD3DVulkan), "renderer", "vulkan", false));
  CHECK(has_reg(with(Backend::WineD3DGL), "renderer", "gl", false));
  CHECK(with(Backend::WineD3DVulkan).dll_overrides.empty());

  player::Settings cnc = with(Backend::CncDdraw);
  CHECK(cnc.dlls == std::vector<std::string>{"ddraw"});
  CHECK_EQ(cnc.dll_overrides, std::string("ddraw=n,b"));

  player::Settings sw = with(Backend::Software);
  CHECK_EQ(env_of(sw.env, "LIBGL_ALWAYS_SOFTWARE"), std::string("1"));
  CHECK_EQ(env_of(sw.env, "GALLIUM_DRIVER"), std::string("llvmpipe"));
  CHECK(has_reg(sw, "renderer", "gl", false));

  // What every choice shares: sound through pulse then alsa, pads through SDL.
  for (Backend b : {Backend::Dxvk3, Backend::Dxvk2, Backend::WineD3DVulkan, Backend::WineD3DGL,
                    Backend::CncDdraw, Backend::NativeGL, Backend::Software}) {
    player::Settings s = with(b);
    CHECK(has_reg(s, "Audio", "pulse,alsa", false));
    CHECK(has_reg(s, "DisableHidraw", "1", true));
    CHECK(has_reg(s, "Enable SDL", "1", true));
    CHECK(!s.refused());
  }
  player::Settings no = with(Backend::Refuse);
  CHECK(no.refused());
  CHECK(no.registry.empty());
  CHECK(no.dlls.empty());
  CHECK_EQ(player::audio_drivers(), std::string("pulse,alsa"));

  // plan() is the two together, including the display.
  gpu::HostCaps deck = caps(VulkanLevel::V1_3);
  deck.gamescope = true;
  player::Settings p = player::plan(AuthorBackend::Auto, exe({"d3d9.dll"}), deck, false);
  CHECK(p.decision.backend == Backend::Dxvk2);
  CHECK(p.display == player::DisplayPath::GamescopeDirect);

  std::string reg = player::registry_file(
      {{"HKEY_CURRENT_USER\\Software\\Wine\\Direct3D", "renderer", false, "vulkan"},
       {"HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\WineBus", "DisableHidraw", true, "1"},
       {"HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\WineBus", "Enable SDL", true, "1"},
       {"HKEY_CURRENT_USER\\Software\\Wine\\Drivers", "Audio", false, "a\"b\\c"}});
  CHECK_EQ(reg, std::string("REGEDIT4\n"
                            "\n[HKEY_CURRENT_USER\\Software\\Wine\\Direct3D]\n"
                            "\"renderer\"=\"vulkan\"\n"
                            "\n[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\WineBus]\n"
                            "\"DisableHidraw\"=dword:00000001\n"
                            "\"Enable SDL\"=dword:00000001\n"
                            "\n[HKEY_CURRENT_USER\\Software\\Wine\\Drivers]\n"
                            "\"Audio\"=\"a\\\"b\\\\c\"\n"));
}

// A refusal handed to a session is said before anything is looked up, locked
// or mounted: here the game is not even installed, and the refusal is still
// the answer rather than "not installed".
static void test_session_refuses_first() {
  session::Options o;
  o.backend = player::plan(AuthorBackend::Auto, exe({"d3d9.dll"}),
                           caps(VulkanLevel::V1_4, true, NvidiaState::Mismatch, false), true);
  CHECK(o.backend->refused());
  std::string said;
  try {
    session::play(rt::Env{}, "kretro-test-no-such-game", o);
  } catch (const std::exception& ex) {
    said = ex.what();
  }
  CHECK(contains(said, "needs a GPU"));
  CHECK(!contains(said, "not installed"));
}

// PE imports straight into the table: the fixture executables decide.
static void test_backend_from_fixture_pe() {
  gpu::HostCaps c = caps(VulkanLevel::V1_3);
  auto pick = [&](const std::vector<uint8_t>& b) {
    return player::choose_backend(AuthorBackend::Auto, pe::parse(b), c, false).backend;
  };
  CHECK(pick(make_pe({"KERNEL32.dll", "d3d9.dll"})) == Backend::Dxvk2);
  CHECK(pick(make_pe({"DDRAW.dll", "WINMM.dll"})) == Backend::CncDdraw);
  CHECK(pick(make_pe({"OPENGL32.dll"}, true)) == Backend::NativeGL);
  std::vector<uint8_t> d3d7 = make_pe({"DDRAW.dll"});
  const uint8_t iid3[16] = {0x40, 0x32, 0x22, 0xbb, 0x2b, 0xe7, 0xd0, 0x11,
                            0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e};
  std::memcpy(&d3d7[0x390], iid3, 16);  // IID_IDirect3D3: Direct3D 6
  CHECK(pick(d3d7) == Backend::WineD3DVulkan);
}

// ---- the doctor ---------------------------------------------------------------

static void test_redact() {
  using player::doctor::redact;
  CHECK_EQ(redact("state /home/kim/.local/share/kretro, kim ran it", "/home/kim", "kim"),
           std::string("state ~/.local/share/kretro, <user> ran it"));
  CHECK_EQ(redact("/home/kim", "/home/kim/", "kim"), std::string("~"));
  // Somebody else's home, and a longer name that merely starts with ours.
  CHECK_EQ(redact("/home/kimberly/x", "/home/kim", "kim"), std::string("/home/<user>/x"));
  CHECK_EQ(redact("/var/home/kim/a", "/var/home/kim", "kim"), std::string("~/a"));
  CHECK_EQ(redact("/var/home/sam/a", "/var/home/kim", "kim"), std::string("/home/<user>/a"));
  CHECK_EQ(redact("/root/.cache", "/root", "root"), std::string("~/.cache"));
  // The name as a word, in any case; not inside another word.
  CHECK_EQ(redact("KIM and Kim, kimono", "/home/kim", "kim"), std::string("<user> and <user>, kimono"));
  CHECK_EQ(redact("user=kim;", "", "kim"), std::string("user=<user>;"));
  // A one-letter name would take every "a" with it and hide nobody.
  CHECK_EQ(redact("a cat", "/home/a", "a"), std::string("a cat"));
  // An empty or root home is not a directory anybody is found by.
  CHECK_EQ(redact("/usr/lib", "/", "kim"), std::string("/usr/lib"));
  CHECK_EQ(redact("/run/user/1000/wayland-0", "/home/kim", "kim"), std::string("/run/user/1000/wayland-0"));
}

static void test_doctor_report(const fs::path& tmp) {
  namespace doc = player::doctor;
  fs::path root = nvidia_host(tmp, "doctor-host", "595.84", "590.48");
  write(root / "etc/os-release", "NAME=\"Ubuntu\"\nPRETTY_NAME=\"Ubuntu 26.04 LTS\"\n");
  write(root / "proc/sys/kernel/osrelease", "7.0.0-31-generic\n");
  write(root / "lib64/ld-linux-x86-64.so.2", "");

  doc::Inputs in;
  in.host = gpu::Host::fixture(root, {{"XDG_SESSION_TYPE", "x11"}, {"DISPLAY", ":0"}});
  in.gpu = gpu::probe(in.host);
  gpu::Probes none;
  in.caps = gpu::probe_caps(in.host, in.gpu, none);
  in.runtime_glibc = "2.42";
  in.bundle_id = "retro-pack";
  in.bundle_title = "Retro Pack";
  in.bundle_version = "1.0";
  in.runtime_version = "rt-abc";
  in.log_tail = "wine: cannot open /home/kim/.local/share/retro-pack/example/prefix\n";
  in.extra.push_back({"runtime", {{"wine", "/run/user/1000/kretro/rt/usr/bin/wine"}}});

  doc::Report r = doc::collect(in);
  std::string text = doc::render(r);
  CHECK(contains(text, "Ubuntu 26.04 LTS"));
  CHECK(contains(text, "7.0.0-31-generic"));
  CHECK(contains(text, "glibc"));
  CHECK(contains(text, "2.42"));
  CHECK(contains(text, "kernel 595.84, libraries 590.48: MISMATCH"));
  CHECK(contains(text, "retro-pack - Retro Pack"));
  CHECK(contains(text, "nested-weston"));
  CHECK(contains(text, "last session log"));
  CHECK(contains(text, "problems\n"));
  CHECK(!r.blocking());

  // One line per failure, each saying what to do.
  size_t lines = 0;
  std::istringstream in_text(text);
  std::string line;
  while (std::getline(in_text, line)) {
    if (line.rfind("  - ", 0) == 0 || line.rfind("  ! ", 0) == 0) ++lines;
  }
  CHECK_EQ(lines, r.problems.size());
  CHECK(r.problems.size() >= 2);  // the NVIDIA mismatch, and no sound server at least
  for (const gpu::Problem& p : r.problems) CHECK(!p.fix.empty());

  // The saved form carries neither the home directory nor the name.
  std::string saved = doc::redact(text, "/home/kim", "kim");
  CHECK(!contains(saved, "/home/kim"));
  CHECK(!contains(saved, "kim"));
  CHECK(contains(saved, "~/.local/share/retro-pack/example/prefix"));

  // Blocking first, and each problem once however many roads reached it.
  in.gpu.issues.push_back({gpu::Problem::Severity::Blocking, "No display", "Run it from a desktop"});
  in.caps.problems.push_back({gpu::Problem::Severity::Blocking, "No display", "Run it from a desktop"});
  doc::Report b = doc::collect(in);
  CHECK(b.blocking());
  CHECK(!b.problems.empty() && b.problems[0].what == "No display");
  size_t dup = 0;
  for (const gpu::Problem& p : b.problems) dup += p.what == "No display";
  CHECK_EQ(dup, 1u);
  CHECK(contains(doc::render(b), "  ! No display. Run it from a desktop"));

  doc::Report empty;
  CHECK(contains(doc::render(empty), "no problems found"));

  fs::path log = tmp / "session.log";
  std::string many;
  for (int i = 1; i <= 100; ++i) many += "line " + std::to_string(i) + "\n";
  write(log, many);
  std::string tail = doc::tail_lines(log, 40);
  CHECK(contains(tail, "line 61\n"));
  CHECK(!contains(tail, "line 60\n"));
  CHECK(contains(tail, "line 100\n"));
  CHECK(doc::tail_lines(tmp / "nothing.log", 40).empty());
}

int main() {
  fs::path tmp = fs::temp_directory_path() / ("kretro-test-policy-" + std::to_string(getpid()));
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  try {
    test_pe_valid();
    test_pe_direct3d_iid();
    test_pe_truncated();
    test_pe_malicious();
    test_pe_file(tmp);
    test_nvidia_matched(tmp);
    test_nvidia_mismatch(tmp);
    test_nvidia_missing_and_absent(tmp);
    test_nvidia_current_driver(tmp);
    test_materialize_routes_mesa(tmp);
    test_vulkaninfo_parse();
    test_glxinfo_parse();
    test_gamescope_and_session(tmp);
    test_probe_caps(tmp);
    test_backend_table();
    test_settings();
    test_backend_from_fixture_pe();
    test_session_refuses_first();
    test_redact();
    test_doctor_report(tmp);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "  FAIL unexpected exception: %s\n", e.what());
    ++failures;
  }

  fs::remove_all(tmp);
  std::fprintf(stderr, "\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
