// BLAKE3 hashing: the one hash function kretrogame uses, for file content,
// tree manifests, disc fingerprints and pack bodies alike.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace kg {

using Hash = std::array<uint8_t, 32>;

// A hash of nothing; distinct from any real content hash.
Hash hash_zero();

Hash hash_bytes(const void* data, size_t len);
Hash hash_string(std::string_view s);

// Streams the file in fixed-size blocks, so hashing a 3.6 GB ISO costs a
// constant amount of memory. Throws std::runtime_error if it cannot be read.
Hash hash_file(const std::filesystem::path& p);

// Hashes at most `limit` bytes from the start of the file. Used for cheap
// pre-screening before committing to a whole-image hash.
Hash hash_file_prefix(const std::filesystem::path& p, uint64_t limit);

std::string to_hex(const Hash& h);

// Throws std::runtime_error unless `s` is exactly 64 hex digits.
Hash from_hex(std::string_view s);

// Incremental hashing, for callers that assemble input from several pieces.
class Hasher {
 public:
  Hasher();
  ~Hasher();
  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;

  void update(const void* data, size_t len);
  void update(std::string_view s);
  Hash finish() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace kg
