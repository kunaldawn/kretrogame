// A game's id: what it is derived from, and what it must not collide with.
#pragma once

#include <string>
#include <string_view>

#include "../rt/env.h"

namespace kg::install {

// "Example Game: The Sequel" -> "example-game-the-sequel".
std::string slug(std::string_view name);

// An id is four things, not one: state/games/<id>.kgpack, state/saves/<id>,
// state/prefixes/<id>, and <id>.toml among the local manifests. Checking
// only the pack is what lets a new game silently inherit an old game's prefix
// and, worse, its saves.
struct IdClash {
  bool pack = false;
  bool saves = false;
  bool prefix = false;
  bool manifest = false;
  bool any() const { return pack || saves || prefix || manifest; }
  // "a pack and its saves", for a sentence the user reads rather than a
  // checklist they have to decode.
  std::string sentence() const;
};
IdClash id_clash(const rt::Env& e, const std::string& id);

// The next id in the same family that clashes with nothing: "example-game-2",
// then "-3". Offered to the wizard's user as an edit to the field. Never
// applied for them - rebuilding the game you already have is a reasonable
// thing to want, and routing around it silently would take the choice away.
std::string next_free_id(const rt::Env& e, const std::string& id);

}  // namespace kg::install
