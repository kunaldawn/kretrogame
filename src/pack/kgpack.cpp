#include "kgpack.h"

#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "../util/cbor.h"

namespace kg {
namespace fs = std::filesystem;

namespace {

constexpr int kMetaCompressionLevel = 19;  // metadata is small; spend the time

// 64 MiB is far beyond any real manifest. It bounds the decompression of the
// metadata, and it bounds the header's declared meta_len too: a pack is a thing
// people are told to pass around, so the number a stranger wrote in the header
// gets checked before a byte is allocated on the strength of it.
constexpr uint64_t kMaxMetaLen = 64u << 20;

// And the same for the body. A capsule carrying a three-disc game's discs is
// a few gigabytes and a large one is not much more; a terabyte is past anything
// that will ever be a game. The number matters less than the bound existing:
// body_off and body_len are a stranger's arithmetic, and Pack::open compares
// them against the file's size. Unbounded, 2^64-4096 is a body length that
// passes every check by wrapping.
constexpr uint64_t kMaxBodyLen = 1ull << 40;

void put_u16(std::string& s, uint16_t v) {
  s.push_back(static_cast<char>(v & 0xff));
  s.push_back(static_cast<char>(v >> 8));
}

void put_u64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

uint16_t get_u16(std::string_view s, size_t off) {
  return static_cast<uint16_t>(static_cast<uint8_t>(s[off])) |
         static_cast<uint16_t>(static_cast<uint8_t>(s[off + 1])) << 8;
}

uint64_t get_u64(std::string_view s, size_t off) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<uint8_t>(s[off + i])) << (8 * i);
  }
  return v;
}

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

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

std::string zstd_compress(std::string_view in) {
  size_t bound = ZSTD_compressBound(in.size());
  std::string out;
  out.resize(bound);
  size_t n = ZSTD_compress(out.data(), bound, in.data(), in.size(), kMetaCompressionLevel);
  if (ZSTD_isError(n)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(n));
  out.resize(n);
  return out;
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

void encode_anchors(cbor::Encoder& e, const std::vector<Anchor>& v) {
  e.array(v.size());
  for (const Anchor& a : v) {
    e.map(3);
    e.text("path"); e.text(a.path);
    e.text("size"); e.uint_val(a.size);
    e.text("blake3"); e.bytes(a.hash.data(), a.hash.size());
  }
}

void encode_strings(cbor::Encoder& e, const std::vector<std::string>& v) {
  e.array(v.size());
  for (const std::string& s : v) e.text(s);
}

std::vector<std::string> decode_strings(const cbor::Value* v) {
  std::vector<std::string> out;
  if (!v || !v->is_array()) return out;
  for (const cbor::Value& x : v->arr) {
    if (x.is_text()) out.push_back(x.s);
  }
  return out;
}

Hash decode_hash(const cbor::Value* v) {
  Hash h{};
  if (v && v->is_bytes() && v->s.size() == h.size()) {
    std::memcpy(h.data(), v->s.data(), h.size());
  }
  return h;
}

// What is put back in an error message. A pack from a stranger holds a string
// of the stranger's choosing, and a terminal is not the place to find out how
// long it is or what control codes it contains.
std::string as_shown(std::string_view s) {
  std::string out = "\"";
  size_t n = 0;
  for (char c : s) {
    if (n++ == 40) { out += "..."; break; }
    unsigned char u = static_cast<unsigned char>(c);
    out.push_back(u < 0x20 || u == 0x7f ? '?' : c);
  }
  out.push_back('"');
  return out;
}

}  // namespace

