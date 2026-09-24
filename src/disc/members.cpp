#include "members.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include "../util/text.h"

namespace kg::iso {
namespace fs = std::filesystem;

namespace {

std::string rtrim(const std::string& s) {
  size_t b = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? "" : s.substr(0, b + 1);
}

// A mounted CD, or a disc somebody already extracted, is a directory, and
// there is no image for 7z to open: `7z l` on a directory exits 2 and every
// caller below turns that into an error the person cannot act on. The tree is
// the disc, so the four functions that reach for 7z read it directly instead.

// Every path under `dir`, spelled the way a 7z listing spells one: relative to
// the root, forward slashes, directories included.
std::vector<std::string> walk(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) throw std::runtime_error("cannot read " + dir.string() + ": " + ec.message());
  for (fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
    if (ec) throw std::runtime_error("cannot read " + dir.string() + ": " + ec.message());
    // Lexically, not fs::relative: that one resolves through symlinks, and a
    // linked entry would be listed by where it points rather than by the name
    // the disc gives it.
    fs::path rel = it->path().lexically_relative(dir);
    if (rel.empty()) continue;
    out.push_back(rel.generic_string());
  }
  // An increment that fails leaves the iterator at the end, so the loop above
  // ends the way an exhausted one does. Without this a disc that stopped being
  // readable half way through would list as the half that was.
  if (ec) throw std::runtime_error("cannot read " + dir.string() + ": " + ec.message());
  std::sort(out.begin(), out.end());
  return out;
}

// A real copy, not a link farm: what this produces goes into the pack body, so
// it has to survive the builder's filesystem going away. Symlinked directories
// are not descended into - a mount has none, and an extracted disc that does
// could be a loop.
bool copy_tree(const fs::path& from, const fs::path& to, std::string* err) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) {
    if (err) *err = "cannot make " + to.string() + ": " + ec.message();
    return false;
  }
  fs::recursive_directory_iterator it(from, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    if (err) *err = "cannot read " + from.string() + ": " + ec.message();
    return false;
  }
  for (fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
    if (ec) {
      if (err) *err = "cannot read " + from.string() + ": " + ec.message();
      return false;
    }
    fs::path rel = it->path().lexically_relative(from);
    if (rel.empty()) continue;
    fs::path dst = to / rel;
    std::error_code e2;
    if (it->is_directory(e2)) {
      fs::create_directories(dst, e2);
      if (e2) {
        if (err) *err = "cannot make " + dst.string() + ": " + e2.message();
        return false;
      }
      continue;
    }
    fs::create_directories(dst.parent_path(), e2);
    e2.clear();
    fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, e2);
    if (e2) {
      if (err) *err = "cannot copy " + it->path().string() + ": " + e2.message();
      return false;
    }
  }
  // As in walk(): a failed increment ends the loop rather than announcing
  // itself, and half a disc copied is not a disc copied.
  if (ec) {
    if (err) *err = "cannot read " + from.string() + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace

std::vector<std::string> list(const rt::Env& e, const fs::path& p) {
  std::error_code dec;
  if (fs::is_directory(p, dec)) {
    std::vector<std::string> out = walk(p);
    if (out.empty()) throw std::runtime_error(p.string() + " is empty; there is no disc in it");
    return out;
  }
  fs::path z = rt::which(e, "7z");
  if (z.empty()) throw std::runtime_error("no 7z in this runtime");
  // -slt gives one attribute per line, which parses without guessing at
  // column positions.
  ProcResult r = rt::run(e, z, {"l", "-slt", "-ba", p.string()});
  if (!r.ok()) throw std::runtime_error("7z could not read " + p.string() + ":\n" + r.out);

  std::vector<std::string> out;
  std::istringstream in(r.out);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("Path = ", 0) == 0) out.push_back(rtrim(line.substr(7)));
  }
  // bsdtar returns an empty listing with a zero exit status on some discs - a
  // silent failure that reads as an empty disc rather than an error. 7z does
  // not, but assert it anyway, because that is exactly the bug class here.
  if (out.empty()) throw std::runtime_error(p.string() + " lists as empty; it is probably not a disc image");
  return out;
}

