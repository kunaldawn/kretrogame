// Progress and cancelling for the long file work: copying a player together,
// verifying one, hashing a range.
#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string_view>

namespace kg::bundle {

struct Progress {
  std::string_view stage;  // "copying" or "verifying"
  uint64_t done = 0;
  uint64_t total = 0;
};

struct Callbacks {
  // Called about once a megabyte. May be empty.
  std::function<void(const Progress&)> progress;
  // Asked as often; returning true stops the work with Cancelled. May be empty.
  std::function<bool()> cancelled;
};

class Cancelled : public std::runtime_error {
 public:
  Cancelled() : std::runtime_error("cancelled") {}
};

}  // namespace kg::bundle
