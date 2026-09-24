// Reading the environment, in the two ways the code asks.
#pragma once

#include <cstdlib>
#include <string>

namespace kg {

// The variable's value, or nullptr when it is unset or set to "". Most of
// kretro's variables name a path, and an empty path means nothing.
inline const char* env_nonempty(const char* name) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : nullptr;
}

// The variable's value, or "" when it is unset.
inline std::string env_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

}  // namespace kg
