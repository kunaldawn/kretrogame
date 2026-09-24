#include "pack.h"

#include <unistd.h>
#include <zstd.h>

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "../util/bytes.h"

namespace kg {
namespace fs = std::filesystem;

namespace {

constexpr int kMetaCompressionLevel = 19;  // metadata is small; spend the time

// The same one-megabyte copy buffer as pack.cpp's readers, for the same
// reason: the body goes through it and is never held whole.
constexpr size_t kBodyChunk = 1 << 20;

std::string zstd_compress(std::string_view in) {
  size_t bound = ZSTD_compressBound(in.size());
  std::string out;
  out.resize(bound);
  size_t n = ZSTD_compress(out.data(), bound, in.data(), in.size(), kMetaCompressionLevel);
  if (ZSTD_isError(n)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(n));
  out.resize(n);
  return out;
}

}  // namespace

void write_pack(const fs::path& out, Meta meta, const WriteOptions& opt) {
  // Two passes over the body, and never a copy of it in memory.
  //
  // Pass one only measures: the hash and the length go into the metadata, and
  // the metadata is written before the body, so both have to be known first.
  // Pass two copies the file through a one-megabyte buffer. Peak occupancy is
  // the compressed metadata plus that buffer, whether the body is a 40 MB
  // game tree or a 4 GB capsule carrying three discs.
  std::error_code ec;
  if (opt.body) {
    uint64_t len = fs::file_size(*opt.body, ec);
    if (ec) throw std::runtime_error("cannot size " + opt.body->string() + ": " + ec.message());
    meta.body.length = len;
    meta.body.blake3 = hash_file(*opt.body);
  } else {
    meta.body = Meta::Body{};
  }

  std::string meta_cbor = meta.encode();
  std::string meta_z = zstd_compress(meta_cbor);

  Header h;
  h.kind = opt.kind;
  h.flags = 0;
  h.meta_off = kPackHeaderSize;
  h.meta_len = meta_z.size();
  if (opt.body) {
    h.flags |= kHasBody;
    if (opt.body_is_squashfs) h.flags |= kBodyIsSquashfs;
    h.body_off = align_up(h.meta_off + h.meta_len, kBodyAlign);
    h.body_len = meta.body.length;
  }
  h.blake3_root = meta.tree.root();

  // The destination is usually a capsule this machine is already playing, and
  // fopen(out, "wb") empties it before the first byte of the replacement is
  // written: a reinstall that failed on the third disc, or a recipe import that
  // failed its rebuild, would take the working game with it. Streaming made the
  // window the whole multi-gigabyte body copy rather than one fwrite. So the
  // pack is written beside its destination and renamed over it only once every
  // byte is down. The temporary is a sibling, so the rename is within one
  // filesystem and therefore atomic, and no failure below leaves it behind.
  fs::path tmp_out = out;
  tmp_out += ".partial-" + std::to_string(static_cast<long>(::getpid()));

  std::FILE* f = std::fopen(tmp_out.c_str(), "wb");
  if (!f) throw std::runtime_error("cannot write " + tmp_out.string());
  auto fail = [&](const std::string& msg) {
    if (f) std::fclose(f);
    f = nullptr;
    std::error_code rm;
    fs::remove(tmp_out, rm);
    return std::runtime_error(msg);
  };
  auto put = [&](std::string_view s) {
    if (std::fwrite(s.data(), 1, s.size(), f) != s.size()) {
      throw fail("short write to " + tmp_out.string());
    }
  };
  put(h.serialize());
  put(meta_z);
  if (opt.body) {
    std::string pad(static_cast<size_t>(h.body_off - (h.meta_off + h.meta_len)), '\0');
    put(pad);

    std::FILE* in = std::fopen(opt.body->c_str(), "rb");
    if (!in) throw fail("cannot open " + opt.body->string());
    std::vector<char> buf(kBodyChunk);
    uint64_t copied = 0;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
      if (std::fwrite(buf.data(), 1, n, f) != n) {
        std::fclose(in);
        throw fail("short write to " + tmp_out.string());
      }
      copied += n;
    }
    bool bad = std::ferror(in) != 0;
    std::fclose(in);
    if (bad) throw fail("read error on " + opt.body->string());
    // The header now declares a length that pass one measured. If the file
    // changed underneath us, the pack would claim bytes it does not have and
    // every later reader would blame the disk.
    if (copied != meta.body.length) {
      throw fail("the body changed size while the pack was being written");
    }
  }
  if (std::fclose(f) != 0) {
    f = nullptr;
    throw fail("cannot close " + tmp_out.string());
  }
  f = nullptr;
  fs::rename(tmp_out, out, ec);
  if (ec) throw fail("cannot put the finished pack at " + out.string() + ": " + ec.message());
}

}  // namespace kg
