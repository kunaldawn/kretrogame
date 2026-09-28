#include "trim.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "../../pack/dwarfs.h"
#include "../../pack/pack.h"
#include "../../util/hash.h"

namespace kg::bundle {
namespace fs = std::filesystem;

SetMeta trim_meta(const SetMeta& s, const std::vector<std::string>& keep) {
  SetMeta t;
  t.set_id = s.set_id;
  t.body = s.body;
  for (const std::string& id : keep) {
    const Meta* g = s.find(id);
    if (!g) throw std::runtime_error("the set " + s.set_id + " does not hold " + id);
    t.games.push_back(*g);
  }
  for (const Meta::Disc& d : s.discs) {
    const bool used = std::any_of(t.games.begin(), t.games.end(), [&](const Meta& g) {
      return std::any_of(g.discs.begin(), g.discs.end(), [&](const Meta::Disc& x) { return x.key == d.key; });
    });
    if (used) t.discs.push_back(d);
  }
  return t;
}

fs::path trimmed_set(const fs::path& pack, const std::vector<std::string>& keep, const fs::path& tool,
                     const fs::path& cache, const Callbacks& cb) {
  Pack p = Pack::open(pack);
  const SetMeta& set = p.set();
  SetMeta t = trim_meta(set, keep);
  if (t.games.size() == set.games.size()) return pack;

  // Named for what it was cut from and what it kept, so a set rebuilt since -
  // another game added to it, a game reinstalled - is never served an old
  // trim, and a second build of the same bundle is served the first one's.
  std::vector<std::string> ids = keep;
  std::sort(ids.begin(), ids.end());
  std::string what = to_hex(set.body.blake3);
  for (const std::string& id : ids) what += "\n" + id;
  const std::string key = to_hex(hash_string(what)).substr(0, 16);
  const fs::path dir = cache / "trim";
  const fs::path out = dir / (set.set_id + "-" + key + ".kgpack");
  std::error_code ec;
  if (fs::exists(out, ec)) {
    try {
      if (Pack::open(out).set().root() == t.root()) return out;
    } catch (const std::exception&) {
    }
  }

  if (tool.empty() || !fs::exists(tool, ec)) throw std::runtime_error("no DwarFS tool to trim " + set.set_id + " with");
  if (!p.has_body()) throw std::runtime_error(pack.filename().string() + " is a recipe and carries no game");
  auto step = [&](std::string_view stage, uint64_t done) {
    if (cb.cancelled && cb.cancelled()) throw Cancelled();
    if (cb.progress) cb.progress(Progress{stage, done, 4});
  };
  const fs::path scratch = dir / (key + ".scratch");
  struct Scratch {
    fs::path dir;
    ~Scratch() {
      std::error_code e;
      fs::remove_all(dir, e);
    }
  } s{scratch};
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch / "tree", ec);
  if (ec) throw std::runtime_error("cannot make " + scratch.string() + ": " + ec.message());

  step("unpacking", 0);
  extract_body_tree(tool, p, scratch / "tree", scratch / "body.image");

  step("trimming", 1);
  for (const Meta& g : set.games) {
    if (!t.find(g.id)) fs::remove_all(scratch / "tree" / body_game_dir(g.id), ec);
  }
  for (const Meta::Disc& d : set.discs) {
    const bool kept = std::any_of(t.discs.begin(), t.discs.end(), [&](const Meta::Disc& x) { return x.key == d.key; });
    if (!kept) fs::remove_all(scratch / "tree" / body_disc_dir(d.key), ec);
  }

  step("packing", 2);
  const fs::path body = scratch / "trimmed.dwarfs";
  ProcResult r = mkdwarfs_body(tool, scratch / "tree", body);
  if (!r.ok()) throw std::runtime_error("mkdwarfs failed:\n" + r.out);

  step("writing", 3);
  t.body.packing = kBodyPacking;
  try {
    WriteOptions wo;
    wo.kind = p.header().kind;
    wo.body = body;
    write_pack(out, t, wo);
    Pack::Verification v = Pack::open(out).verify();
    if (!v.ok) throw std::runtime_error("the trimmed pack does not verify: " + v.detail);
    if (cb.cancelled && cb.cancelled()) throw Cancelled();
  } catch (...) {
    fs::remove(out, ec);
    throw;
  }
  step("done", 4);
  return out;
}

}  // namespace kg::bundle
