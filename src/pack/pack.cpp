#include "pack.h"

#include <zstd.h>

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace kg {
namespace fs = std::filesystem;

namespace {

std::string read_range(const fs::path& p, uint64_t off, uint64_t len) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open " + p.string());
  if (std::fseek(f, static_cast<long>(off), SEEK_SET) != 0) {
    std::fclose(f);
    throw std::runtime_error("cannot seek in " + p.string());
  }
  std::string out;
  out.resize(static_cast<size_t>(len));
  size_t got = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  if (got != out.size()) throw std::runtime_error("pack is truncated: " + p.string());
  return out;
}

// One megabyte, matching hash.cpp's block size for the same reason: it is large
// enough that the syscall cost disappears and small enough that it never shows
// up in a memory profile.
constexpr size_t kBodyChunk = 1 << 20;

// Hashes `len` bytes at `off` without holding them. The body of a capsule is
// the whole point of this: verifying a four-gigabyte pack must not need four
// gigabytes of memory.
Hash hash_range(const fs::path& p, uint64_t off, uint64_t len) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open " + p.string());
  if (std::fseek(f, static_cast<long>(off), SEEK_SET) != 0) {
    std::fclose(f);
    throw std::runtime_error("cannot seek in " + p.string());
  }
  Hasher h;
  std::vector<char> buf(kBodyChunk);
  uint64_t left = len;
  while (left > 0) {
    size_t want = buf.size();
    if (left < want) want = static_cast<size_t>(left);
    size_t n = std::fread(buf.data(), 1, want, f);
    if (n == 0) break;
    h.update(buf.data(), n);
    left -= n;
  }
  bool bad = std::ferror(f) != 0;
  std::fclose(f);
  if (bad) throw std::runtime_error("read error on " + p.string());
  if (left != 0) throw std::runtime_error("pack is truncated: " + p.string());
  return h.finish();
}

// Copies `len` bytes at `off` out to `out`, a chunk at a time.
void copy_range(const fs::path& p, uint64_t off, uint64_t len, const fs::path& out) {
  std::FILE* in = std::fopen(p.c_str(), "rb");
  if (!in) throw std::runtime_error("cannot open " + p.string());
  if (std::fseek(in, static_cast<long>(off), SEEK_SET) != 0) {
    std::fclose(in);
    throw std::runtime_error("cannot seek in " + p.string());
  }
  std::FILE* f = std::fopen(out.c_str(), "wb");
  if (!f) {
    std::fclose(in);
    throw std::runtime_error("cannot write " + out.string());
  }
  std::vector<char> buf(kBodyChunk);
  uint64_t left = len;
  bool ok = true;
  while (left > 0 && ok) {
    size_t want = buf.size();
    if (left < want) want = static_cast<size_t>(left);
    size_t n = std::fread(buf.data(), 1, want, in);
    if (n == 0) break;
    ok = std::fwrite(buf.data(), 1, n, f) == n;
    left -= n;
  }
  ok = ok && std::ferror(in) == 0;
  std::fclose(in);
  ok = std::fclose(f) == 0 && ok;
  if (!ok || left != 0) throw std::runtime_error("could not extract the body to " + out.string());
}

std::string zstd_decompress(std::string_view in, size_t limit) {
  unsigned long long want = ZSTD_getFrameContentSize(in.data(), in.size());
  if (want == ZSTD_CONTENTSIZE_ERROR) throw std::runtime_error("metadata is not zstd");
  if (want == ZSTD_CONTENTSIZE_UNKNOWN || want > limit) {
    throw std::runtime_error("metadata declares an implausible size");
  }
  std::string out;
  out.resize(static_cast<size_t>(want));
  size_t n = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
  if (ZSTD_isError(n)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(n));
  out.resize(n);
  return out;
}

}  // namespace

Pack Pack::open(const fs::path& p) {
  std::error_code ec;
  uint64_t file_size = fs::file_size(p, ec);
  if (ec) throw std::runtime_error("cannot open " + p.string() + ": " + ec.message());
  return open(p, 0, file_size);
}

Pack Pack::open(const fs::path& p, uint64_t off, uint64_t len) {
  Pack pk;
  pk.path_ = p;
  pk.base_ = off;

  std::error_code ec;
  uint64_t whole = fs::file_size(p, ec);
  if (ec) throw std::runtime_error("cannot open " + p.string() + ": " + ec.message());
  // The range is the caller's claim - for a pack inside a player, a number out
  // of that player's table of contents - so it is checked the way the header's
  // numbers are below, by subtraction, and cannot wrap its way into the file.
  if (off > whole || len > whole - off) {
    throw std::runtime_error("kgpack runs past the end of " + p.string());
  }
  if (len < kPackHeaderSize) throw std::runtime_error("not a kgpack: file is too short");
  std::string head = read_range(p, off, kPackHeaderSize);
  pk.header_ = Header::parse(head);

  // From here on the pack's own length stands in for the file's: a pack inside
  // a player must not be allowed to name bytes that belong to its neighbour.
  uint64_t file_size = len;
  // Subtraction rather than addition, because both halves are numbers a
  // stranger wrote and off + len is computed in 64 bits: a body_off of 4096
  // with a body_len of 2^64-4096 sums to zero, which is inside every file there
  // has ever been. Header::parse has bounded body_len, and this bounds the pair.
  if (pk.header_.meta_off > file_size ||
      pk.header_.meta_len > file_size - pk.header_.meta_off) {
    throw std::runtime_error("kgpack metadata runs past the end of the file");
  }
  if (pk.header_.has_body() &&
      (pk.header_.body_off > file_size ||
       pk.header_.body_len > file_size - pk.header_.body_off)) {
    throw std::runtime_error("kgpack body runs past the end of the file");
  }

  // Header::parse has already refused a meta_len over kMaxMetaLen, so this read
  // cannot be talked into committing whatever the file's size allows; the same
  // bound then holds the decompressed side against a bomb.
  std::string meta_z = read_range(p, off + pk.header_.meta_off, pk.header_.meta_len);
  std::string meta_cbor = zstd_decompress(meta_z, kMaxMetaLen);
  pk.set_ = SetMeta::decode(meta_cbor);
  return pk;
}

const Meta& Pack::game(std::string_view id) const {
  if (const Meta* m = set_.find(id)) return *m;
  throw std::runtime_error(path_.filename().string() + " does not carry " + std::string(id));
}

void Pack::extract_body(const fs::path& out) const {
  if (!has_body()) throw std::runtime_error("this kgpack has no body to extract");
  copy_range(path_, base_ + header_.body_off, header_.body_len, out);
}

Pack::Verification Pack::verify() const {
  Verification v;
  v.root_matches = set_.root() == header_.blake3_root;
  if (!v.root_matches) {
    v.detail = "tree Merkle root does not match the header";
  }

  if (has_body()) {
    Hash actual = hash_range(path_, base_ + header_.body_off, header_.body_len);
    v.body_matches = actual == set_.body.blake3 && header_.body_len == set_.body.length;
    if (!v.body_matches) {
      if (!v.detail.empty()) v.detail += "; ";
      v.detail += "body hash does not match the metadata";
    }
  } else {
    v.body_matches = true;
  }

  v.ok = v.root_matches && v.body_matches;
  if (v.ok) v.detail = "ok";
  return v;
}

}  // namespace kg
