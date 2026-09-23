#include "build.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include "../pack/kgpack.h"

namespace kg::bundle {
namespace fs = std::filesystem;

namespace {

// A megabyte at a time, as the pack code does: large enough that the syscalls
// vanish, small enough that copying a four-gigabyte pack costs no memory, and
// often enough that a cancel is answered at once.
constexpr size_t kChunk = 1 << 20;

// bundle.meta carries PNGs and a few author-supplied DLLs. 64 MiB is past any
// honest one, and a table entry claiming more is not read into memory whole.
constexpr uint64_t kMaxMetaBytes = 64ull << 20;

[[noreturn]] void fail(const std::string& why) { throw std::runtime_error(why); }

std::string sys(const std::string& what, const fs::path& p) {
  return what + " " + p.string() + ": " + std::strerror(errno);
}

class Fd {
 public:
  Fd(const fs::path& p, int flags, mode_t mode = 0) : fd_(::open(p.c_str(), flags | O_CLOEXEC, mode)) {
    if (fd_ < 0) fail(sys("cannot open", p));
  }
  ~Fd() { close(); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }
  // Returns whether the close itself failed, which on some filesystems is where
  // a full disk is first admitted.
  bool close() {
    if (fd_ < 0) return true;
    int r = ::close(fd_);
    fd_ = -1;
    return r == 0;
  }

 private:
  int fd_;
};

// Counts bytes against a total, reports them, and is where a cancel is heard.
struct Meter {
  const Callbacks& cb;
  std::string_view stage;
  uint64_t done = 0;
  uint64_t total = 0;

  void step(uint64_t n) {
    done += n;
    if (cb.cancelled && cb.cancelled()) throw Cancelled();
    if (cb.progress) cb.progress(Progress{stage, done, total});
  }
};

// Reads `len` bytes at `off` and hands them over a chunk at a time, with each
// chunk's position within the range. A file that ends early is an error: the
// range came from a table, and a table that promised bytes the file does not
// have describes a file that has been cut short.
template <typename Fn>
void stream(const fs::path& p, uint64_t off, uint64_t len, Meter& m, Fn&& fn) {
  Fd in(p, O_RDONLY);
  std::vector<char> buf(kChunk);
  uint64_t at = 0;
  while (at < len) {
    size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), len - at));
    ssize_t r = ::pread(in.get(), buf.data(), want, static_cast<off_t>(off + at));
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) fail(sys("cannot read", p));
    if (r == 0) fail(p.string() + " ended " + std::to_string(len - at) + " bytes before it should have");
    fn(buf.data(), static_cast<size_t>(r), at);
    at += static_cast<uint64_t>(r);
    m.step(static_cast<uint64_t>(r));
  }
}

// Appends payloads to a file being linked, each on a page, and then the table
// and the trailer that describe them.
class Writer {
 public:
  explicit Writer(const fs::path& p) : path_(p), fd_(p, O_WRONLY | O_CREAT | O_TRUNC, 0755) {}

  void write(const char* data, size_t n) {
    while (n > 0) {
      ssize_t r = ::write(fd_.get(), data, n);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0) fail(sys("cannot write", path_));
      data += r;
      n -= static_cast<size_t>(r);
      pos_ += static_cast<uint64_t>(r);
    }
  }

  void pad() {
    static const char zeros[kAlign] = {};
    if (pos_ % kAlign) write(zeros, static_cast<size_t>(kAlign - pos_ % kAlign));
  }

  uint64_t pos() const { return pos_; }
  std::vector<Entry>& entries() { return entries_; }

  void add_bytes(Kind k, std::string name, std::string_view data) {
    pad();
    Entry e{k, 0, pos_, data.size(), hash_bytes(data.data(), data.size()), std::move(name), true};
    write(data.data(), data.size());
    entries_.push_back(std::move(e));
  }

  // The hash is of the bytes as they were read, and so of the bytes written;
  // if the source changes under the copy, what was copied is still what the
  // table describes, and it is the pack's own hashes, checked afterwards, that
  // notice the source was torn.
  void add_file(Kind k, std::string name, const fs::path& src, uint64_t len, Meter& m) {
    pad();
    Entry e{k, 0, pos_, len, {}, std::move(name), true};
    Hasher h;
    stream(src, 0, len, m, [&](const char* d, size_t n, uint64_t) {
      h.update(d, n);
      write(d, n);
    });
    e.blake3 = h.finish();
    entries_.push_back(std::move(e));
  }

  void finish() {
    std::string toc = encode_toc(entries_);
    uint64_t toc_off = pos_;
    write(toc.data(), toc.size());
    std::string tr = encode_trailer(toc_off, toc.size(), hash_bytes(toc.data(), toc.size()));
    write(tr.data(), tr.size());
    // Before the rename, not after: a rename that lands before the data does
    // is a finished-looking file full of zeros after a power cut.
    if (::fsync(fd_.get()) != 0) fail(sys("cannot flush", path_));
    if (!fd_.close()) fail(sys("cannot finish writing", path_));
  }

 private:
  fs::path path_;
  Fd fd_;
  uint64_t pos_ = 0;
  std::vector<Entry> entries_;
};

