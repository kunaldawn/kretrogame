// Keys in a pack's registry fragment: reading the fragment back, finding a key
// left in it, and suggesting where an embedded key would go.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../../wine/registry.h"

namespace kg::bundle {

// ---- keys -------------------------------------------------------------------------

// A value in the fragment that is a key: serial-shaped by registry.cpp's rule,
// or exactly the key the vault holds for this game. Install keeps keys out of
// packs; a pack made elsewhere, or before that rule, may still carry one.
std::optional<wine::RegValue> key_in_fragment(std::string_view regedit4, const std::string& vault_key);

// Where an embedded key would be written. The key's own old place when the
// fragment still shows it; otherwise the game's own key under Software - the
// one its installer wrote most values to - with the value name left for the
// author, because no pack records what the key it held back was called.
struct KeySpot {
  std::string path;   // with the hive: HKEY_LOCAL_MACHINE\Software\...
  std::string value;
};
KeySpot suggest_key_spot(std::string_view regedit4, const std::string& vault_key);

}  // namespace kg::bundle
