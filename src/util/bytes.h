// Little-endian integers in byte strings, and rounding up to an alignment:
// the codecs a pack header, a table of contents and a trailer are written in.
// Every width is written and read least significant byte first, whatever the
// host's order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace kg {

// `v` rounded up to the next multiple of `a`, which must not be zero.
constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

namespace le {

inline void put_u16(std::string& s, uint16_t v) {
  s.push_back(static_cast<char>(v & 0xff));
  s.push_back(static_cast<char>(v >> 8));
}

inline void put_u32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

inline void put_u64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

// The readers do not check `off`: their callers have checked the length of
// what they read from first.
inline uint16_t get_u16(std::string_view s, size_t off) {
  return static_cast<uint16_t>(static_cast<uint8_t>(s[off])) |
         static_cast<uint16_t>(static_cast<uint8_t>(s[off + 1])) << 8;
}

inline uint32_t get_u32(std::string_view s, size_t off) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[off + i])) << (8 * i);
  return v;
}

inline uint64_t get_u64(std::string_view s, size_t off) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(s[off + i])) << (8 * i);
  return v;
}

}  // namespace le
}  // namespace kg