bool id_is_safe(std::string_view id) {
  // One path component and a conservative one: letters, digits, dash,
  // underscore and dot, starting with a letter or a digit. That admits every id
  // slug() has ever produced and every id anybody has typed into the wizard,
  // and it admits nothing that is a separator, nothing that is "." or "..",
  // and nothing that begins with a dot and hides.
  if (id.empty() || id.size() > 100) return false;
  for (char c : id) {
    if (c >= 'a' && c <= 'z') continue;
    if (c >= 'A' && c <= 'Z') continue;
    if (c >= '0' && c <= '9') continue;
    if (c == '-' || c == '_' || c == '.') continue;
    return false;
  }
  char first = id.front();
  return (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
         (first >= '0' && first <= '9');
}

bool install_dir_is_safe(std::string_view dir) {
  if (dir.empty()) return true;         // no install directory recorded; nothing is linked
  if (dir.size() > 1024) return false;
  // A path recorded on Windows spells itself with backslashes, and drive_c is
  // reached through a POSIX join, so the separators are made one kind first -
  // otherwise "..\..\.bashrc" is one innocent-looking component.
  std::string s(dir);
  std::replace(s.begin(), s.end(), '\\', '/');
  fs::path p(s);
  if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
  fs::path norm = p.lexically_normal();
  for (const fs::path& part : norm) {
    if (part == "..") return false;
  }
  return !norm.empty() && norm != ".";
}

std::string Header::serialize() const {
  std::string s;
  s.reserve(kHeaderSize);
  s.append(kMagic, sizeof(kMagic));
  s.push_back(static_cast<char>(revision));
  put_u16(s, format_version);
  put_u16(s, flags);
  put_u16(s, static_cast<uint16_t>(kind));
  put_u16(s, 0);  // reserved
  put_u64(s, meta_off);
  put_u64(s, meta_len);
  put_u64(s, body_off);
  put_u64(s, body_len);
  put_u64(s, sig_off);
  put_u64(s, sig_len);
  s.append(reinterpret_cast<const char*>(blake3_root.data()), blake3_root.size());
  if (s.size() != kHeaderSize) throw std::runtime_error("header size drifted from 96 bytes");
  return s;
}

Header Header::parse(std::string_view raw) {
  if (raw.size() < kHeaderSize) throw std::runtime_error("not a kgpack: file is too short");
  if (std::memcmp(raw.data(), kMagic, sizeof(kMagic)) != 0) {
    throw std::runtime_error("not a kgpack: bad magic");
  }
  Header h;
  h.revision = static_cast<uint8_t>(raw[7]);
  // A range, not an equality. Every pack ever written by this program is either
  // revision 1 (flat body) or revision 2 (rooted body), and both are readable:
  // Meta::layout says which, and a revision 1 pack that never carried its discs
  // is still a game that plays. Anything above 2 is a body laid out by a build
  // that came after this one, and mounting it on a guess is how you hand a game
  // the wrong directory.
  if (h.revision < kOldestReadableRevision || h.revision > kContainerRevision) {
    throw std::runtime_error("kgpack container revision " + std::to_string(h.revision) +
                             " is not one this build understands (it reads " +
                             std::to_string(kOldestReadableRevision) + " to " +
                             std::to_string(kContainerRevision) + ")");
  }
  h.format_version = get_u16(raw, 8);
  h.flags = get_u16(raw, 10);
  uint16_t k = get_u16(raw, 12);
  if (k < 1 || k > 3) throw std::runtime_error("kgpack declares an unknown kind");
  h.kind = static_cast<Kind>(k);
  h.meta_off = get_u64(raw, 16);
  h.meta_len = get_u64(raw, 24);
  if (h.meta_len > kMaxMetaLen) {
    throw std::runtime_error("kgpack declares an implausible amount of metadata");
  }
  h.body_off = get_u64(raw, 32);
  h.body_len = get_u64(raw, 40);
  if (h.body_len > kMaxBodyLen) {
    throw std::runtime_error("kgpack declares an implausible amount of body");
  }
  h.sig_off = get_u64(raw, 48);
  h.sig_len = get_u64(raw, 56);
  std::memcpy(h.blake3_root.data(), raw.data() + 64, h.blake3_root.size());
  if (h.has_body() && h.body_len == 0) throw std::runtime_error("kgpack claims a body but declares none");
  if (!h.has_body() && h.body_len != 0) throw std::runtime_error("kgpack declares a body it does not claim");
  return h;
}

std::string Meta::encode() const {
  cbor::Encoder e;
  e.map(16);

  e.text("id"); e.text(id);
  e.text("name"); e.text(name);
  e.text("year"); e.uint_val(year);
  e.text("layout"); e.text(layout);

  e.text("who");
  e.map(2);
  e.text("developer"); e.text(developer);
  e.text("publisher"); e.text(publisher);

  e.text("recipe");
  e.map(8);
  e.text("method"); e.text(recipe.method);
  e.text("member"); e.text(recipe.member);
  e.text("subdir"); e.text(recipe.subdir);
  e.text("setup"); e.text(recipe.setup);
  e.text("setup_ref"); e.text(recipe.setup_ref);
  e.text("discs"); encode_strings(e, recipe.discs);
  e.text("verify"); encode_strings(e, recipe.verify);
  e.text("fingerprints");
  e.array(recipe.fingerprints.size());
  for (const DiscFingerprint& d : recipe.fingerprints) {
    e.map(6);
    e.text("filename"); e.text(d.filename);
    e.text("size"); e.uint_val(d.size);
    e.text("blake3"); e.bytes(d.blake3.data(), d.blake3.size());
    e.text("volume_id"); e.text(d.volume_id);
    e.text("created"); e.text(d.created);
    e.text("anchors"); encode_anchors(e, d.anchors);
  }

  e.text("run");
  e.map(5);
  e.text("exe"); e.text(run.exe);
  e.text("args"); e.text(run.args);
  e.text("windows_version"); e.text(run.windows_version);
  e.text("width"); e.uint_val(run.width);
  e.text("height"); e.uint_val(run.height);

  e.text("runtime");
  e.map(5);
  e.text("id"); e.text(runtime.id);
  e.text("blake3"); e.bytes(runtime.blake3.data(), runtime.blake3.size());
  e.text("dlloverrides"); e.text(runtime.dlloverrides);
  e.text("winetricks"); encode_strings(e, runtime.winetricks);
  e.text("dgvoodoo"); e.boolean(runtime.dgvoodoo);

  e.text("present");
  e.map(2);
  e.text("dar"); e.text(present.dar);
  e.text("pause_on_blur"); e.boolean(present.pause_on_blur);

  // The key is "discs2" because the recipe map already has a "discs"; keeping
  // the names distinct saves the decoder from having to know which map it is in.
  e.text("install");
  e.map(1);
  e.text("install_dir"); e.text(install.install_dir);

  e.text("registry");
  e.map(1);
  e.text("fragment"); e.text(registry.fragment);

  e.text("system");
  e.map(2);
  e.text("files"); e.uint_val(system.files);
  e.text("bytes"); e.uint_val(system.bytes);

  e.text("input");
  e.map(input.size());
  for (const auto& kv : input) { e.text(kv.first); e.text(kv.second); }

  e.text("discs2");
  e.array(discs.size());
  for (const Disc& d : discs) {
    e.map(5);
    e.text("label"); e.text(d.label);
    e.text("serial"); e.uint_val(d.serial);
    e.text("ref"); e.text(d.ref);
    e.text("source"); e.text(d.source);
    e.text("embedded"); e.boolean(d.embedded);
  }

  e.text("body");
  // A key only when there is something to say, so a pack that says nothing
  // about its packing encodes byte for byte as it did before there was one.
  e.map(body.packing.empty() ? 2 : 3);
  e.text("length"); e.uint_val(body.length);
  e.text("blake3"); e.bytes(body.blake3.data(), body.blake3.size());
  if (!body.packing.empty()) { e.text("packing"); e.text(body.packing); }

  // The tree travels as its canonical text. It is the hash input, so shipping
  // anything else would mean two representations that could disagree.
  e.text("tree"); e.text(tree.canonical());

  return e.take();
}

Meta Meta::decode(std::string_view data) {
  cbor::Value v = cbor::decode(data);
  if (!v.is_map()) throw std::runtime_error("kgpack metadata is not a map");

  Meta m;
  m.id = v.find("id") ? v.find("id")->text_or() : "";
  // The id is a path component on whoever's machine opens this: it names
  // games/<id>.kgpack, saves/<id> and prefixes/<id>. A pack is a file people
  // send each other, so an id of "../../../.config/autostart" would let the
  // sender pick the directory as well as the name - on import, on install, on
  // play and on uninstall. The pack is refused whole rather than sanitised,
  // because a pack that has to be corrected to be safe is not one to trust with
  // an executable either.
  if (!id_is_safe(m.id)) {
    throw std::runtime_error("this pack calls itself " + as_shown(m.id) +
                             ", which is not a name a game can have here");
  }
  m.name = v.find("name") ? v.find("name")->text_or() : "";
  m.year = static_cast<uint32_t>(v.find("year") ? v.find("year")->uint_or() : 0);
  // Absent means flat. That is not a fallback, it is the truth about a pack
  // written before the rooted layout existed.
  if (const cbor::Value* x = v.find("layout")) m.layout = x->text_or("flat");

  if (const cbor::Value* w = v.find("who")) {
    if (const cbor::Value* x = w->find("developer")) m.developer = x->text_or();
    if (const cbor::Value* x = w->find("publisher")) m.publisher = x->text_or();
  }

  if (const cbor::Value* r = v.find("recipe")) {
    if (const cbor::Value* x = r->find("method")) m.recipe.method = x->text_or();
    if (const cbor::Value* x = r->find("member")) m.recipe.member = x->text_or();
    if (const cbor::Value* x = r->find("subdir")) m.recipe.subdir = x->text_or();
    if (const cbor::Value* x = r->find("setup")) m.recipe.setup = x->text_or();
    if (const cbor::Value* x = r->find("setup_ref")) m.recipe.setup_ref = x->text_or();
    m.recipe.discs = decode_strings(r->find("discs"));
    m.recipe.verify = decode_strings(r->find("verify"));
    if (const cbor::Value* fs_ = r->find("fingerprints"); fs_ && fs_->is_array()) {
      for (const cbor::Value& d : fs_->arr) {
        DiscFingerprint f;
        if (const cbor::Value* x = d.find("filename")) f.filename = x->text_or();
        if (const cbor::Value* x = d.find("size")) f.size = x->uint_or();
        f.blake3 = decode_hash(d.find("blake3"));
        if (const cbor::Value* x = d.find("volume_id")) f.volume_id = x->text_or();
        if (const cbor::Value* x = d.find("created")) f.created = x->text_or();
        if (const cbor::Value* an = d.find("anchors"); an && an->is_array()) {
          for (const cbor::Value& a : an->arr) {
            Anchor anc;
            if (const cbor::Value* x = a.find("path")) anc.path = x->text_or();
            if (const cbor::Value* x = a.find("size")) anc.size = x->uint_or();
            anc.hash = decode_hash(a.find("blake3"));
            f.anchors.push_back(std::move(anc));
          }
        }
        m.recipe.fingerprints.push_back(std::move(f));
      }
    }
  }

  // Absent keys leave the defaults, so packs written before these blocks
  // existed still decode.
  if (const cbor::Value* r = v.find("install")) {
    if (const cbor::Value* x = r->find("install_dir")) m.install.install_dir = x->text_or();
  }
  // Play joins this onto the prefix's drive_c, removes whatever is there and
  // puts a symlink in its place (session.cpp). "../../../.bashrc" is therefore
  // a pack that deletes a file in the importer's home the first time the game
  // is started.
  if (!install_dir_is_safe(m.install.install_dir)) {
    throw std::runtime_error("this pack says it was installed to " +
                             as_shown(m.install.install_dir) + ", which is not under C:");
  }

  if (const cbor::Value* r = v.find("registry")) {
    if (const cbor::Value* x = r->find("fragment")) m.registry.fragment = x->text_or();
  }

  // Absent in every pack written before system/ existed, and absent is the
  // truth about those: they carried the game directory and nothing else.
  if (const cbor::Value* r = v.find("system")) {
    if (const cbor::Value* x = r->find("files")) m.system.files = static_cast<uint32_t>(x->uint_or());
    if (const cbor::Value* x = r->find("bytes")) m.system.bytes = x->uint_or();
  }

  if (const cbor::Value* im = v.find("input"); im && im->is_map()) {
    for (const auto& kv : im->map) m.input[kv.first.text_or()] = kv.second.text_or();
  }

  if (const cbor::Value* ds = v.find("discs2"); ds && ds->is_array()) {
    for (const cbor::Value& d : ds->arr) {
      Meta::Disc di;
      if (const cbor::Value* x = d.find("label")) di.label = x->text_or();
      if (const cbor::Value* x = d.find("serial")) di.serial = static_cast<uint32_t>(x->uint_or());
      if (const cbor::Value* x = d.find("ref")) di.ref = x->text_or();
      if (const cbor::Value* x = d.find("source")) di.source = x->text_or();
      if (const cbor::Value* x = d.find("embedded")) di.embedded = x->bool_or();
      m.discs.push_back(std::move(di));
    }
  }

  if (const cbor::Value* r = v.find("run")) {
    if (const cbor::Value* x = r->find("exe")) m.run.exe = x->text_or();
    if (const cbor::Value* x = r->find("args")) m.run.args = x->text_or();
    if (const cbor::Value* x = r->find("windows_version")) m.run.windows_version = x->text_or();
    if (const cbor::Value* x = r->find("width")) m.run.width = static_cast<uint32_t>(x->uint_or());
    if (const cbor::Value* x = r->find("height")) m.run.height = static_cast<uint32_t>(x->uint_or());
  }

  if (const cbor::Value* r = v.find("runtime")) {
    if (const cbor::Value* x = r->find("id")) m.runtime.id = x->text_or();
    m.runtime.blake3 = decode_hash(r->find("blake3"));
    if (const cbor::Value* x = r->find("dlloverrides")) m.runtime.dlloverrides = x->text_or();
    m.runtime.winetricks = decode_strings(r->find("winetricks"));
    if (const cbor::Value* x = r->find("dgvoodoo")) m.runtime.dgvoodoo = x->bool_or();
  }

  if (const cbor::Value* r = v.find("present")) {
    if (const cbor::Value* x = r->find("dar")) m.present.dar = x->text_or("4:3");
    if (const cbor::Value* x = r->find("pause_on_blur")) m.present.pause_on_blur = x->bool_or(true);
  }

  if (const cbor::Value* r = v.find("body")) {
    if (const cbor::Value* x = r->find("length")) m.body.length = x->uint_or();
    m.body.blake3 = decode_hash(r->find("blake3"));
    if (const cbor::Value* x = r->find("packing")) m.body.packing = x->text_or("");
  }

  if (const cbor::Value* t = v.find("tree"); t && t->is_text()) {
    m.tree = Tree::from_canonical(t->s);
  }

  return m;
}

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
  h.meta_off = kHeaderSize;
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
  if (len < kHeaderSize) throw std::runtime_error("not a kgpack: file is too short");
  std::string head = read_range(p, off, kHeaderSize);
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
  pk.meta_ = Meta::decode(meta_cbor);
  return pk;
}

void Pack::extract_body(const fs::path& out) const {
  if (!has_body()) throw std::runtime_error("this kgpack has no body to extract");
  copy_range(path_, base_ + header_.body_off, header_.body_len, out);
}

Pack::Verification Pack::verify() const {
  Verification v;
  v.root_matches = meta_.tree.root() == header_.blake3_root;
  if (!v.root_matches) {
    v.detail = "tree Merkle root does not match the header";
  }

  if (has_body()) {
    Hash actual = hash_range(path_, base_ + header_.body_off, header_.body_len);
    v.body_matches = actual == meta_.body.blake3 && header_.body_len == meta_.body.length;
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
