#include "key_fragment.h"

#include <map>

#include "../../util/text.h"

namespace kg::bundle {

// ---- keys -----------------------------------------------------------------------------

std::optional<wine::RegValue> key_in_fragment(std::string_view regedit4, const std::string& vault_key) {
  for (const wine::RegValue& v : wine::parse_fragment(regedit4)) {
    if (wine::is_serial_value(v)) return v;
    if (!vault_key.empty() && v.type == "sz" && v.data == vault_key) return v;
  }
  return std::nullopt;
}

KeySpot suggest_key_spot(std::string_view regedit4, const std::string& vault_key) {
  if (auto k = key_in_fragment(regedit4, vault_key)) return {k->hive + "\\" + k->key, k->name};
  // The game's own key: the one under Software its installer wrote most to,
  // leaving out what Wine and Windows write for every program.
  std::map<std::string, int> count;
  std::string best;
  int best_n = 0;
  for (const wine::RegValue& v : wine::parse_fragment(regedit4)) {
    std::string k = to_lower(v.key);
    if (k.rfind("software\\", 0) != 0) continue;
    if (k.rfind("software\\microsoft", 0) == 0 || k.rfind("software\\wine", 0) == 0 ||
        k.rfind("software\\classes", 0) == 0) {
      continue;
    }
    std::string full = v.hive + "\\" + v.key;
    int n = ++count[full];
    // More values wins; on a tie the machine-wide hive, which is where the
    // installers of this era put a key.
    bool hklm = v.hive == wine::kHiveLocalMachine;
    bool best_hklm = best.rfind(std::string(wine::kHiveLocalMachine), 0) == 0;
    if (n > best_n || (n == best_n && hklm && !best_hklm)) {
      best = full;
      best_n = n;
    }
  }
  return {best, ""};
}

}  // namespace kg::bundle
