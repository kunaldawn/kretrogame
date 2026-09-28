#include "saves_transfer.h"

#include <cstdlib>
#include <stdexcept>
#include <system_error>

#include "../pack/kgpack.h"
#include "../util/paths.h"
#include "../util/proc.h"
#include "lock.h"
#include "saves.h"
#include "saves_layout.h"

namespace kg::session {
namespace fs = std::filesystem;

fs::path export_saves(const std::string& id, const fs::path& out) {
  fs::path live = session::live_dir(id);
  std::error_code ec;
  if (!fs::exists(live, ec) || fs::is_empty(live, ec)) {
    throw std::runtime_error("nothing has been written for " + id + " yet");
  }
  const char* tool = std::getenv("KRETRO_DWARFS");
  if (!tool) throw std::runtime_error("no DwarFS tool (KRETRO_DWARFS is unset)");

  Meta m;
  m.id = id;
  m.name = id + " saves";
  m.tree = Tree::from_directory(live);

  fs::path body = cache_dir() / (id + "-saves.dwarfs");
  ProcResult r = kg::run({tool, "--tool=mkdwarfs", "-i", live.string(), "-o", body.string(),
                          "--log-level=error", "--no-progress", "-f"});
  if (!r.ok()) throw std::runtime_error("could not pack the saves:\n" + r.out);

  fs::path dst = out.empty() ? fs::path(id + ".saves.kgpack") : out;
  write_pack(dst, set_of(m), WriteOptions{PackKind::SaveExport, body, false});
  fs::remove(body, ec);
  return dst;
}

void import_saves(const std::string& id, const fs::path& in) {
  Pack p = Pack::open(in);
  if (p.header().kind != PackKind::SaveExport) {
    throw std::runtime_error(in.string() + " is not a saves export");
  }
  std::error_code ec;
  fs::path live = session::live_dir(id);
  // The same live directory a running session has as the upper layer of its
  // overlay. Unpacking somebody else's saves into it while a game is playing
  // out of it is the same destruction restoring a snapshot would be.
  session::GameLock lock = session::lock_game(id);
  if (lock.busy()) {
    throw std::runtime_error(id + " is being played by another kretro right now. Close that "
                                  "one first - these saves would land underneath it.");
  }
  fs::create_directories(live, ec);

  // Whatever is there now becomes a snapshot first, so importing somebody
  // else's saves over your own is something you can undo.
  if (!fs::is_empty(live, ec)) session::snapshot(id, live);

  const char* tool = std::getenv("KRETRO_DWARFS");
  if (!tool) throw std::runtime_error("no DwarFS tool (KRETRO_DWARFS is unset)");
  fs::path body = cache_dir() / (id + "-import.dwarfs");
  p.extract_body(body);
  ProcResult r = kg::run({tool, "--tool=dwarfsextract", "-i", body.string(), "-o", live.string()});
  fs::remove(body, ec);
  if (!r.ok()) throw std::runtime_error("could not unpack the saves:\n" + r.out);
}

}  // namespace kg::session