std::string resolve(const std::vector<std::string>& listing, const std::string& want) {
  std::string w = to_lower(want);
  std::replace(w.begin(), w.end(), '\\', '/');
  for (const std::string& e : listing) {
    std::string c = to_lower(e);
    std::replace(c.begin(), c.end(), '\\', '/');
    if (c == w) return e;
  }
  // Try it as a basename, since a manifest may name a file without its
  // directory.
  for (const std::string& e : listing) {
    std::string c = to_lower(fs::path(e).filename().string());
    if (c == w) return e;
  }
  return "";
}

bool extract_member(const rt::Env& e, const fs::path& iso, const std::string& member,
                    const fs::path& out_dir, std::string* err) {
  std::error_code dec;
  if (fs::is_directory(iso, dec)) {
    fs::path src = iso / member;
    if (!fs::is_regular_file(src, dec)) {
      if (err) *err = member + " is not on this disc";
      return false;
    }
    fs::create_directories(out_dir, dec);
    // Named as 7z's `e` leaves it: the basename, without the directories it
    // sat in.
    dec.clear();
    fs::copy_file(src, out_dir / src.filename(), fs::copy_options::overwrite_existing, dec);
    if (dec) {
      if (err) *err = "cannot copy " + src.string() + ": " + dec.message();
      return false;
    }
    return true;
  }
  fs::path z = rt::which(e, "7z");
  if (z.empty()) {
    if (err) *err = "no 7z in this runtime";
    return false;
  }
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  // `e` extracts without directory structure; the member is named exactly as
  // the listing spells it.
  ProcResult r = rt::run(e, z, {"e", "-y", "-o" + out_dir.string(), iso.string(), member});
  if (!r.ok() && err) *err = r.out;
  return r.ok();
}

bool extract_subtree(const rt::Env& e, const fs::path& iso, const std::string& subdir,
                     const fs::path& out_dir, std::string* err) {
  std::error_code dec;
  if (fs::is_directory(iso, dec)) {
    fs::path from = subdir.empty() ? iso : iso / subdir;
    if (!fs::is_directory(from, dec)) {
      if (err) *err = "extracted, but " + subdir + " is not where the disc said it would be";
      return false;
    }
    // As with an image: a second attempt replaces the first rather than
    // landing on top of it.
    fs::remove_all(out_dir, dec);
    return copy_tree(from, out_dir, err);
  }
  fs::path z = rt::which(e, "7z");
  if (z.empty()) {
    if (err) *err = "no 7z in this runtime";
    return false;
  }
  std::error_code ec;
  fs::path staging = out_dir.string() + ".staging";
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);

  // `x` keeps the directory structure, which we then strip by moving the named
  // subtree up to become the game root.
  std::vector<std::string> args = {"x", "-y", "-o" + staging.string(), iso.string()};
  if (!subdir.empty()) args.push_back(subdir + "/*");
  ProcResult r = rt::run(e, z, args);
  if (!r.ok()) {
    if (err) *err = r.out;
    fs::remove_all(staging, ec);
    return false;
  }

  fs::path from = subdir.empty() ? staging : staging / subdir;
  if (!fs::exists(from, ec)) {
    if (err) *err = "extracted, but " + subdir + " is not where the disc said it would be";
    fs::remove_all(staging, ec);
    return false;
  }
  fs::remove_all(out_dir, ec);
  fs::rename(from, out_dir, ec);
  if (ec) {
    // Across filesystems rename fails; copy instead.
    fs::copy(from, out_dir, fs::copy_options::recursive, ec);
    if (ec) {
      if (err) *err = "cannot move the extracted tree into place: " + ec.message();
      fs::remove_all(staging, ec);
      return false;
    }
  }
  fs::remove_all(staging, ec);
  return true;
}

bool hash_member(const rt::Env& e, const fs::path& iso, const std::string& member, Hash* out,
                 uint64_t* size) {
  std::error_code dec;
  if (fs::is_directory(iso, dec)) {
    fs::path src = iso / member;
    if (!fs::is_regular_file(src, dec)) return false;
    if (out) *out = hash_file(src);
    if (size) *size = fs::file_size(src, dec);
    return true;
  }
  fs::path z = rt::which(e, "7z");
  if (z.empty()) return false;
  // -so writes the member to stdout, so a 573 MB anchor never touches disk.
  ProcResult r = rt::run(e, z, {"e", "-so", iso.string(), member});
  if (!r.ok()) return false;
  if (out) *out = hash_bytes(r.out.data(), r.out.size());
  if (size) *size = r.out.size();
  return true;
}

}  // namespace kg::iso
