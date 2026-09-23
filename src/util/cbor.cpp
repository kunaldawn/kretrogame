#include "cbor.h"

#include <algorithm>
#include <stdexcept>

namespace kg::cbor {
namespace {

// How many elements a container is allowed to book space for up front. A
// declared length is the writer's claim and nothing has been read to back it
// yet, so reserving on it is doing the attacker's allocation for him;
// sizeof(Value) is 112 bytes, which turns a count into a hundredfold. Past this
// the vector grows as the elements actually arrive, which costs a few
// reallocations on a manifest that has thousands of anything - none does.
constexpr size_t kReserveCap = 1024;

[[noreturn]] void bad(const char* why) {
  throw std::runtime_error(std::string("malformed CBOR: ") + why);
}

struct Reader {
  std::string_view in;
  size_t pos = 0;
  const Limits& lim;
  size_t items = 0;

  uint8_t byte() {
    if (pos >= in.size()) bad("truncated");
    return static_cast<uint8_t>(in[pos++]);
  }

  std::string_view take(uint64_t n) {
    if (n > in.size() - pos) bad("length runs past end of buffer");
    std::string_view s = in.substr(pos, static_cast<size_t>(n));
    pos += static_cast<size_t>(n);
    return s;
  }

  uint64_t argument(uint8_t info) {
    if (info < 24) return info;
    switch (info) {
      case 24: return byte();
      case 25: {
        uint64_t v = byte();
        return (v << 8) | byte();
      }
      case 26: {
        uint64_t v = 0;
        for (int k = 0; k < 4; ++k) v = (v << 8) | byte();
        return v;
      }
      case 27: {
        uint64_t v = 0;
        for (int k = 0; k < 8; ++k) v = (v << 8) | byte();
        return v;
      }
      default: bad("indefinite lengths and reserved additional info are not accepted");
    }
  }

  Value value(size_t depth) {
    if (depth > lim.max_depth) bad("nested too deeply");
    if (++items > lim.max_items) bad("too many items");

    uint8_t ib = byte();
    uint8_t major = ib >> 5;
    uint8_t info = ib & 0x1f;
    Value v;

    switch (major) {
      case 0:
        v.type = Value::Type::Uint;
        v.u = argument(info);
        return v;
      case 1: {
        uint64_t a = argument(info);
        if (a > static_cast<uint64_t>(INT64_MAX)) bad("negative integer out of range");
        v.type = Value::Type::Int;
        v.i = -1 - static_cast<int64_t>(a);
        return v;
      }
      case 2: {
        uint64_t n = argument(info);
        v.type = Value::Type::Bytes;
        v.s = std::string(take(n));
        return v;
      }
      case 3: {
        uint64_t n = argument(info);
        v.type = Value::Type::Text;
        v.s = std::string(take(n));
        return v;
      }
      case 4: {
        uint64_t n = argument(info);
        // Each element costs at least one byte, so a length that exceeds the
        // remaining buffer is a lie and we reject it before allocating.
        if (n > in.size() - pos) bad("array longer than the buffer allows");
        // And the item budget is spent before the space is. It used to be
        // checked one element at a time, after the reserve, so a few kilobytes
        // of pack declaring an array of forty million asked for four gigabytes
        // and got it - the budget refused the pack a moment later, out of
        // memory it had already committed.
        if (n > lim.max_items - items) bad("too many items");
        v.type = Value::Type::Array;
        v.arr.reserve(static_cast<size_t>(std::min<uint64_t>(n, kReserveCap)));
        for (uint64_t k = 0; k < n; ++k) v.arr.push_back(value(depth + 1));
        return v;
      }
      case 5: {
        uint64_t n = argument(info);
        if (n > (in.size() - pos) / 2 + 1) bad("map longer than the buffer allows");
        // A pair is two items, and a pair is 224 bytes here.
        if (n > (lim.max_items - items) / 2) bad("too many items");
        v.type = Value::Type::Map;
        v.map.reserve(static_cast<size_t>(std::min<uint64_t>(n, kReserveCap)));
        for (uint64_t k = 0; k < n; ++k) {
          Value key = value(depth + 1);
          Value val = value(depth + 1);
          v.map.emplace_back(std::move(key), std::move(val));
        }
        return v;
      }
      case 7:
        switch (info) {
          case 20: v.type = Value::Type::Bool; v.b = false; return v;
          case 21: v.type = Value::Type::Bool; v.b = true; return v;
          case 22: v.type = Value::Type::Null; return v;
          default: bad("unsupported simple value");
        }
      default:
        bad("unsupported major type");
    }
  }
};

}  // namespace

void Encoder::head(uint8_t major, uint64_t v) {
  uint8_t m = static_cast<uint8_t>(major << 5);
  if (v < 24) {
    buf_.push_back(static_cast<char>(m | v));
  } else if (v <= 0xff) {
    buf_.push_back(static_cast<char>(m | 24));
    buf_.push_back(static_cast<char>(v));
  } else if (v <= 0xffff) {
    buf_.push_back(static_cast<char>(m | 25));
    for (int k = 1; k >= 0; --k) buf_.push_back(static_cast<char>(v >> (8 * k)));
  } else if (v <= 0xffffffffu) {
    buf_.push_back(static_cast<char>(m | 26));
    for (int k = 3; k >= 0; --k) buf_.push_back(static_cast<char>(v >> (8 * k)));
  } else {
    buf_.push_back(static_cast<char>(m | 27));
    for (int k = 7; k >= 0; --k) buf_.push_back(static_cast<char>(v >> (8 * k)));
  }
}

void Encoder::uint_val(uint64_t v) { head(0, v); }

void Encoder::int_val(int64_t v) {
  if (v < 0) head(1, static_cast<uint64_t>(-1 - v));
  else head(0, static_cast<uint64_t>(v));
}

void Encoder::bytes(const void* p, size_t n) {
  head(2, n);
  buf_.append(static_cast<const char*>(p), n);
}

void Encoder::text(std::string_view s) {
  head(3, s.size());
  buf_.append(s);
}

void Encoder::array(size_t n) { head(4, n); }
void Encoder::map(size_t n) { head(5, n); }
void Encoder::boolean(bool b) { buf_.push_back(static_cast<char>(0xe0 | (b ? 21 : 20))); }
void Encoder::null_val() { buf_.push_back(static_cast<char>(0xe0 | 22)); }

const Value* Value::find(std::string_view key) const {
  if (type != Type::Map) return nullptr;
  for (const auto& kv : map) {
    if (kv.first.type == Type::Text && kv.first.s == key) return &kv.second;
  }
  return nullptr;
}

std::string Value::text_or(std::string_view def) const {
  return type == Type::Text ? s : std::string(def);
}

uint64_t Value::uint_or(uint64_t def) const { return type == Type::Uint ? u : def; }

bool Value::bool_or(bool def) const { return type == Type::Bool ? b : def; }

Value decode(std::string_view in, const Limits& lim) {
  Reader r{in, 0, lim, 0};
  Value v = r.value(0);
  if (r.pos != in.size()) bad("trailing bytes after the top-level value");
  return v;
}

}  // namespace kg::cbor
