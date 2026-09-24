// Fixture packs and a fixture player base, out of small files, for the tests
// that build players: test_bundle and test_player. Both write the same bytes,
// so a change to them is a change to both suites at once.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "bundle/build.h"
#include "pack/kgpack.h"
#include "pack/tree.h"
#include "files.h"

namespace kgtest {

// A game's metadata over a two-file tree under tmp/tree-<id>.
inline kg::Meta pack_meta(const std::filesystem::path& tmp, const std::string& id) {
  std::filesystem::path root = tmp / ("tree-" + id);
  write_file(root / "GAME.EXE", "MZ this is " + id);
  write_file(root / "data" / "level1.dat", std::string(3000, 'L') + id);
  kg::Meta m;
  m.id = id;
  m.name = "The game " + id;
  m.year = 1999;
  m.run.exe = "GAME.EXE";
  m.tree = kg::Tree::from_directory(root);
  return m;
}

// A capsule with a body, as install writes one, at tmp/shelf/<id>.kgpack. The
// body is not a DwarFS image; nothing here mounts it, and its hash is all that
// is checked.
inline std::filesystem::path make_pack(const std::filesystem::path& tmp, const std::string& id,
                                       size_t body_size = 20000) {
  std::filesystem::path body = tmp / (id + ".body");
  std::string b;
  for (size_t i = 0; i < body_size; ++i) b.push_back(static_cast<char>('a' + (i * 7 + id.size()) % 26));
  write_file(body, b);
  std::filesystem::path out = tmp / "shelf" / (id + ".kgpack");
  std::filesystem::create_directories(out.parent_path());
  kg::write_pack(out, pack_meta(tmp, id), kg::WriteOptions{kg::PackKind::Game, body, false});
  return out;
}

// A player base as the Makefile links one, but out of small files, at
// tmp/player-base.
inline std::filesystem::path make_base(const std::filesystem::path& tmp) {
  write_file(tmp / "in" / "bootstrap", std::string("\x7f"
                                                   "ELF") +
                                           std::string(2000, 'b'));
  write_file(tmp / "in" / "tools", std::string(6000, 't'));
  write_file(tmp / "in" / "runtime", std::string(9000, 'r'));
  write_file(tmp / "in" / "app", std::string(3000, 'a'));
  std::filesystem::path base = tmp / "player-base";
  kg::bundle::link_file(base, tmp / "in" / "bootstrap",
                        {{kg::bundle::Kind::Tools, tmp / "in" / "tools", ""},
                         {kg::bundle::Kind::Runtime, tmp / "in" / "runtime", ""},
                         {kg::bundle::Kind::App, tmp / "in" / "app", ""}});
  return base;
}

}  // namespace kgtest
