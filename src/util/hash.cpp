#include "hash.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

extern "C" {
#include "blake3.h"
}

namespace kg {
namespace {

constexpr size_t kBlock = 1 << 20;  // 1 MiB; keeps ISO hashing memory-flat.

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

Hash hash_stream(const std::filesystem::path& p, uint64_t limit) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open " + p.string());

  blake3_hasher h;
  blake3_hasher_init(&h);
  std::vector<unsigned char> buf(kBlock);
  uint64_t read_total = 0;
  while (limit == 0 || read_total < limit) {
    size_t want = buf.size();
    if (limit != 0 && limit - read_total < want) want = limit - read_total;
    size_t n = std::fread(buf.data(), 1, want, f);
    if (n == 0) break;
    blake3_hasher_update(&h, buf.data(), n);
    read_total += n;
  }
  bool bad = std::ferror(f) != 0;
  std::fclose(f);
  if (bad) throw std::runtime_error("read error on " + p.string());

  Hash out{};
  blake3_hasher_finalize(&h, out.data(), out.size());
  return out;
}

}  // namespace

struct Hasher::Impl {
  blake3_hasher h;
};

Hasher::Hasher() : impl_(new Impl) { blake3_hasher_init(&impl_->h); }
Hasher::~Hasher() { delete impl_; }

void Hasher::update(const void* data, size_t len) {
  blake3_hasher_update(&impl_->h, data, len);
}

void Hasher::update(std::string_view s) { update(s.data(), s.size()); }

Hash Hasher::finish() const {
  // finalize does not consume the state, but it is not const in the C API.
  blake3_hasher copy = impl_->h;
  Hash out{};
  blake3_hasher_finalize(&copy, out.data(), out.size());
  return out;
}

Hash hash_zero() { return Hash{}; }

Hash hash_bytes(const void* data, size_t len) {
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, data, len);
  Hash out{};
  blake3_hasher_finalize(&h, out.data(), out.size());
  return out;
}

Hash hash_string(std::string_view s) { return hash_bytes(s.data(), s.size()); }

Hash hash_file(const std::filesystem::path& p) { return hash_stream(p, 0); }

Hash hash_file_prefix(const std::filesystem::path& p, uint64_t limit) {
  return hash_stream(p, limit);
}

std::string to_hex(const Hash& h) {
  static const char* kHex = "0123456789abcdef";
  std::string s(64, '0');
  for (size_t i = 0; i < h.size(); ++i) {
    s[i * 2] = kHex[h[i] >> 4];
    s[i * 2 + 1] = kHex[h[i] & 0x0f];
  }
  return s;
}

Hash from_hex(std::string_view s) {
  if (s.size() != 64) throw std::runtime_error("hash must be 64 hex digits");
  Hash h{};
  for (size_t i = 0; i < 32; ++i) {
    int hi = hex_val(s[i * 2]);
    int lo = hex_val(s[i * 2 + 1]);
    if (hi < 0 || lo < 0) throw std::runtime_error("hash has a non-hex digit");
    h[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return h;
}

}  // namespace kg
