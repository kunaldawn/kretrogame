// What an install left behind, read back: the registry and C: diffed against
// the snapshot, or the extracted tree surveyed, and the answers kept for write().
#include "build.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../wine/system_files.h"
#include "staging.h"

namespace kg::install {
namespace fs = std::filesystem;

void Build::diff_after() {
  std::vector<wine::RegValue> reg_after = wine::snapshot_prefix(prefix_);
  // The serial the person typed into the installer is theirs, not the game's,
  // and keys.h says so twice. It is held out of the fragment here, which is the
  // only place the fragment is made: everything downstream - Meta.registry,
  // registry.reg in the body, every recipe exported from the pack - reads what
  // this line produced. Said out loud rather than dropped quietly, because a
  // game whose key does not come back will ask for it on first run, and the
  // vault has it under this id for exactly that moment.
  std::vector<std::string> serials;
  fragment_ = wine::to_reg_fragment(wine::without_serials(wine::diff_reg(reg_before_, reg_after), &serials));
  for (const std::string& s : serials) {
    say_("  keeping your serial out of the pack: " + s);
  }

  Tree c_after = Tree::from_directory(staging_drive_c(work_));
  Tree::Diff cdiff = c_before_.diff_to(c_after);
  if (cdiff.added.empty() && cdiff.changed.empty()) {
    throw std::runtime_error(
        "the installer wrote nothing to C:. It did not run, or it was cancelled.");
  }
  say_("  it wrote " + std::to_string(cdiff.added.size()) + " files to C:");
  // Both lists, because a DLL an installer replaced is as much a part of what
  // it did as one it added, and the fragment that registers it does not
  // distinguish. This is kept whole and filtered in write(), once a person has
  // said which of these directories is the game.
  c_written_.clear();
  c_written_.reserve(cdiff.added.size() + cdiff.changed.size());
  for (const TreeEntry& t : cdiff.added) c_written_.push_back(t);
  for (const TreeEntry& t : cdiff.changed) c_written_.push_back(t);
  // Where it landed is now a question with a ranked list of answers rather
  // than a single guess that needed source.verify to be right in advance.
  candidates_ = rank_candidates(staging_drive_c(work_), cdiff);
}

wine::SystemFiles Build::outside(const fs::path& install_dir) const {
  wine::SystemFiles out;
  const std::string dir = install_dir.generic_string();
  for (const TreeEntry& t : c_written_) {
    if (!t.is_regular() && !t.is_symlink()) continue;
    if (!wine::is_outside_the_game(t.path, dir)) continue;
    ++out.files;
    if (t.is_regular()) out.bytes += t.size;
  }
  return out;
}

fs::path Build::installed_root() const {
  std::error_code ec;
  return fs::exists(tree_dir_, ec) ? tree_dir_ : staging_drive_c(work_);
}

void Build::survey_extracted() {
  if (cancelled_) return;
  std::error_code ec;
  if (!fs::exists(tree_dir_, ec)) {
    throw std::runtime_error("nothing came off the disc: there is no tree to pack");
  }
  // No installer ran, so there is no registry to diff either: the pack gets an
  // empty fragment rather than a stale one - and nothing was written to C: at
  // all, so there is no system/ to carry.
  fragment_.clear();
  c_written_.clear();
  Candidate c = survey_tree(tree_dir_);
  if (c.files == 0) {
    throw std::runtime_error(
        "nothing came off the disc: what was copied is an empty directory");
  }
  say_("  " + std::to_string(c.files) + " files came off the disc");
  candidates_ = {std::move(c)};
}

}  // namespace kg::install
