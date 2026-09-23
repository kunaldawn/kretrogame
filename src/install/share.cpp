#include "share.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "../pack/kgpack.h"
#include "../util/paths.h"
#include "install.h"
#include "../session/session.h"
#include "../util/proc.h"

namespace kg::install {
namespace fs = std::filesystem;

fs::path export_recipe(const std::string& id, const fs::path& out) {
  fs::path src = games_dir() / (id + ".kgpack");
  std::error_code ec;
  if (!fs::exists(src, ec)) throw std::runtime_error(id + " is not installed");

  Pack p = Pack::open(src);
  Meta m = p.meta();
  if (m.recipe.method.empty()) {
    throw std::runtime_error(id + " has no recipe recorded, so it cannot be shared this way");
  }
  if (m.recipe.fingerprints.empty()) {
    throw std::runtime_error(id + " has no disc fingerprint recorded; reinstall it to record one");
  }

  fs::path dst = out.empty() ? fs::path(id + ".recipe.kgpack") : out;
  // No body. The tree, the recipe and the root travel; the bytes do not.
  write_pack(dst, m, WriteOptions{Kind::Game, std::nullopt, false});
  return dst;
}

fs::path export_saves(const std::string& id, const fs::path& out, const rt::Env& e) {
  fs::path live = game_saves_dir(id) / "live";
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
  write_pack(dst, m, WriteOptions{Kind::SaveExport, body, false});
  fs::remove(body, ec);
  (void)e;
  return dst;
}

void import_saves(const std::string& id, const fs::path& in) {
  Pack p = Pack::open(in);
  if (p.header().kind != Kind::SaveExport) {
    throw std::runtime_error(in.string() + " is not a saves export");
  }
  std::error_code ec;
  fs::path live = game_saves_dir(id) / "live";
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

ImportResult import_pack(const rt::Env& e, const fs::path& in, bool replace,
                         const std::function<void(const std::string&)>& say) {
  ImportResult res;
  Pack p = Pack::open(in);
  const Meta& m = p.meta();
  res.id = m.id;
  res.name = m.name.empty() ? m.id : m.name;
  ensure_state_dirs();

  if (p.header().kind == Kind::SaveExport) {
    throw std::runtime_error(
        "that is a saves pack, not a game. Import it with: kretro import-saves <file>");
  }

  std::error_code ec;
  fs::path dst = games_dir() / (m.id + ".kgpack");

  if (p.has_body()) {
    // A capsule. Verify it, then it simply becomes the installed game - the
    // file already is what an install produces.
    res.had_body = true;
    say("verifying " + in.filename().string());
    Pack::Verification v = p.verify();
    if (!v.ok) throw std::runtime_error("this pack is damaged: " + v.detail);
    if (fs::exists(dst, ec)) {
      if (!replace) throw std::runtime_error(m.name + " is already installed");
      say("replacing the " + m.name + " already here");
      fs::remove(dst, ec);
    }
    fs::copy_file(in, dst, ec);
    if (ec) throw std::runtime_error("cannot place the pack: " + ec.message());
    res.pack = dst;
    res.root_matched = true;
    return res;
  }

  // A recipe. Rebuild it from a disc the importer owns, and prove the result.
  res.rebuilt = true;
  say("this is a recipe - rebuilding " + res.name + " from your own disc");
  Options opt;
  opt.force = true;
  opt.expect_root_set = true;
  opt.expect_root = p.header().blake3_root;
  Result r = run(e, m, opt, say);
  res.pack = r.pack;
  res.root_matched = r.root_matched;
  return res;
}

}  // namespace kg::install
