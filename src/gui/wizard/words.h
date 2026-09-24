// The wizard's small pieces of wording, and the one helper every step's text
// fields share. Internal to the wizard: nothing outside src/gui/wizard/ names
// this namespace.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "../../install/draft.h"
#include "../../install/source.h"

namespace kg::gui::wizard_detail {

// "three discs" rather than "3 discs".
std::string discs_in_words(size_t n);
// What step 1 calls a source of each kind.
const char* kind_word(install::Source::Kind k);
// The sentence step 3 offers each install method as.
const char* method_word(install::Draft::Method m);
// A candidate directory, said out loud.
std::string where_word(const std::filesystem::path& dir);
// A text field's fixed buffer, set to `v` and cut to fit.
void set_buf(char* buf, size_t n, const std::string& v);

}  // namespace kg::gui::wizard_detail