uint64_t size_of(const fs::path& p) {
  std::error_code ec;
  uint64_t n = fs::file_size(p, ec);
  if (ec) fail("cannot open " + p.string() + ": " + ec.message());
  return n;
}

// A player base is a v4 file carrying tools, runtime and app, once each, and
// nothing else: no meta, because it is not yet a player, and no games.
void check_base(const Toc& t, const std::string& what) {
  if (t.version != kTrailerVersion) {
    fail(what + " was linked with a v" + std::to_string(t.version) + " trailer; a player is built on v4 only");
  }
  for (Kind k : {Kind::Tools, Kind::Runtime, Kind::App}) {
    if (!t.find(k)) fail(what + " carries no " + kind_name(k));
  }
  for (const Entry& e : t.entries) {
    if (e.kind != Kind::Tools && e.kind != Kind::Runtime && e.kind != Kind::App) {
      fail(what + " carries a " + kind_name(e.kind) + ", which a player base does not");
    }
  }
}

// What a person is told was damaged: the game by its id, the rest by what it is.
std::string entry_words(const Entry& e) {
  switch (e.kind) {
    case Kind::Pack: return "game " + e.name;
    case Kind::Tools: return "the dwarfs tool";
    case Kind::Meta: return "bundle.meta";
    default: return "the " + kind_name(e.kind);
  }
}

