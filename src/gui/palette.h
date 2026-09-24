// The few text colours the pages share, so a warning on the shelf is the same
// colour as one on the Bundles page or in a player's launcher.
#pragma once

#include "imgui.h"

namespace kg::gui {

// Something is wrong, or about to be: a refusal, a failed step, a risk.
inline constexpr ImVec4 kWarn{1.0f, 0.6f, 0.4f, 1.0f};
// Worth noticing, not wrong: where a game was left, a note.
inline constexpr ImVec4 kWarm{1.0f, 0.86f, 0.55f, 1.0f};
// The Bundles page's all-clear.
inline constexpr ImVec4 kGood{0.55f, 0.85f, 0.55f, 1.0f};
// The doctor's "no problems found". A different green from kGood, kept as it
// always was.
inline constexpr ImVec4 kHealthy{0.5f, 0.9f, 0.5f, 1.0f};

}  // namespace kg::gui
