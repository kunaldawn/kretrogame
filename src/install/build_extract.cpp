// Build's two methods that run no installer: the game copied, or unzipped,
// straight off the first disc into the staging tree.
#include "build.h"

#include <stdexcept>
#include <string>
#include <vector>

#include "../disc/members.h"
#include "../util/hash.h"
#include "../util/text.h"

namespace kg::install {
namespace fs = std::filesystem;

void Build::copy_from_disc(const std::string& subdir) {
  if (cancelled_) return;
  if (discs_.empty()) throw std::runtime_error("no disc to copy from");
  say_("extracting (copy)");
  // A second attempt replaces the first rather than landing on top of it: the
  // wizard can go back to step 3, name a different directory and try again,
  // and half of a wrong answer left underneath would go into the pack.
  std::error_code rec;
  fs::remove_all(tree_dir_, rec);
  std::string err;
  std::vector<std::string> listing = iso::list(env_, discs_[0].iso);
  // Joliet casing is not predictable, so the requested spelling is resolved
  // against what the disc actually says - a manifest's `PC` may be `pc`.
  std::string real = subdir.empty() ? "" : iso::resolve(listing, subdir);
  if (!subdir.empty() && real.empty()) {
    throw std::runtime_error("the disc has no directory called " + subdir);
  }
  if (!iso::extract_subtree(env_, discs_[0].iso, real, tree_dir_, &err)) {
    throw std::runtime_error("extraction failed:\n" + err);
  }
}

void Build::unzip_from_disc(const std::string& member, const std::string& subdir) {
  if (cancelled_) return;
  if (discs_.empty()) throw std::runtime_error("no disc to unpack from");
  say_("extracting (unzip)");
  // As above; and the rename at the end of this cannot move a tree onto a
  // non-empty one, so without it a second try fails on the first one's
  // leavings rather than replacing them.
  std::error_code ec;
  fs::remove_all(tree_dir_, ec);
  fs::remove_all(work_ / "member", ec);
  fs::remove_all(work_ / "unpacked", ec);
  std::vector<std::string> listing = iso::list(env_, discs_[0].iso);
  std::string real = iso::resolve(listing, member);
  if (real.empty()) throw std::runtime_error("the disc has no member called " + member);

  fs::path staged = work_ / "member";
  say_("  taking " + real + " off the disc");
  std::string err;
  if (!iso::extract_member(env_, discs_[0].iso, real, staged, &err)) {
    throw std::runtime_error("could not take " + real + " off the disc:\n" + err);
  }
  fs::path archive;
  for (const fs::directory_entry& de : fs::directory_iterator(staged, ec)) archive = de.path();
  if (archive.empty()) throw std::runtime_error("nothing came out of " + real);

  // The archive is an anchor: it is the part of the disc this install actually
  // depends on, so its hash is what a rebuild has to match. It is hashed from
  // the extracted file rather than streamed out of 7z, because a member can be
  // gigabytes and streaming it would mean holding all of it.
  say_("  hashing " + archive.filename().string());
  anchor_ = Anchor{real, fs::file_size(archive, ec), hash_file(archive)};

  fs::path z = rt::which(env_, "7z");
  fs::path unpacked = work_ / "unpacked";
  fs::create_directories(unpacked, ec);
  say_("  unpacking " + archive.filename().string());
  ProcResult r = rt::run(env_, z, {"x", "-y", "-o" + unpacked.string(), archive.string()});
  if (!r.ok()) {
    throw std::runtime_error("could not unpack " + archive.filename().string() + ":\n" + r.out);
  }

  fs::path from = unpacked;
  if (!subdir.empty()) {
    bool found = false;
    for (const fs::directory_entry& de : fs::directory_iterator(unpacked, ec)) {
      if (to_lower(de.path().filename().string()) == to_lower(subdir)) {
        from = de.path();
        found = true;
        break;
      }
    }
    if (!found) throw std::runtime_error("the archive has no directory called " + subdir);
  }
  fs::rename(from, tree_dir_, ec);
  if (ec) throw std::runtime_error("cannot move the game into place: " + ec.message());
}

}  // namespace kg::install