Verified verify_with(const fs::path& p, Meter& m) {
  Toc t = read_toc(p);
  if (t.version != kTrailerVersion) fail(p.string() + " is not a v4 player");
  if (!t.is_player()) fail(p.string() + " carries no bundle.meta, so it is not a player");
  for (Kind k : {Kind::Tools, Kind::Runtime, Kind::App}) {
    if (!t.find(k)) fail(p.string() + " carries no " + kind_name(k));
  }
  // kretro carries a player base; a player built from one does not.
  if (t.find(Kind::PlayerBase)) fail(p.string() + " carries a player base, which a player does not");

  std::string meta_bytes;
  for (const Entry& e : t.entries) {
    Hasher whole;
    if (e.kind == Kind::Pack) {
      // Opened out of the player at its own offset, so the pack is judged by
      // the bytes the player carries and not by the file it was copied from.
      std::optional<Pack> pk;
      try {
        pk = Pack::open(p, t.at(e), e.len);
      } catch (const std::exception& ex) {
        fail("game " + e.name + " is damaged inside this file: " + ex.what());
      }
      if (pk->meta().id != e.name) {
        fail("the table says game " + e.name + ", but the pack there is game " + pk->meta().id);
      }
      if (!pk->has_body()) fail("game " + e.name + " is a recipe: it carries no game to play");
      if (pk->meta().tree.root() != pk->header().blake3_root) {
        fail("game " + e.name + " is damaged inside this file: its Merkle root does not match its tree");
      }
      // The body is hashed in the same pass as the whole entry: the pack is
      // read once, however large it is.
      Hasher body;
      uint64_t b0 = pk->header().body_off, b1 = b0 + pk->header().body_len;
      stream(p, t.at(e), e.len, m, [&](const char* d, size_t n, uint64_t at) {
        whole.update(d, n);
        uint64_t lo = std::max(at, b0), hi = std::min(at + n, b1);
        if (lo < hi) body.update(d + (lo - at), static_cast<size_t>(hi - lo));
      });
      if (body.finish() != pk->meta().body.blake3 || pk->header().body_len != pk->meta().body.length) {
        fail("game " + e.name + " is damaged inside this file: its body does not match its hash");
      }
    } else if (e.kind == Kind::Meta) {
      if (e.len > kMaxMetaBytes) fail("bundle.meta is " + std::to_string(e.len) + " bytes, more than any can be");
      stream(p, t.at(e), e.len, m, [&](const char* d, size_t n, uint64_t) {
        whole.update(d, n);
        meta_bytes.append(d, n);
      });
    } else {
      stream(p, t.at(e), e.len, m, [&](const char* d, size_t n, uint64_t) { whole.update(d, n); });
    }
    if (whole.finish() != e.blake3) {
      fail(entry_words(e) + " is damaged inside this file: its bytes do not match the table");
    }
  }

  BundleMeta meta = BundleMeta::decode(meta_bytes);
  for (const GameMeta& g : meta.games) {
    if (!t.pack(g.id)) fail("bundle.meta lists game " + g.id + ", which this file does not carry");
  }
  for (const Entry* e : t.all(Kind::Pack)) {
    bool listed = std::any_of(meta.games.begin(), meta.games.end(), [&](const GameMeta& g) { return g.id == e->name; });
    if (!listed) fail("this file carries game " + e->name + ", which bundle.meta does not list");
  }
  return Verified{std::move(t), std::move(meta)};
}

uint64_t payload_bytes(const Toc& t) {
  uint64_t n = 0;
  for (const Entry& e : t.entries) n += e.len;
  return n;
}

}  // namespace

Hash hash_range(const fs::path& p, uint64_t off, uint64_t len, const Callbacks& cb) {
  Meter m{cb, "hashing", 0, len};
  Hasher h;
  stream(p, off, len, m, [&](const char* d, size_t n, uint64_t) { h.update(d, n); });
  return h.finish();
}

BaseSource extract_player_base(const fs::path& self) {
  Toc t = read_toc(self);
  const Entry* e = t.find(Kind::PlayerBase);
  if (!e) fail(self.string() + " carries no player base: it was linked without one, so it cannot build players");
  Toc inner = read_toc(self, t.at(*e), e->len);
  check_base(inner, "the player base inside " + self.string());
  BaseSource b{self, t.at(*e), e->len, std::nullopt};
  if (e->hashed) b.blake3 = e->blake3;
  return b;
}

Verified verify_bundle(const fs::path& p, const Callbacks& cb) {
  Toc t = read_toc(p);
  Meter m{cb, "verifying", 0, payload_bytes(t)};
  return verify_with(p, m);
}

