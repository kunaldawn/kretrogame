#include "draft.h"

#include <algorithm>

#include "../../install/game_id.h"
#include "../../util/safe_names.h"

namespace kg::bundle {

// ---- identity ---------------------------------------------------------------

std::string id_from_title(std::string_view title) {
  std::string s = install::slug(title);
  // Long enough for any title anybody means; short enough that the file name
  // it becomes half of still fits on every filesystem an author uploads from.
  constexpr size_t kMax = 64;
  if (s.size() > kMax) s.resize(kMax);
  while (!s.empty() && s.back() == '-') s.pop_back();
  if (!kg::id_is_safe(s)) return "bundle";
  return s;
}

bool version_is_safe(std::string_view v) { return v.size() <= 32 && kg::id_is_safe(v); }

// ---- the draft --------------------------------------------------------------

std::string DraftGame::display_name() const { return name.empty() ? id : name; }

bool Draft::acked(std::string_view check_id) const {
  return std::find(acknowledged.begin(), acknowledged.end(), check_id) != acknowledged.end();
}

}  // namespace kg::bundle
