// The player's own file: its table of contents, its bundle.meta, and the
// packs the table points at.
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

#include "../bundle/meta.h"
#include "../bundle/toc.h"

namespace kg::player {

// A pack's bytes are not the bytes the table says they are.
class Damaged : public std::runtime_error {
 public:
  Damaged(const std::string& game, const std::string& what)
      : std::runtime_error(what), game_(game) {}
  const std::string& game() const { return game_; }

 private:
  std::string game_;
};

struct Bundle {
  std::filesystem::path self;
  bundle::Toc toc;
  bundle::BundleMeta meta;

  // Reads `self`'s table and its bundle.meta. `toc_env` is KRETRO_TOC as the
  // bootstrap set it ("off:len"); when given, it must agree with the table
  // read here, or the file changed under us. Throws with a message a person
  // can act on.
  static Bundle open(const std::filesystem::path& self, const std::string& toc_env = "");

  const bundle::GameMeta* game(const std::string& id) const;
  const bundle::Entry* pack(const std::string& id) const;
  // "example-game, other-game" for a message about a game that is not here.
  std::string game_list() const;
};

}  // namespace kg::player
