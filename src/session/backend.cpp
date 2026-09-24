#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "../util/format.h"
#include "../util/proc.h"
#include "internal.h"

namespace kg::session::detail {
namespace fs = std::filesystem;

// Puts a backend decision into the prefix and the environment: the DLLs it
// brings, the overrides that make Wine load them, and its registry values -
// all written as one .reg file and imported with one Wine start, because each
// `wine reg add` is a wineserver start of its own.
//
// DLLs are copied on every launch rather than once. wineboot -u, which a
// newer runtime runs over an old prefix, puts Wine's own builtins back in
// system32 and would silently turn DXVK off.
void apply_backend(const rt::Env& e, rt::Env& we, const fs::path& prefix,
                   const backend::Plan& s) {
  if (s.refused()) throw std::runtime_error(s.decision.reason);
  const std::string name = backend::backend_name(s.decision.backend);
  log_line("graphics: " + name + " - " + s.decision.reason);
  for (const auto& kv : s.env) we.set(kv.first, kv.second);

  std::error_code ec;
  bool dlls_in = s.dlls.empty();
  if (!s.dlls.empty()) {
    fs::path src;
    for (const std::string& d : s.dll_dirs) {
      if (fs::is_directory(e.root / d, ec)) {
        src = e.root / d;
        break;
      }
    }
    // New WoW64 lays a prefix out as Windows does: 64-bit DLLs in system32,
    // 32-bit ones in syswow64. A 32-bit-only prefix has system32 alone.
    fs::path win = prefix / "drive_c" / "windows";
    const bool wow = fs::is_directory(win / "syswow64", ec);
    const fs::path dir32 = win / (wow ? "syswow64" : "system32");
    size_t n = 0;
    for (const std::string& dll : s.dlls) {
      if (src.empty()) break;
      const std::string f = dll + ".dll";
      for (const auto& [from, to] : {std::pair{src / "x32" / f, dir32},
                                     std::pair{src / "x64" / f, wow ? win / "system32" : fs::path()},
                                     std::pair{src / f, dir32}}) {
        if (to.empty() || !fs::exists(from, ec)) continue;
        fs::copy_file(from, to / f, fs::copy_options::overwrite_existing, ec);
        if (!ec) ++n;
      }
    }
    dlls_in = n > 0;
    if (!dlls_in) log_line("warning: " + name + " is not in this runtime; Wine's own Direct3D draws instead");
  }
  // Overriding a DLL that was not put there would make Wine look for a native
  // copy, find none, and fall back - or, for ddraw, fail outright.
  if (dlls_in && !s.dll_overrides.empty()) we.append("WINEDLLOVERRIDES", s.dll_overrides, ';');

  if (!s.registry.empty()) {
    // Inside drive_c, so Wine is handed a path it can name without a Z:
    // drive, which a player's prefix does not have.
    fs::path reg = prefix / "drive_c" / "kretro-backend.reg";
    std::ofstream(reg) << backend::registry_file(s.registry);
    ProcResult r = rt::run(we, rt::find_wine(e.root), {"regedit", "/S", "C:\\kretro-backend.reg"});
    if (!r.ok()) log_line("warning: the " + name + " settings could not be written to the registry");
  }
}

}  // namespace kg::session::detail
