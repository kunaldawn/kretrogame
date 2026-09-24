#include "exe_probe.h"

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <optional>
#include <utility>

#include "../../backend/policy.h"
#include "../../gpu/caps.h"
#include "../../util/proc.h"
#include "../../util/text.h"

namespace kg::bundle {
namespace fs = std::filesystem;

// ---- graphics -------------------------------------------------------------------------

AutoBackend auto_backend(const pe::Imports& exe) {
  AutoBackend a;
  a.known = exe.ok;
  gpu::HostCaps caps;
  caps.vulkan.ran = true;
  caps.vulkan.level = gpu::VulkanLevel::V1_4;
  caps.vulkan.api_version = "1.4";
  caps.gl.ran = true;
  caps.gl.hardware = true;
  caps.render_device = true;
  caps.other_gpu = true;
  backend::Decision d = backend::choose_backend(backend::AuthorBackend::Auto, exe, caps, false);
  a.reason = d.reason;
  switch (d.backend) {
    case backend::Backend::Dxvk3:
    case backend::Backend::Dxvk2: a.backend = "dxvk"; break;
    case backend::Backend::WineD3DVulkan: a.backend = "wined3d-vk"; break;
    case backend::Backend::WineD3DGL: a.backend = "wined3d-gl"; break;
    case backend::Backend::CncDdraw: a.backend = "cnc-ddraw"; break;
    case backend::Backend::NativeGL: a.backend = "native OpenGL"; break;
    default: a.backend = backend::backend_name(d.backend); break;
  }
  return a;
}

pe::Imports read_exe_imports(const PackFacts& f, const fs::path& tool, const fs::path& scratch) {
  pe::Imports none;
  auto fail = [&](std::string why) {
    none.ok = false;
    none.error = std::move(why);
    return none;
  };
  if (f.meta.run.exe.empty()) return fail("the pack names no executable");
  std::error_code ec;
  if (tool.empty() || !fs::exists(tool, ec)) return fail("no DwarFS tool to read the pack with");

  // The executable as the tree spells it. run.exe was typed by a person or an
  // installer, and Wine does not care about case; dwarfsextract's pattern does.
  std::string want = f.meta.run.exe;
  std::replace(want.begin(), want.end(), '\\', '/');
  while (!want.empty() && want.front() == '/') want.erase(want.begin());
  std::string inside;
  for (const TreeEntry& e : f.meta.tree.entries()) {
    if (e.is_regular() && to_lower(e.path) == to_lower(want)) { inside = e.path; break; }
  }
  if (inside.empty()) return fail(f.meta.run.exe + " is not in the pack's tree");
  if (f.meta.rooted()) inside = "game/" + inside;

  std::optional<Pack> p;
  try {
    p = Pack::open(f.path);
  } catch (const std::exception& ex) {
    return fail(ex.what());
  }
  if (!p->has_body()) return fail("the pack is a recipe and carries no game");
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  ProcResult r = kg::run({tool.string(), "--tool=dwarfsextract", "-i", f.path.string(), "-O",
                          std::to_string(p->base() + p->header().body_off), "-o", scratch.string(),
                          "--pattern", inside, "--log-level=error"});
  fs::path got = scratch / inside;
  pe::Imports out;
  if (!r.ok() || !fs::exists(got, ec)) {
    std::string why = r.out;
    while (!why.empty() && (why.back() == '\n' || why.back() == ' ')) why.pop_back();
    out = fail("could not read " + inside + " out of the pack" + (why.empty() ? "" : ": " + why));
  } else {
    out = pe::parse_file(got);
  }
  fs::remove_all(scratch, ec);
  return out;
}

bool glide_only(const pe::Imports& exe) {
  if (!exe.ok) return false;
  bool glide = exe.imports("glide") || exe.imports("glide2x") || exe.imports("glide3x");
  if (!glide) return false;
  for (const char* d : {"ddraw", "d3d8", "d3d9", "d3drm", "d3dim", "d3dim700", "opengl32", "d3d11", "dxgi"}) {
    if (exe.imports(d)) return false;
  }
  return !exe.direct3d_im;
}

}  // namespace kg::bundle
