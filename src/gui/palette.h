// The colours kretro and a player's launcher are drawn in: a phosphor-green
// terminal, dark and quiet, with amber for whatever has the focus and cyan
// for keys. The one place a colour is named, so a warning on the shelf is the
// same colour as one on the Bundles page or in a player's launcher, and the
// ImGui style in window.cpp is built from the same tokens.
#pragma once

#include <cstdint>

#include "imgui.h"

namespace kg::gui {

// A colour written the way a designer writes it, 0xRRGGBB, with an alpha.
constexpr ImVec4 rgb(uint32_t hex, float a = 1.0f) {
  return ImVec4(static_cast<float>((hex >> 16) & 0xff) / 255.0f, static_cast<float>((hex >> 8) & 0xff) / 255.0f,
                static_cast<float>(hex & 0xff) / 255.0f, a);
}

// The same colour with its alpha multiplied, for a tint of a token.
constexpr ImVec4 alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, c.w * a); }

// A token as a draw list wants it.
inline ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImU32 u32(const ImVec4& c, float a) { return ImGui::ColorConvertFloat4ToU32(alpha(c, a)); }

// ---- surfaces ---------------------------------------------------------------

// The window itself, and the clear colour behind it.
inline constexpr ImVec4 kBg0 = rgb(0x0A0D0B);
// Panels, popups and bordered children: one step up from the window.
inline constexpr ImVec4 kBg1 = rgb(0x0F1411);
// Frames: text fields, buttons at rest, table headers.
inline constexpr ImVec4 kBg2 = rgb(0x151D18);
// A frame under the pointer.
inline constexpr ImVec4 kBg3 = rgb(0x1C2821);
// A frame being pressed or typed in.
inline constexpr ImVec4 kBgActive = rgb(0x22352A);
// A button under the pointer: a frame with the accent's green in it.
inline constexpr ImVec4 kButtonHover = rgb(0x1E3A29);
// Rules and borders.
inline constexpr ImVec4 kLine = rgb(0x22302A);
// The border of a frame - a button, a field - a little greener than a rule so
// the controls stand out from the layout around them.
inline constexpr ImVec4 kFrameLine = rgb(0x2B4A38);

// ---- text -------------------------------------------------------------------

inline constexpr ImVec4 kText = rgb(0xD5E2D8);
// Hints, captions, anything secondary.
inline constexpr ImVec4 kDim = rgb(0x6C8274);

// ---- accents ----------------------------------------------------------------

// Phosphor green: the prompt, checkmarks, what is active.
inline constexpr ImVec4 kAccent = rgb(0x5CF08A);
// The same green at rest: the blinking cursor, selection, a pressed button.
inline constexpr ImVec4 kAccentDim = rgb(0x2B7A47);
// The focus: the tile or widget a keyboard or pad is on.
inline constexpr ImVec4 kAmber = rgb(0xFFB547);
// Keys and information: the keycaps in the status bar, links.
inline constexpr ImVec4 kCyan = rgb(0x5CC8F0);
// Failed outright, as opposed to kWarn's "about to be".
inline constexpr ImVec4 kError = rgb(0xFF5566);

// ---- the text colours the pages have always had -----------------------------

// Something is wrong, or about to be: a refusal, a failed step, a risk.
inline constexpr ImVec4 kWarn = rgb(0xFF7A59);
// Worth noticing, not wrong: where a game was left, a note.
inline constexpr ImVec4 kWarm = rgb(0xF0D27A);
// The Bundles page's all-clear.
inline constexpr ImVec4 kGood = rgb(0x7BE39A);
// ---- the Steam-like pieces, all derived: no new hues ------------------------

// The focus glow round whatever the keyboard or the pad is on.
inline constexpr ImVec4 kGlow = kAmber;
// The window's colour as a scrim over art: gradients that sink a picture into
// the page run from it at no alpha to it at nearly full.
inline constexpr ImVec4 kScrim = kBg0;
// The play block: the accent as a fill, the window's colour as its text, and
// the fill a touch lighter under the pointer.
inline constexpr ImVec4 kPlayText = kBg0;
inline constexpr ImVec4 kPlayHover = rgb(0x70F298);
// The install block, in the information colour: a step towards playing, not
// playing yet.
inline constexpr ImVec4 kInstall = kCyan;
// A surface raised over a panel: the current row of a sidebar, a dialog's
// footer band.
inline constexpr ImVec4 kPanelRaised = kBg2;

// The doctor's "no problems found". A different green from kGood, as it
// always was: the full phosphor.
inline constexpr ImVec4 kHealthy = kAccent;

}  // namespace kg::gui
