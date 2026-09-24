// The gamepad map's text form, shared by the Bundles page, the builder and the
// player.
#pragma once

#include <map>
#include <string>
#include <string_view>

namespace kg::bundle {

// The gamepad map's one written form: "button=keysym", one a line, in button
// order. It is what the Bundles page shows and edits, what the builder makes
// from a pack's own [input], and what the player's helper reads back, so the
// three cannot drift apart. The button names are the helper's own (a, b, x,
// y, up, down, left, right, start, back, l, r) and the keys are X keysym
// names, as a manifest writes them.
std::string format_gamepad(const std::map<std::string, std::string>& binds);

// Reads that form back. Lenient about what a person typed on the page: a ','
// or ';' also ends an entry, blanks around either side go, and anything that
// is not button=key is skipped - a wrong entry costs that one button, not the
// gamepad. A button named twice keeps its last key.
std::map<std::string, std::string> parse_gamepad(std::string_view text);

}  // namespace kg::bundle
