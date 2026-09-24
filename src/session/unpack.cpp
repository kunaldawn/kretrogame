#include "unpack.h"

#include <cstdint>
#include <fstream>
#include <stdexcept>

#include "../pack/dwarfs.h"
#include "../util/hash.h"
#include "../util/paths.h"
#include "../util/proc.h"

namespace kg::session {
namespace fs = std::filesystem;

fs::path extraction_stamp_file(const fs::path& dir) {
  fs::path d = dir;
  if (!d.has_filename()) d = d.parent_path();
  return d.parent_path() / (d.filename().string() + ".stamp");
}

bool may_unpack_into(const fs::path& dir, const std::string& id) {
  std::error_code ec;
  const fs::file_status st = fs::symlink_status(dir, ec);
  // Nowhere, or an empty directory: anybody's to fill.
  if (!fs::exists(st)) return true;
  if (fs::is_directory(st) && fs::is_empty(dir, ec) && !ec) return true;
  // A tree an unpack made has its stamp beside it - a finished one's, or the
  // placeholder an unfinished one leaves. The cache a game is unpacked into
  // when nobody says where is kretro's whatever is in it: one made before
  // there were stamps has none.
  if (fs::exists(extraction_stamp_file(dir), ec)) return true;
  return !id.empty() && dir.lexically_normal() == game_extract_dir(id).lexically_normal();
}

void unpack_body(const Pack& pack, const fs::path& dir) {
  // Before anything is looked at, let alone removed. The directory may be one
  // a person named - --extract-to ~/Games puts the game at ~/Games/<id> - and
  // the id is the bundle author's choice: ~/Games/<id> may be somebody's own
  // copy of the same game, and replacing it whole is not what "unpack into"
  // says.
  if (!may_unpack_into(dir, pack.meta().id)) {
    throw std::runtime_error(dir.string() + " is already there, and kretro did not unpack it, so it is "
                             "left alone. Unpack somewhere else, or move that directory aside.");
  }
  fs::path tool = dwarfs_tool();
  if (tool.empty()) throw std::runtime_error("no DwarFS tool to unpack with");
  const Header& h = pack.header();
  if (!h.has_body()) throw std::runtime_error(pack.meta().id + " carries no game to unpack");
  std::error_code ec;
  const fs::path stamp_file = extraction_stamp_file(dir);
  // The stamp goes first, so an unpack interrupted halfway leaves a cache
  // that describes nothing and is redone rather than played. Written over
  // rather than removed: a stamp that matches no pack still says the tree
  // beside it is an unpack's, which the next unpack may replace.
  write_extraction_stamp(stamp_file, "unpacking");
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (ec) throw std::runtime_error("cannot create " + dir.string() + ": " + ec.message());

  // A pack that is a file of its own ends with its body, so the tool can be
  // pointed at the offset. One inside a player is followed by the next game
  // or the table of contents, which dwarfsextract would read as more image:
  // it has no counterpart to the mount's imagesize.
  fs::path image = pack.path();
  uint64_t offset = h.body_off;
  fs::path copied;
  std::error_code sz;
  const uint64_t whole = fs::file_size(pack.path(), sz);
  if (pack.base() != 0 || sz || h.body_off + h.body_len != whole) {
    copied = fs::path(stamp_file).replace_extension(".image");
    pack.extract_body(copied);
    image = copied;
    offset = 0;
  }
  ProcResult r = kg::run({tool.string(), "--tool=dwarfsextract", "-i", image.string(), "-O",
                          std::to_string(offset), "-o", dir.string()});
  if (!copied.empty()) fs::remove(copied, ec);
  if (!r.ok()) {
    fs::remove_all(dir, ec);
    throw std::runtime_error("could not unpack the game:\n" + r.out);
  }
  write_extraction_stamp(stamp_file, extraction_stamp(pack.meta(), h));
}

// Three facts, one line, and every one of them is a reason to unpack again: the
// layout says where the game sits inside the unpacked tree, the body hash says
// which bytes were unpacked, and the Merkle root says which install they are.
// An id says none of that, so an id alone is not enough for the cache.
std::string extraction_stamp(const Meta& m, const Header& h) {
  return (m.rooted() ? std::string("rooted") : std::string("flat")) + " " + to_hex(m.body.blake3) +
         " " + to_hex(h.blake3_root);
}

bool extraction_stamp_matches(const fs::path& stamp_file, const std::string& stamp) {
  std::ifstream f(stamp_file);
  if (!f) return false;
  std::string got;
  std::getline(f, got);
  return got == stamp;
}

void write_extraction_stamp(const fs::path& stamp_file, const std::string& stamp) {
  std::error_code ec;
  fs::create_directories(stamp_file.parent_path(), ec);
  std::ofstream f(stamp_file, std::ios::trunc);
  if (!f) throw std::runtime_error("cannot write the unpacked game's stamp");
  f << stamp << "\n";
}

}  // namespace kg::session
