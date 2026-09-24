// Small string helpers shared across the tree, so no file keeps its own copy.
// Case folding is std::tolower and std::toupper per byte: locale-based rather than an ASCII table, with each byte passed as an
// unsigned char so that bytes above 0x7f are not undefined behaviour.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace kg {

std::string to_lower(std::string_view s);
std::string to_upper(std::string_view s);

// Without leading and trailing spaces, tabs, carriage returns and newlines.
std::string trim(std::string_view s);

// Equal ignoring case, byte for byte, as to_lower would fold them.
bool iequals(std::string_view a, std::string_view b);

// The strings with `sep` between each pair and none at either end.
std::string join(const std::vector<std::string>& v, std::string_view sep);

}  // namespace kg
