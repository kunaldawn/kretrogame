// Build's last step: the body laid out, hashed, packed and written as a
// .kgpack. write(Meta) is its steps in order; write(Draft) is the wizard's way
// in.
#include "build.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../disc/members.h"
#include "../pack/dwarfs.h"
#include "../util/fs_ci.h"
#include "../util/paths.h"
#include "../wine/system_files.h"
#include "../session/lock.h"
#include "body.h"
#include "staging.h"

namespace kg::install {
namespace fs = std::filesystem;

void Build::place_game_tree(const Meta& m) {
  std::error_code ec;

  // The installer methods leave the game under C: and say where; copy and
  // unzip have already put it at tree_dir_. One place, either way, before
  // anything hashes it.
  if (!fs::exists(tree_dir_, ec) && !m.install.install_dir.empty()) {
    fs::rename(staging_drive_c(work_) / m.install.install_dir, tree_dir_, ec);
    if (ec) throw std::runtime_error("cannot move the game into place: " + ec.message());
  }
  if (!fs::exists(tree_dir_, ec)) throw std::runtime_error("there is no installed tree to pack");
}

void Build::merge_registry_and_anchor(Meta& m) const {
  if (m.registry.fragment.empty()) m.registry.fragment = fragment_;
  if (anchor_ && !m.recipe.fingerprints.empty()) {
    m.recipe.fingerprints[0].anchors.push_back(*anchor_);
  }
}

void Build::check_verify(const Meta& m) const {
  say_("verifying");
  for (const std::string& v : m.recipe.verify) {
    if (!exists_ci(tree_dir_, v)) {
      throw std::runtime_error("the extracted game has no " + v +
                               "; the disc is not what the manifest expects");
    }
  }
  if (!exists_ci(tree_dir_, m.run.exe)) {
    throw std::runtime_error("the extracted game has no " + m.run.exe);
  }
}

std::vector<BodyDisc> Build::collect_body_discs(const Meta& m, const std::vector<std::string>& only) {
  std::error_code ec;

  // Every disc travels, whatever method installed from it. An installer game
  // already has one extracted tree per drive letter; a copy or unzip game
  // never mounted its disc as a drive, so its tree is pulled off the image
  // here. mkdwarfs deduplicates the installed files against the disc files
  // they were copied from, so for a copy game game/ is very nearly free and
  // for an installer game the immovable assets - the data archives, the
  // video - are stored once. That dedup is what makes carrying everything
  // affordable rather than merely honest.
  //
  // m.discs names each disc's key and discs_ has its bytes; they were made
  // from each other, one for one, and the body needs both.
  if (m.discs.size() != discs_.size()) {
    throw std::runtime_error("the pack's disc list does not match the discs this install opened");
  }
  disc_trees_.resize(discs_.size());
  std::vector<BodyDisc> body_discs;
  for (size_t i = 0; i < discs_.size(); ++i) {
    // A disc the set already carries comes out of the set's own body: it is
    // not read off its image a second time.
    if (std::find(only.begin(), only.end(), m.discs[i].key) == only.end()) continue;
    if (disc_trees_[i].empty()) {
      fs::path t = work_ / ("disc-tree-" + std::to_string(i + 1));
      fs::create_directories(t, ec);
      say_("  reading " + discs_[i].label + " for the pack");
      std::string derr;
      if (!iso::extract_subtree(env_, discs_[i].iso, "", t, &derr)) {
        throw std::runtime_error("could not read " + discs_[i].label + ":\n" + derr);
      }
      disc_trees_[i] = t;
    }
    std::vector<fs::path> tracks;
    for (const disc::AudioTrack& a : discs_[i].audio) {
      if (!a.file.empty()) tracks.push_back(a.file);
    }
    body_discs.push_back(BodyDisc{m.discs[i].key, disc_trees_[i], discs_[i].label, discs_[i].serial, tracks});
  }
  return body_discs;
}

fs::path Build::collect_system_files(Meta& m) {
  // What the installer put on the machine that is not the game: the DLLs, the
  // OCXs, the shared runtime it dropped into windows/system32. The registry
  // fragment names those files by path and is applied verbatim on the machine
  // that opens this pack, so leaving them behind ships a set of registrations
  // pointing at nothing. Collected now, because this is the first moment both
  // facts exist at once - the diff, and which directory the game is.
  fs::path system_dir = work_ / "system";
  {
    std::error_code se;
    fs::remove_all(system_dir, se);
    std::vector<std::string> paths;
    for (const TreeEntry& t : c_written_) {
      if (!t.is_regular() && !t.is_symlink() && !t.is_dir()) continue;
      if (wine::is_outside_the_game(t.path, m.install.install_dir)) paths.push_back(t.path);
    }
    wine::SystemFiles got = wine::gather_system_files(staging_drive_c(work_), paths, system_dir);
    m.system.files = static_cast<uint32_t>(got.files);
    m.system.bytes = got.bytes;
    if (got.files) {
      say_("  " + std::to_string(got.files) +
           " files the installer wrote outside the game travel too");
    }
  }
  return system_dir;
}

void Build::collect_folds(const MergePlan& plan, const std::vector<ShelfSet>& sets, const Meta& m,
                          std::vector<BodyGame>& games, std::vector<BodyDisc>& discs) {
  if (plan.fold.empty()) return;
  // Unpacked rather than mounted: it needs no FUSE, and every piece of it can
  // then be moved into the new layout the way the new game's tree is. The
  // cost is the set's unpacked size here for the length of the build, which
  // an install from its discs needed anyway.
  const fs::path tool = kg::dwarfs_tool();
  if (tool.empty()) throw std::runtime_error("no DwarFS tool (KRETRO_DWARFS is unset)");
  std::vector<std::string> keep;
  for (const Meta::Disc& d : plan.meta.discs) keep.push_back(d.key);
  for (size_t i : plan.fold) {
    const ShelfSet& set = sets[i];
    const fs::path from = work_ / ("fold-" + set.meta.set_id);
    std::error_code ec;
    fs::remove_all(from, ec);
    fs::create_directories(from, ec);
    say_("  unpacking the set " + set.meta.set_id + " to add " + m.id + " to it");
    extract_body_tree(tool, Pack::open(set.path), from, work_ / "fold.image");
    collect_from_set(from, set.meta, m.id, keep, games, discs);
  }
}

fs::path Build::lay_out_and_hash(Meta& m, MergePlan& plan, const std::vector<BodyGame>& games,
                                 const std::vector<BodyDisc>& discs) {
  say_("laying out the body");
  fs::path stage = work_ / "body";
  std::vector<uint64_t> sizes = lay_out_set_body(stage, games, discs);
  for (size_t i = 0; i < discs.size(); ++i) {
    for (Meta::Disc& d : plan.meta.discs) {
      if (d.key == discs[i].key) d.bytes = sizes[i];
    }
  }
  for (Meta::Disc& d : m.discs) {
    for (const Meta::Disc& s : plan.meta.discs) {
      if (s.key == d.key) d.bytes = s.bytes;
    }
  }

  // The tree covers game/ and nothing else, with paths relative to it. That is
  // what keeps the Merkle root meaning the identity of the installed game:
  // `kretro verify` reports the game rather than the game plus two gigabytes
  // of disc, and a session's exit-time diff compares like with like. What is
  // outside game/ is covered by the body hash instead.
  say_("hashing the tree");
  m.tree = Tree::from_directory(stage / body_game_dir(m.id) / "game");
  // The plan's copy of this game was taken before there was a tree to hash.
  for (Meta& g : plan.meta.games) {
    if (g.id == m.id) g = m;
  }
  return stage;
}

fs::path Build::pack_body(const fs::path& stage) {
  say_("packing");
  // Looked up only now, after the body is laid out and hashed: a missing tool
  // stops the install here, with game/ already in place under the staging tree.
  const fs::path tool = kg::dwarfs_tool();
  if (tool.empty()) throw std::runtime_error("no DwarFS tool (KRETRO_DWARFS is unset)");
  fs::path body = work_ / "body.dwarfs";
  // --categorize stores what is already compressed - disc images of video,
  // the textures in a zipped data file - raw instead of squeezing it for nothing, and 4 MiB
  // blocks (-S 22) keep a random read to one block's worth of decompression.
  // A pack is played from a mount, so how fast it reads matters more than the
  // last few percent of size. Neither flag touches the Merkle root, which
  // covers the files, not how the body stores them.
  ProcResult r = kg::mkdwarfs_body(tool, stage, body);
  if (!r.ok()) throw std::runtime_error("mkdwarfs failed:\n" + r.out);
  return body;
}

Result Build::write_set(MergePlan& plan, const std::vector<ShelfSet>& sets, const Meta& m, const fs::path& body) {
  std::error_code ec;
  // Said in the pack, so a pack made with other flags can be told from one
  // made with these.
  plan.meta.body.packing = kBodyPacking;
  fs::path out = set_pack(plan.set_id);
  // Only a set being folded into this one may be replaced by it: any other
  // pack of this name holds games this plan knows nothing of.
  const bool ours = std::any_of(plan.fold.begin(), plan.fold.end(), [&](size_t i) { return sets[i].path == out; });
  if (!ours && fs::exists(out, ec)) {
    throw std::runtime_error(out.filename().string() + " is already on the shelf and is not the set this game joins");
  }
  // The metadata as a reader will see it, before anything is replaced or
  // removed: a set that would not open is the sets it folded, lost.
  try {
    SetMeta::decode(plan.meta.encode());
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("the new set would not open (") + ex.what() + "); the shelf is left as it was");
  }
  WriteOptions wo;
  wo.kind = PackKind::Game;
  wo.body = body;
  // Qualified: inside Build, the bare name is this member. write_pack puts the
  // set in place by a rename, so a game playing from the set it replaces keeps
  // the file it opened.
  kg::write_pack(out, plan.meta, wo);
  // Each index after the set it names, so a crash between the two leaves a
  // game where it was rather than pointing at a set that is not there yet;
  // and the folded sets last, when nothing names them any more.
  Result res;
  for (const Meta& g : plan.meta.games) {
    write_game_index(g.id, plan.set_id);
    res.set_games.push_back(g.id);
  }
  for (size_t i : plan.fold) {
    if (sets[i].path == out) continue;
    std::error_code rm;
    if (fs::remove(sets[i].path, rm)) {
      res.folded.push_back(sets[i].path);
    } else if (rm) {
      // Its games are in the new set and their indexes name it; this copy is
      // only disc space, and the next install that folds it takes each game
      // once.
      say_("  could not remove " + sets[i].path.string() + ": " + rm.message());
    }
  }

