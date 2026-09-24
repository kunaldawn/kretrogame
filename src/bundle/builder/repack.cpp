#include "repack.h"

#include <stdexcept>
#include <string>
#include <string_view>

#include "../../pack/dwarfs.h"
#include "../../util/proc.h"

namespace kg::bundle {
namespace fs = std::filesystem;

bool packed_before_faster_loading(const PackFacts& f) {
  return f.meta.body.length > 0 && f.meta.body.packing != kBodyPacking;
}

void repack_for_faster_loading(const fs::path& pack, const fs::path& tool, const fs::path& scratch,
                               const Callbacks& cb) {
  std::error_code ec;
  if (tool.empty() || !fs::exists(tool, ec)) throw std::runtime_error("no DwarFS tool to repack with");
  Pack p = Pack::open(pack);
  const Header& h = p.header();
  if (!p.has_body()) throw std::runtime_error(pack.filename().string() + " is a recipe and carries no game");
  if (h.body_is_squashfs()) throw std::runtime_error(pack.filename().string() + " has a squashfs body");
  // A signature covers the body it signed, and a new body has none.
  if (h.flags & kSigned) throw std::runtime_error(pack.filename().string() + " is signed; repacking would unsign it");
  Meta m = p.meta();
  auto step = [&](std::string_view stage, uint64_t done) {
    if (cb.cancelled && cb.cancelled()) throw Cancelled();
    if (cb.progress) cb.progress(Progress{stage, done, 4});
  };

  struct Scratch {
    fs::path dir;
    ~Scratch() {
      std::error_code e;
      fs::remove_all(dir, e);
    }
  } s{scratch};
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  if (ec) throw std::runtime_error("cannot make " + scratch.string() + ": " + ec.message());

  step("unpacking", 0);
  const fs::path old_body = scratch / "old.dwarfs", tree = scratch / "tree";
  p.extract_body(old_body);
  fs::create_directories(tree, ec);
  ProcResult r = kg::run({tool.string(), "--tool=dwarfsextract", "-i", old_body.string(), "-o", tree.string(),
                          "--log-level=error"});
  if (!r.ok()) throw std::runtime_error("could not unpack " + pack.filename().string() + ":\n" + r.out);
  fs::remove(old_body, ec);

  // The same tree going back in, by its root, before the pack is touched: a
  // repack is only ever the same game stored another way.
  step("checking", 1);
  const Hash root = Tree::from_directory(m.rooted() ? tree / "game" : tree).root();
  if (root != m.tree.root()) {
    throw std::runtime_error(pack.filename().string() + " unpacks to a tree that is not its own (Merkle root " +
                             to_hex(root) + ", it says " + to_hex(m.tree.root()) + "); it was left as it is");
  }

  step("packing", 2);
  const fs::path new_body = scratch / "new.dwarfs";
  r = kg::mkdwarfs_body(tool, tree, new_body);
  if (!r.ok()) throw std::runtime_error("mkdwarfs failed:\n" + r.out);
  fs::remove_all(tree, ec);

  step("writing", 3);
  m.body.packing = kBodyPacking;
  fs::path next = pack;
  next += ".repack";
  try {
    WriteOptions wo;
    wo.kind = h.kind;
    wo.body = new_body;
    write_pack(next, m, wo);
    Pack::Verification v = Pack::open(next).verify();
    if (!v.ok) throw std::runtime_error("the repacked pack does not verify: " + v.detail);
    if (cb.cancelled && cb.cancelled()) throw Cancelled();
  } catch (...) {
    fs::remove(next, ec);
    throw;
  }
  // A session playing the game has the old pack open, and keeps reading the
  // file it opened; the next one opens this.
  fs::rename(next, pack, ec);
  if (ec) {
    std::error_code e2;
    fs::remove(next, e2);
    throw std::runtime_error("could not put the repacked pack in place: " + ec.message());
  }
  step("done", 4);
}

}  // namespace kg::bundle
