#include "share.h"

#include <fstream>
#include <stdexcept>

#include "../pack/kgpack.h"
#include "../util/paths.h"
#include "install.h"

namespace kg::install {
namespace fs = std::filesystem;

fs::path export_recipe(const std::string& id, const fs::path& out) {
  fs::path src = game_pack(id);
  std::error_code ec;
  if (!fs::exists(src, ec)) throw std::runtime_error(id + " is not installed");

  Pack p = Pack::open(src);
  const Meta& m = p.meta();
  if (m.recipe.method.empty()) {
    throw std::runtime_error(id + " has no recipe recorded, so it cannot be shared this way");
  }
  if (m.recipe.fingerprints.empty()) {
    throw std::runtime_error(id + " has no disc fingerprint recorded; reinstall it to record one");
  }

  fs::path dst = out.empty() ? fs::path(id + ".recipe.kgpack") : out;
  // No body. The tree, the recipe and the root travel; the bytes do not.
  write_pack(dst, m, WriteOptions{PackKind::Game, std::nullopt, false});
  return dst;
}

ImportResult import_pack(const rt::Env& e, const fs::path& in, bool replace,
                         const std::function<void(const std::string&)>& say) {
  ImportResult res;
  Pack p = Pack::open(in);
  const Meta& m = p.meta();
  res.id = m.id;
  res.name = m.name.empty() ? m.id : m.name;
  ensure_state_dirs();

  if (p.header().kind == PackKind::SaveExport) {
    throw std::runtime_error(
        "that is a saves pack, not a game. Import it with: kretro import-saves <file>");
  }

  std::error_code ec;
  fs::path dst = game_pack(m.id);

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