  res.pack = out;
  res.tree_bytes = m.tree.total_bytes();
  res.entries = m.tree.size();
  res.pack_bytes = fs::file_size(out, ec);
  res.root = m.tree.root();
  res.set_id = plan.set_id;
  return res;
}

Result Build::write(Meta m) {
  place_game_tree(m);
  merge_registry_and_anchor(m);
  check_verify(m);
  fs::path system_dir = collect_system_files(m);
  // One writer of the shelf at a time: an install reads the sets, rewrites
  // one and removes others, and two doing it at once would each write a set
  // without the other's game.
  session::GameLock shelf = lock_shelf();
  // Where this game goes: a set of its own, or the set its discs are already
  // in, with any set it bridges folded into that one.
  std::vector<ShelfSet> sets = shelf_sets();
  MergePlan plan = plan_merge(sets, m);
  std::vector<BodyGame> games = {BodyGame{m.id, tree_dir_, system_dir, m.registry.fragment}};
  std::vector<BodyDisc> discs = collect_body_discs(m, plan.new_disc_keys);
  collect_folds(plan, sets, m, games, discs);
  fs::path stage = lay_out_and_hash(m, plan, games, discs);
  fs::path body = pack_body(stage);
  return write_set(plan, sets, m, body);
}

Result Build::write(const Draft& d) {
  Meta m = draft_to_meta(d, discs_);
  // The verify list is read off the confirmed directory rather than named in
  // advance. Step 6 does that reading, the moment the user picks the
  // executable, and draft_to_meta carries the answer here - so this asks for
  // one only when the draft brought none. Computing one unconditionally, over
  // the top of the list it has just been handed, would make the field one
  // that is filled, carried and discarded on arrival, and two answers to one
  // question is one answer too many.
  m.recipe.verify = verify_for(d, installed_root());
  return write(std::move(m));
}

}  // namespace kg::install
