// Small file helpers the unit tests share: write a fixture, read one back,
// look for text, damage one byte of a file, and show bytes as hex.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace kgtest {

// Writes `content` to `p`, creating its directories and replacing what was there.
inline void write_file(const std::filesystem::path& p, std::string_view content) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// The whole of `p`, or "" when it cannot be read.
inline std::string slurp(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

inline bool contains(std::string_view s, std::string_view what) { return s.find(what) != std::string_view::npos; }

// Bytes as lowercase hex, two digits a byte: the form the golden-byte checks
// compare an encoder's output in, so a mismatch prints as something readable.
inline std::string to_hex(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (char c : bytes) {
    const auto b = static_cast<unsigned char>(c);
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0x0f]);
  }
  return out;
}

// Damages the byte at `off` in place, so that it is certain to differ.
inline void flip_byte(const std::filesystem::path& p, uint64_t off) {
  std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
  f.seekg(static_cast<std::streamoff>(off));
  char c = 0;
  f.read(&c, 1);
  c = static_cast<char>(c ^ 0x5a);
  f.seekp(static_cast<std::streamoff>(off));
  f.write(&c, 1);
}

}  // namespace kgtest
