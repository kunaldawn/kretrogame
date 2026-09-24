// The stage's proof, with no GUI: what `kretro stage-probe` and
// tests/integration/stage.sh run.
#pragma once

#include "../../rt/env.h"

namespace kg::gui {

// Proves the whole arrangement with no GUI: runs winecfg on a headless Weston,
// reads the Xwayland root window after `seconds`, and says how much of it is
// not black. Returns 0 when it captured a picture, 77 when this runtime has no
// compositor to try it with, and 1 when headless Weston came up but its root
// window could not be read - which is the finding this milestone exists to
// make. Used by tests/integration/stage.sh and by `kretro stage-probe`.
int stage_probe(const rt::Env& e, int seconds);

}  // namespace kg::gui
