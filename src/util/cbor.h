// A deliberately small CBOR codec (RFC 8949), enough for kgpack metadata and
// no more. Packs are shared between people, so the decoder treats its input as
// hostile: every length is bounded, recursion is capped, and a truncated or
// malformed pack raises rather than reading past its buffer.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace kg::cbor {

class Encoder {
 public:
  void uint_val(uint64_t v);
  void int_val(int64_t v);
  void bytes(const void* p, size_t n);
  void text(std::string_view s);
  void boolean(bool b);
  void null_val();
  // Header for a definite-length container; the caller then encodes n items
  // (arrays) or 2n items (maps, key then value).
  void array(size_t n);
  void map(size_t n);

  const std::string& data() const { return buf_; }
  std::string take() { return std::move(buf_); }

 private:
  void head(uint8_t major, uint64_t v);
  std::string buf_;
};

struct Value {
  enum class Type { Uint, Int, Bytes, Text, Array, Map, Bool, Null };

  Type type = Type::Null;
  uint64_t u = 0;
  int64_t i = 0;
  std::string s;  // Bytes and Text both
  std::vector<Value> arr;
  std::vector<std::pair<Value, Value>> map;
  bool b = false;

  bool is_uint() const { return type == Type::Uint; }
  bool is_text() const { return type == Type::Text; }
  bool is_bytes() const { return type == Type::Bytes; }
  bool is_array() const { return type == Type::Array; }
  bool is_map() const { return type == Type::Map; }

  // Map lookup by text key. Returns nullptr when absent, so callers can treat
  // a missing optional section as a default rather than an error.
  const Value* find(std::string_view key) const;

  // Typed accessors that fall back rather than throw, because a pack written
  // by a newer kretrogame must still open in an older one.
  std::string text_or(std::string_view def = "") const;
  uint64_t uint_or(uint64_t def = 0) const;
  bool bool_or(bool def = false) const;
};

struct Limits {
  size_t max_depth = 32;
  size_t max_items = 1u << 20;  // total across the whole document
};

// Decodes exactly one value. Throws std::runtime_error on malformed input,
// on trailing garbage, or when a limit is exceeded.
Value decode(std::string_view in, const Limits& lim = {});

}  // namespace kg::cbor