Built build_bundle(const BaseSource& base, const BundleMeta& meta, const std::vector<fs::path>& packs,
                   const fs::path& out, const Callbacks& cb) {
  // Everything that can be refused without writing a byte is refused first.
  meta.validate();
  uint64_t whole = size_of(base.path);
  uint64_t base_len = base.len ? *base.len : whole - std::min(base.off, whole);
  Toc bt = read_toc(base.path, base.off, base_len);
  check_base(bt, "the player base " + base.path.string());

  // The copied prefix ends where the base's last payload does; its table and
  // trailer are replaced, not kept.
  uint64_t prefix = 0;
  for (const Entry& e : bt.entries) prefix = std::max(prefix, e.off + e.len);

  std::vector<std::string> ids;
  std::vector<uint64_t> sizes;
  for (const fs::path& pp : packs) {
    Pack pk = Pack::open(pp);
    if (!pk.has_body()) fail(pp.string() + " is a recipe: it carries no game to play");
    const std::string& id = pk.meta().id;
    if (std::find(ids.begin(), ids.end(), id) != ids.end()) fail("game " + id + " is given twice");
    bool listed = std::any_of(meta.games.begin(), meta.games.end(), [&](const GameMeta& g) { return g.id == id; });
    if (!listed) fail(pp.string() + " is game " + id + ", which bundle.meta does not list");
    ids.push_back(id);
    sizes.push_back(size_of(pp));
  }
  for (const GameMeta& g : meta.games) {
    if (std::find(ids.begin(), ids.end(), g.id) == ids.end()) fail("no pack was given for game " + g.id);
  }
  // The finished player is renamed over <out>. If <out> is the base or a pack
  // it was built from, that rename replaces the author's only copy of it -
  // kretro itself, or a game off the shelf - with the player.
  {
    std::vector<fs::path> inputs = packs;
    inputs.push_back(base.path);
    for (const fs::path& in : inputs) {
      std::error_code eq;
      if (fs::equivalent(in, out, eq)) fail("the player cannot be written over " + in.string() + ", which it is built from");
    }
  }

  std::string meta_bytes = meta.encode();
  // The verify pass refuses a bundle.meta over this, so a build that wrote one
  // would copy every game and only then fail, with a message about the file
  // rather than about the pictures and DLLs that made it too big.
  if (meta_bytes.size() > kMaxMetaBytes) {
    fail("bundle.meta would be " + std::to_string(meta_bytes.size()) + " bytes, more than the " +
         std::to_string(kMaxMetaBytes) + " a player reads: the covers, banner, icon and extra files are too big");
  }
  // bundle.meta is written from memory and is not counted while copying; it is
  // counted when the verify pass reads it back.
  uint64_t copy_total = base.blake3 ? base_len : prefix;
  // The verify pass that follows the copy reads every payload once more.
  uint64_t payload_total = payload_bytes(bt) + meta_bytes.size();
  for (uint64_t s : sizes) copy_total += s, payload_total += s;
  Meter m{cb, "copying", 0, copy_total + payload_total};

  fs::path partial = out;
  partial += ".partial";
  std::error_code ec;
  try {
    // A .partial is only ever this function's own leftover, from a build that
    // was killed rather than cancelled.
    fs::remove(partial, ec);
    Writer w(partial);

    // The base goes across in one sequential read. When it came out of kretro
    // there is a hash for all of it, so all of it is hashed - the table and
    // trailer too - but only the prefix is written.
    Hasher h;
    stream(base.path, base.off, base.blake3 ? base_len : prefix, m, [&](const char* d, size_t n, uint64_t at) {
      if (base.blake3) h.update(d, n);
      if (at < prefix) w.write(d, static_cast<size_t>(std::min<uint64_t>(n, prefix - at)));
    });
    if (base.blake3 && h.finish() != *base.blake3) {
      fail("the player base inside " + base.path.string() + " is damaged: its bytes do not match its hash");
    }
    // The same offsets in the same order: the prefix is byte for byte the
    // base's, so the base's entries describe it exactly.
    w.entries() = bt.entries;
    std::sort(w.entries().begin(), w.entries().end(), [](const Entry& a, const Entry& b) { return a.off < b.off; });

    w.add_bytes(Kind::Meta, "", meta_bytes);
    for (size_t i = 0; i < packs.size(); ++i) w.add_file(Kind::Pack, ids[i], packs[i], sizes[i], m);
    w.finish();

    m.stage = "verifying";
    Verified v = verify_with(partial, m);

    fs::rename(partial, out, ec);
    if (ec) fail("cannot put the finished player at " + out.string() + ": " + ec.message());
    return Built{out, size_of(out), std::move(v.toc)};
  } catch (...) {
    fs::remove(partial, ec);
    throw;
  }
}

void link_file(const fs::path& out, const fs::path& bootstrap, const std::vector<Part>& parts) {
  Callbacks none;
  Meter m{none, "linking", 0, 0};
  Writer w(out);
  stream(bootstrap, 0, size_of(bootstrap), m, [&](const char* d, size_t n, uint64_t) { w.write(d, n); });
  for (const Part& p : parts) w.add_file(p.kind, p.name, p.path, size_of(p.path), m);
  w.finish();
}

}  // namespace kg::bundle
