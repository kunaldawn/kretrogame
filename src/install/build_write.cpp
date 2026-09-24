// Build's last step: the body laid out, hashed, packed and written as a
// .kgpack. write(Meta) is its steps in order; write(Draft) is the wizard's way
// in.
#include "build.h"

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

std::vector<BodyDisc> Build::collect_body_discs(const Meta& m) {
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
  // All discs or none. lay_out_body numbers them 1..n and Meta.discs[i] is
  // disc i+1 for every reader of that layout, so leaving one out would
  // renumber the rest; Draft::embed_discs is one checkbox for the same reason.
  bool embed = true;
  for (const Meta::Disc& d : m.discs) {
    if (!d.embedded) embed = false;
  }
  disc_trees_.resize(discs_.size());
  std::vector<BodyDisc> body_discs;
  if (embed) {
    for (size_t i = 0; i < discs_.size(); ++i) {
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
      body_discs.push_back(BodyDisc{disc_trees_[i], discs_[i].label, discs_[i].serial, tracks});
    }
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

fs::path Build::lay_out_and_hash(Meta& m, const std::vector<BodyDisc>& body_discs,
                                 const fs::path& system_dir) {
  say_("laying out the body");
  fs::path stage = work_ / "body";
  lay_out_body(stage, tree_dir_, body_discs, m.registry.fragment, system_dir);
  // Every pack this engine writes is rooted, discs or no discs: registry.reg
  // is in the body either way, and session::open_layers only looks for
  // the game at image/game when the layout says to. draft_to_meta already
  // says "rooted"; this is the line that makes that true.
  m.layout = "rooted";

  // The tree covers game/ and nothing else, with paths relative to it. That is
  // what keeps the Merkle root meaning the identity of the installed game:
  // this pack and a pack of the same install built without its discs have the
  // same root, `kretro verify` reports the game rather than the game plus two
  // gigabytes of disc, and a session's exit-time diff compares like with like.
  // What is outside game/ is covered by the body hash instead.
  say_("hashing the tree");
  m.tree = Tree::from_directory(stage / "game");
  return stage;
}

fs::path Build::pack_body(Meta& m, const fs::path& stage) {
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
  // Said in the pack, so a pack made before these flags can be told from one
  // made with them, and offered a repack when it goes into a player.
  m.body.packing = kBodyPacking;
  return body;
}

Result Build::write_pack(const Meta& m, const fs::path& body) {
  std::error_code ec;
  fs::path out = game_pack(m.id);
  WriteOptions wo;
  wo.kind = PackKind::Game;
  wo.body = body;
  // Qualified: inside Build, the bare name is this member.
  kg::write_pack(out, m, wo);

  Result res;
  res.pack = out;
  res.tree_bytes = m.tree.total_bytes();
  res.entries = m.tree.size();
  res.pack_bytes = fs::file_size(out, ec);
  res.root = m.tree.root();
  return res;
}

Result Build::write(Meta m) {
  place_game_tree(m);
  merge_registry_and_anchor(m);
  check_verify(m);
  std::vector<BodyDisc> body_discs = collect_body_discs(m);
  fs::path system_dir = collect_system_files(m);
  fs::path stage = lay_out_and_hash(m, body_discs, system_dir);
  fs::path body = pack_body(m, stage);
  return write_pack(m, body);
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
