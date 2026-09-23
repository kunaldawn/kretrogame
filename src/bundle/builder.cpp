#include "builder.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "../gpu/caps.h"
#include "../install/build.h"
#include "../player/policy.h"
#include "../util/cbor.h"
#include "../util/paths.h"
#include "../util/proc.h"

namespace kg::bundle {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

uint64_t align_up(uint64_t n) { return (n + kAlign - 1) / kAlign * kAlign; }

// The name the page uses for a game in a sentence.
std::string called(const DraftGame& g) { return g.name.empty() ? g.id : g.name; }

// The shelf keeps a game as <id>.kgpack; a pack copied in by hand may be named
// anything, and is found by the id it says it is.
fs::path pack_for(const fs::path& games_dir, const std::string& id) {
  std::error_code ec;
  fs::path direct = games_dir / (id + ".kgpack");
  if (fs::exists(direct, ec)) return direct;
  for (const fs::directory_entry& de : fs::directory_iterator(games_dir, ec)) {
    if (de.path().extension() != ".kgpack") continue;
    try {
      if (Pack::open(de.path()).meta().id == id) return de.path();
    } catch (const std::exception&) {
    }
  }
  return {};
}

// ---- the draft's CBOR ------------------------------------------------------------

using cbor::Value;

[[noreturn]] void bad(const std::string& why) { throw std::runtime_error("remembered bundle: " + why); }

const Value* field(const Value& m, std::string_view key, Value::Type t, const char* word) {
  const Value* v = m.find(key);
  if (!v) return nullptr;
  if (v->type != t) bad("'" + std::string(key) + "' should be " + word);
  return v;
}
std::string text(const Value& m, std::string_view key, std::string def = "") {
  const Value* v = field(m, key, Value::Type::Text, "text");
  return v ? v->s : def;
}
std::string bytes(const Value& m, std::string_view key) {
  const Value* v = field(m, key, Value::Type::Bytes, "bytes");
  return v ? v->s : std::string();
}
bool boolean(const Value& m, std::string_view key) {
  const Value* v = field(m, key, Value::Type::Bool, "a boolean");
  return v && v->b;
}
uint64_t whole(const Value& m, std::string_view key) {
  const Value* v = field(m, key, Value::Type::Uint, "a whole number");
  return v ? v->u : 0;
}
const Value* list(const Value& m, std::string_view key) { return field(m, key, Value::Type::Array, "an array"); }

// Maps are written with their size counted first, which is the one thing a
// hand-written encoder gets wrong; this counts as it goes.
struct MapOut {
  std::vector<std::pair<std::string, std::string>> items;  // key, encoded value
  void add(const std::string& k, const std::string& encoded) { items.emplace_back(k, encoded); }
  void t(const std::string& k, const std::string& v) { cbor::Encoder e; e.text(v); add(k, e.take()); }
  void b(const std::string& k, const std::string& v) { cbor::Encoder e; e.bytes(v.data(), v.size()); add(k, e.take()); }
  void flag(const std::string& k, bool v) { cbor::Encoder e; e.boolean(v); add(k, e.take()); }
  void u(const std::string& k, uint64_t v) { cbor::Encoder e; e.uint_val(v); add(k, e.take()); }
  std::string take() {
    cbor::Encoder e;
    e.map(items.size());
    std::string out = e.take();
    for (auto& [k, v] : items) {
      cbor::Encoder ke;
      ke.text(k);
      out += ke.take();
      out += v;
    }
    return out;
  }
};

std::string encode_game(const DraftGame& g) {
  MapOut m;
  m.t("id", g.id);
  m.t("name", g.name);
  m.u("year", g.year);
  if (!g.cover.empty()) m.b("cover", g.cover);
  m.t("cover_from", g.cover_from);
  m.t("backend", g.backend);
  m.flag("needs_gpu", g.needs_gpu);
  m.t("display", g.display);
  m.flag("fullscreen", g.fullscreen);
  m.t("gamepad", g.gamepad);
  cbor::Encoder dl;
  dl.array(g.extra_dlls.size());
  for (const GameMeta::Dll& d : g.extra_dlls) {
    dl.map(2);
    dl.text("name"); dl.text(d.name);
    dl.text("data"); dl.bytes(d.data.data(), d.data.size());
  }
  m.add("extra_dlls", dl.take());
  m.flag("embed_key", g.embed_key);
  m.t("key_path", g.key_path);
  m.t("key_value", g.key_value);
  return m.take();
}

}  // namespace

// ---- identity ---------------------------------------------------------------

std::string id_from_title(std::string_view title) {
  std::string s = install::slug(title);
  // Long enough for any title anybody means; short enough that the file name
  // it becomes half of still fits on every filesystem an author uploads from.
  constexpr size_t kMax = 64;
  if (s.size() > kMax) s.resize(kMax);
  while (!s.empty() && s.back() == '-') s.pop_back();
  if (!kg::id_is_safe(s)) return "bundle";
  return s;
}

bool version_is_safe(std::string_view v) { return v.size() <= 32 && kg::id_is_safe(v); }

// ---- the draft --------------------------------------------------------------

bool Draft::acked(std::string_view check_id) const {
  return std::find(acknowledged.begin(), acknowledged.end(), check_id) != acknowledged.end();
}

std::string encode_draft(const Draft& d) {
  MapOut m;
  m.u("format", kDraftFormat);
  m.t("id", d.id);
  m.flag("id_typed", d.id_typed);
  m.flag("published", d.published);
  m.t("title", d.title);
  m.t("version", d.version);
  if (!d.banner.empty()) m.b("banner", d.banner);
  if (!d.icon.empty()) m.b("icon", d.icon);
  m.t("banner_from", d.banner_from);
  m.t("icon_from", d.icon_from);
  {
    std::string g;
    cbor::Encoder e;
    e.array(d.games.size());
    g = e.take();
    for (const DraftGame& x : d.games) g += encode_game(x);
    m.add("games", g);
  }
  m.flag("rights", d.rights);
  {
    cbor::Encoder e;
    e.array(d.acknowledged.size());
    for (const std::string& a : d.acknowledged) e.text(a);
    m.add("acknowledged", e.take());
  }
  m.t("out_dir", d.out_dir);
  m.t("last_built", d.last_built);
  m.u("last_size", d.last_size);
  m.t("last_built_at", d.last_built_at);
  return m.take();
}

Draft decode_draft(std::string_view data) {
  Value root;
  try {
    root = cbor::decode(data);
  } catch (const std::exception& ex) {
    bad(ex.what());
  }
  if (!root.is_map()) bad("not a map");
  // A newer format may mean something else by the same keys; better to say so
  // than to rebuild a bundle from a guess.
  uint64_t format = whole(root, "format");
  if (format != kDraftFormat) bad("format " + std::to_string(format) + " is not one this kretro reads");
  Draft d;
  d.id = text(root, "id");
  d.id_typed = boolean(root, "id_typed");
  d.published = boolean(root, "published");
  d.title = text(root, "title");
  d.version = text(root, "version", "1.0");
  d.banner = bytes(root, "banner");
  d.icon = bytes(root, "icon");
  d.banner_from = text(root, "banner_from");
  d.icon_from = text(root, "icon_from");
  if (const Value* gs = list(root, "games")) {
    for (const Value& gv : gs->arr) {
      if (!gv.is_map()) bad("a game should be a map");
      DraftGame g;
      g.id = text(gv, "id");
      g.name = text(gv, "name");
      uint64_t y = whole(gv, "year");
      g.year = y > UINT32_MAX ? 0 : static_cast<uint32_t>(y);
      g.cover = bytes(gv, "cover");
      g.cover_from = text(gv, "cover_from");
      g.backend = text(gv, "backend", "auto");
      g.needs_gpu = boolean(gv, "needs_gpu");
      g.display = text(gv, "display", "integer");
      g.fullscreen = boolean(gv, "fullscreen");
      g.gamepad = text(gv, "gamepad");
      if (const Value* ds = list(gv, "extra_dlls")) {
        for (const Value& dv : ds->arr) {
          if (!dv.is_map()) bad("an extra file should be a map");
          g.extra_dlls.push_back({text(dv, "name"), bytes(dv, "data")});
        }
      }
      g.embed_key = boolean(gv, "embed_key");
      g.key_path = text(gv, "key_path");
      g.key_value = text(gv, "key_value");
      d.games.push_back(std::move(g));
    }
  }
  d.rights = boolean(root, "rights");
  if (const Value* as = list(root, "acknowledged")) {
    for (const Value& a : as->arr) {
      if (!a.is_text()) bad("an acknowledgement should be text");
      d.acknowledged.push_back(a.s);
    }
  }
  d.out_dir = text(root, "out_dir");
  d.last_built = text(root, "last_built");
  d.last_size = whole(root, "last_size");
  d.last_built_at = text(root, "last_built_at");
  return d;
}

fs::path bundles_dir() { return state_dir() / "bundles"; }

std::vector<Draft> load_drafts(const fs::path& dir, std::vector<std::string>* unreadable) {
  std::vector<Draft> out;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    if (de.path().extension() != ".cbor") continue;
    try {
      out.push_back(decode_draft(slurp(de.path())));
    } catch (const std::exception& ex) {
      if (unreadable) unreadable->push_back(de.path().filename().string() + ": " + ex.what());
    }
  }
  std::sort(out.begin(), out.end(), [](const Draft& a, const Draft& b) {
    return a.title != b.title ? a.title < b.title : a.id < b.id;
  });
  return out;
}

void save_draft(const fs::path& dir, const Draft& d, const std::string& was) {
  // The file is named by the id, and the id is joined onto a directory: it is
  // held to the rule the id is held to everywhere else.
  if (!kg::id_is_safe(d.id)) throw std::runtime_error("the bundle id '" + d.id + "' is not a name a file can have");
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path file = dir / (d.id + ".cbor");
  if (was != d.id && fs::exists(file, ec)) {
    throw std::runtime_error("another bundle is already remembered as '" + d.id +
                             "': give this one a different title or id");
  }
  fs::path tmp = file;
  tmp += ".new";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    std::string bytes = encode_draft(d);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.close();
    if (!f) {
      fs::remove(tmp, ec);
      throw std::runtime_error("cannot remember this bundle in " + dir.string());
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec);
    throw std::runtime_error("cannot remember this bundle in " + dir.string() + ": " + ec.message());
  }
  if (!was.empty() && was != d.id && kg::id_is_safe(was)) fs::remove(dir / (was + ".cbor"), ec);
}

void forget_draft(const fs::path& dir, const std::string& id) {
  if (!kg::id_is_safe(id)) return;
  std::error_code ec;
  fs::remove(dir / (id + ".cbor"), ec);
}

std::string unused_id(const fs::path& dir, const std::string& base) {
  std::error_code ec;
  std::string id = base;
  for (int n = 2; fs::exists(dir / (id + ".cbor"), ec); ++n) id = base + "-" + std::to_string(n);
  return id;
}

bool stamp_built(const fs::path& dir, const std::string& id, const std::string& path, uint64_t size,
                 const std::string& when) {
  if (!kg::id_is_safe(id)) return false;
  fs::path file = dir / (id + ".cbor");
  std::error_code ec;
  if (!fs::exists(file, ec)) return false;
  Draft d;
  try {
    d = decode_draft(slurp(file));
  } catch (const std::exception&) {
    return false;
  }
  d.last_built = path;
  d.last_size = size;
  d.last_built_at = when;
  save_draft(dir, d, d.id);
  return true;
}

bool remember_built(const fs::path& dir, Draft d, const std::vector<Check>& checks, const std::string& path,
                    uint64_t size, const std::string& when) {
  if (!kg::id_is_safe(d.id)) return false;
  std::error_code ec;
  if (fs::exists(dir / (d.id + ".cbor"), ec)) return false;
  for (const Check& c : checks) {
    if (!d.acked(c.id)) d.acknowledged.push_back(c.id);
  }
  d.last_built = path;
  d.last_size = size;
  d.last_built_at = when;
  save_draft(dir, d);
  return true;
}

// ---- packs ------------------------------------------------------------------------

PackFacts read_pack_facts(const fs::path& pack) {
  Pack p = Pack::open(pack);
  PackFacts f;
  f.path = pack;
  f.meta = p.meta();
  std::error_code ec;
  f.bytes = fs::file_size(pack, ec);
  for (const Meta::Disc& d : f.meta.discs) (d.embedded ? f.discs_carried : f.discs_named)++;
  f.without_discs = f.bytes;
  if (f.discs_carried > 0) {
    // What the discs weigh unpacked is what their images weighed; the game's
    // share is its own tree and what its installer wrote to C:.
    uint64_t disc_bytes = 0;
    for (const DiscFingerprint& fp : f.meta.recipe.fingerprints) disc_bytes += fp.size;
    uint64_t game_bytes = f.meta.tree.total_bytes() + f.meta.system.bytes;
    if (disc_bytes > 0 && game_bytes + disc_bytes > 0) {
      long double share = static_cast<long double>(game_bytes) / static_cast<long double>(game_bytes + disc_bytes);
      f.without_discs = static_cast<uint64_t>(static_cast<long double>(f.bytes) * share);
    }
  }
  return f;
}

DraftGame game_from_pack(const PackFacts& f, const fs::path& cover_png) {
  DraftGame g;
  g.id = f.meta.id;
  g.name = f.meta.name.empty() ? f.meta.id : f.meta.name;
  g.year = f.meta.year;
  std::error_code ec;
  if (!cover_png.empty() && fs::exists(cover_png, ec)) {
    g.cover = slurp(cover_png);
    g.cover_from = cover_png.string();
  }
  // The pack's own gamepad bindings, in the form the player reads back, so
  // the player's default for this game is what the author has been playing
  // with.
  g.gamepad = format_gamepad(f.meta.input);
  return g;
}

// ---- graphics -------------------------------------------------------------------------

AutoBackend auto_backend(const pe::Imports& exe) {
  AutoBackend a;
  a.known = exe.ok;
  gpu::HostCaps caps;
  caps.vulkan.ran = true;
  caps.vulkan.level = gpu::VulkanLevel::V1_4;
  caps.vulkan.api_version = "1.4";
  caps.gl.ran = true;
  caps.gl.hardware = true;
  caps.render_device = true;
  caps.other_gpu = true;
  player::Decision d = player::choose_backend(player::AuthorBackend::Auto, exe, caps, false);
  a.reason = d.reason;
  switch (d.backend) {
    case player::Backend::Dxvk3:
    case player::Backend::Dxvk2: a.backend = "dxvk"; break;
    case player::Backend::WineD3DVulkan: a.backend = "wined3d-vk"; break;
    case player::Backend::WineD3DGL: a.backend = "wined3d-gl"; break;
    case player::Backend::CncDdraw: a.backend = "cnc-ddraw"; break;
    case player::Backend::NativeGL: a.backend = "native OpenGL"; break;
    default: a.backend = player::backend_name(d.backend); break;
  }
  return a;
}

pe::Imports read_exe_imports(const PackFacts& f, const fs::path& tool, const fs::path& scratch) {
  pe::Imports none;
  auto fail = [&](std::string why) {
    none.ok = false;
    none.error = std::move(why);
    return none;
  };
  if (f.meta.run.exe.empty()) return fail("the pack names no executable");
  std::error_code ec;
  if (tool.empty() || !fs::exists(tool, ec)) return fail("no DwarFS tool to read the pack with");

  // The executable as the tree spells it. run.exe was typed by a person or an
  // installer, and Wine does not care about case; dwarfsextract's pattern does.
  std::string want = f.meta.run.exe;
  std::replace(want.begin(), want.end(), '\\', '/');
  while (!want.empty() && want.front() == '/') want.erase(want.begin());
  std::string inside;
  for (const TreeEntry& e : f.meta.tree.entries()) {
    if (e.is_regular() && lower(e.path) == lower(want)) { inside = e.path; break; }
  }
  if (inside.empty()) return fail(f.meta.run.exe + " is not in the pack's tree");
  if (f.meta.rooted()) inside = "game/" + inside;

  std::optional<Pack> p;
  try {
    p = Pack::open(f.path);
  } catch (const std::exception& ex) {
    return fail(ex.what());
  }
  if (!p->has_body()) return fail("the pack is a recipe and carries no game");
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  ProcResult r = kg::run({tool.string(), "--tool=dwarfsextract", "-i", f.path.string(), "-O",
                          std::to_string(p->base() + p->header().body_off), "-o", scratch.string(),
                          "--pattern", inside, "--log-level=error"});
  fs::path got = scratch / inside;
  pe::Imports out;
  if (!r.ok() || !fs::exists(got, ec)) {
    std::string why = r.out;
    while (!why.empty() && (why.back() == '\n' || why.back() == ' ')) why.pop_back();
    out = fail("could not read " + inside + " out of the pack" + (why.empty() ? "" : ": " + why));
  } else {
    out = pe::parse_file(got);
  }
  fs::remove_all(scratch, ec);
  return out;
}

bool packed_before_faster_loading(const PackFacts& f) {
  return f.meta.body.length > 0 && f.meta.body.packing != kBodyPacking;
}

void repack_for_faster_loading(const fs::path& pack, const fs::path& tool, const fs::path& scratch,
                               const Callbacks& cb) {
  std::error_code ec;
  if (tool.empty() || !fs::exists(tool, ec)) throw std::runtime_error("no DwarFS tool to repack with");
  Pack p = Pack::open(pack);
  const Header& h = p.header();
  if (!p.has_body()) throw std::runtime_error(pack.filename().string() + " is a recipe and carries no game");
  if (h.body_is_squashfs()) throw std::runtime_error(pack.filename().string() + " has a squashfs body");
  // A signature covers the body it signed, and a new body has none.
  if (h.flags & kSigned) throw std::runtime_error(pack.filename().string() + " is signed; repacking would unsign it");
  Meta m = p.meta();
  auto step = [&](std::string_view stage, uint64_t done) {
    if (cb.cancelled && cb.cancelled()) throw Cancelled();
    if (cb.progress) cb.progress(Progress{stage, done, 4});
  };

  struct Scratch {
    fs::path dir;
    ~Scratch() {
      std::error_code e;
      fs::remove_all(dir, e);
    }
  } s{scratch};
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  if (ec) throw std::runtime_error("cannot make " + scratch.string() + ": " + ec.message());

  step("unpacking", 0);
  const fs::path old_body = scratch / "old.dwarfs", tree = scratch / "tree";
  p.extract_body(old_body);
  fs::create_directories(tree, ec);
  ProcResult r = kg::run({tool.string(), "--tool=dwarfsextract", "-i", old_body.string(), "-o", tree.string(),
                          "--log-level=error"});
  if (!r.ok()) throw std::runtime_error("could not unpack " + pack.filename().string() + ":\n" + r.out);
  fs::remove(old_body, ec);

  // The same tree going back in, by its root, before the pack is touched: a
  // repack is only ever the same game stored another way.
  step("checking", 1);
  const Hash root = Tree::from_directory(m.rooted() ? tree / "game" : tree).root();
  if (root != m.tree.root()) {
    throw std::runtime_error(pack.filename().string() + " unpacks to a tree that is not its own (Merkle root " +
                             to_hex(root) + ", it says " + to_hex(m.tree.root()) + "); it was left as it is");
  }

  step("packing", 2);
  const fs::path new_body = scratch / "new.dwarfs";
  r = kg::run({tool.string(), "--tool=mkdwarfs", "-i", tree.string(), "-o", new_body.string(), "--categorize",
               "-S", "22", "--log-level=error", "--no-progress", "-f"});
  if (!r.ok()) throw std::runtime_error("mkdwarfs failed:\n" + r.out);
  fs::remove_all(tree, ec);

  step("writing", 3);
  m.body.packing = kBodyPacking;
  fs::path next = pack;
  next += ".repack";
  try {
    WriteOptions wo;
    wo.kind = h.kind;
    wo.body = new_body;
    write_pack(next, m, wo);
    Pack::Verification v = Pack::open(next).verify();
    if (!v.ok) throw std::runtime_error("the repacked pack does not verify: " + v.detail);
    if (cb.cancelled && cb.cancelled()) throw Cancelled();
  } catch (...) {
    fs::remove(next, ec);
    throw;
  }
  // A session playing the game has the old pack open, and keeps reading the
  // file it opened; the next one opens this.
  fs::rename(next, pack, ec);
  if (ec) {
    std::error_code e2;
    fs::remove(next, e2);
    throw std::runtime_error("could not put the repacked pack in place: " + ec.message());
  }
  step("done", 4);
}

bool glide_only(const pe::Imports& exe) {
  if (!exe.ok) return false;
  bool glide = exe.imports("glide") || exe.imports("glide2x") || exe.imports("glide3x");
  if (!glide) return false;
  for (const char* d : {"ddraw", "d3d8", "d3d9", "d3drm", "d3dim", "d3dim700", "opengl32", "d3d11", "dxgi"}) {
    if (exe.imports(d)) return false;
  }
  return !exe.direct3d_im;
}

// ---- keys -----------------------------------------------------------------------------

std::vector<install::RegValue> read_fragment(std::string_view text) {
  std::vector<install::RegValue> out;
  std::string hive, key;
  size_t at = 0;
  auto quoted = [](std::string_view s, size_t& i) {
    // s[i] is the opening quote; returns the unescaped contents and leaves i
    // after the closing one.
    std::string o;
    for (++i; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) { o.push_back(s[++i]); continue; }
      if (s[i] == '"') { ++i; break; }
      o.push_back(s[i]);
    }
    return o;
  };
  while (at <= text.size()) {
    size_t nl = text.find('\n', at);
    std::string_view line = text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
    at = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      std::string_view inner = line.substr(1, line.size() - 2);
      size_t bs = inner.find('\\');
      hive = std::string(inner.substr(0, bs));
      key = bs == std::string_view::npos ? std::string() : std::string(inner.substr(bs + 1));
      continue;
    }
    if (hive.empty()) continue;  // REGEDIT4, or anything before the first key
    install::RegValue v;
    v.hive = hive;
    v.key = key;
    size_t i = 0;
    if (line.front() == '@') {
      v.name = "@";
      i = 1;
    } else if (line.front() == '"') {
      v.name = quoted(line, i);
    } else {
      continue;
    }
    if (i >= line.size() || line[i] != '=') continue;
    ++i;
    if (i < line.size() && line[i] == '"') {
      v.type = "sz";
      v.data = quoted(line, i);
    } else {
      std::string_view rhs = line.substr(i);
      size_t colon = rhs.find(':');
      if (colon == std::string_view::npos) continue;
      v.type = std::string(rhs.substr(0, colon));
      v.data = std::string(rhs.substr(colon + 1));
    }
    out.push_back(std::move(v));
  }
  return out;
}

std::optional<install::RegValue> key_in_fragment(std::string_view regedit4, const std::string& vault_key) {
  for (const install::RegValue& v : read_fragment(regedit4)) {
    if (install::is_serial_value(v)) return v;
    if (!vault_key.empty() && v.type == "sz" && v.data == vault_key) return v;
  }
  return std::nullopt;
}

KeySpot suggest_key_spot(std::string_view regedit4, const std::string& vault_key) {
  if (auto k = key_in_fragment(regedit4, vault_key)) return {k->hive + "\\" + k->key, k->name};
  // The game's own key: the one under Software its installer wrote most to,
  // leaving out what Wine and Windows write for every program.
  std::map<std::string, int> count;
  std::string best;
  int best_n = 0;
  for (const install::RegValue& v : read_fragment(regedit4)) {
    std::string k = lower(v.key);
    if (k.rfind("software\\", 0) != 0) continue;
    if (k.rfind("software\\microsoft", 0) == 0 || k.rfind("software\\wine", 0) == 0 ||
        k.rfind("software\\classes", 0) == 0) {
      continue;
    }
    std::string full = v.hive + "\\" + v.key;
    int n = ++count[full];
    // More values wins; on a tie the machine-wide hive, which is where the
    // installers of this era put a key.
    bool hklm = v.hive == install::kHiveLocalMachine;
    bool best_hklm = best.rfind(std::string(install::kHiveLocalMachine), 0) == 0;
    if (n > best_n || (n == best_n && hklm && !best_hklm)) {
      best = full;
      best_n = n;
    }
  }
  return {best, ""};
}

// ---- checks -----------------------------------------------------------------------------

namespace {

bool is_dgvoodoo_file(const std::string& name) {
  std::string n = lower(fs::path(name).filename().string());
  return n.find("dgvoodoo") != std::string::npos;
}

bool tree_has(const Meta& m, std::initializer_list<const char*> names) {
  for (const TreeEntry& e : m.tree.entries()) {
    std::string n = lower(fs::path(e.path).filename().string());
    for (const char* w : names) {
      if (n == w) return true;
    }
  }
  return false;
}

}  // namespace

std::vector<Check> run_checks(const Draft& d, const std::vector<GameFacts>& games) {
  std::vector<Check> out;
  for (size_t i = 0; i < d.games.size() && i < games.size(); ++i) {
    const DraftGame& g = d.games[i];
    const GameFacts& f = games[i];
    const std::string name = called(g);

    if (f.pack) {
      const Meta& m = f.pack->meta;
      // Protection: the wizard's signs, over the whole installed tree rather
      // than one directory, since a pack remembers every file it holds.
      std::vector<std::string> seen;
      for (const TreeEntry& e : m.tree.entries()) {
        std::string file = fs::path(e.path).filename().string();
        std::string what = install::protection_of(file);
        if (what.empty() || std::find(seen.begin(), seen.end(), what) != seen.end()) continue;
        seen.push_back(what);
        out.push_back({"protection:" + lower(what) + ":" + g.id, g.id,
                       name + " carries " + file + ", which is " + what +
                           ". The player gives it a CD-ROM drive made of files, which cannot answer "
                           "the raw-sector read " + what + " makes, so the game may refuse to start.",
                       ""});
      }

      // The author's key.
      if (auto k = key_in_fragment(m.registry.fragment, f.vault_key)) {
        out.push_back({"key-in-pack:" + g.id, g.id,
                       "registry.reg in " + name + "'s pack still holds a key (" + k->hive + "\\" + k->key +
                           "\\" + k->name + "). It would be in every copy of this bundle, readable by "
                           "anyone who has one.",
                       "Install the game again with this kretro, which keeps keys out of the pack."});
      } else if (!f.vault_key.empty() && !g.embed_key) {
        out.push_back({"key-removed:" + g.id, g.id,
                       "Your key for " + name + " was kept out of registry.reg. If the game asks for "
                       "one when it starts, each player types their own.",
                       "Embed my key in this bundle, under " + name + "."});
      }
      if (g.embed_key) {
        out.push_back({"key-embedded:" + g.id, g.id,
                       "Your key for " + name + " will be embedded: it will be in every copy of this "
                       "bundle, and anyone who has one can read it.",
                       "Untick \"Embed my key in this bundle\"."});
      }

      // dgVoodoo.
      bool supplied = std::any_of(g.extra_dlls.begin(), g.extra_dlls.end(),
                                  [](const GameMeta::Dll& x) { return is_dgvoodoo_file(x.name); });
      bool in_tree = tree_has(m, {"dgvoodoo.conf", "dgvoodoocpl.exe"});
      if (supplied || in_tree) {
        out.push_back({"dgvoodoo:" + g.id, g.id,
                       "dgVoodoo is in this bundle for " + name + (in_tree ? ", inside the game's own files" : "") +
                           ". Its licence allows it with one game, as here, but not in a launcher for "
                           "general use; distributing it is on you, not on kretro.",
                       ""});
      } else if (m.runtime.dgvoodoo) {
        out.push_back({"dgvoodoo-missing:" + g.id, g.id,
                       name + " was set up to run under dgVoodoo, which the player does not carry. It "
                       "will run without it.",
                       "Add dgVoodoo's files for this game under Per game."});
      }

      // Glide.
      if (f.imports && glide_only(*f.imports)) {
        out.push_back({"glide:" + g.id, g.id,
                       name + "'s executable draws only through Glide, and the player has no Glide. It "
                       "needs a wrapper for it supplied with the game, or a Direct3D or OpenGL renderer "
                       "chosen in the game's own setup.",
                       ""});
      } else if (f.imports && f.imports->ok && tree_has(m, {"glide.dll", "glide2x.dll", "glide3x.dll"}) &&
                 !f.imports->imports("ddraw") && !f.imports->imports("opengl32") &&
                 !f.imports->imports("d3d8") && !f.imports->imports("d3d9")) {
        // A renderer loaded by name at run time imports nothing; the files are
        // the only evidence, and they are only evidence.
        out.push_back({"glide-maybe:" + g.id, g.id,
                       name + " carries Glide libraries and its executable imports no other renderer: "
                       "it may draw only through Glide, which the player does not have.",
                       ""});
      }
    }

    if (g.cover.empty()) {
      out.push_back({"cover:" + g.id, g.id,
                     name + " has no cover art: its tile shows its name on a colour.",
                     "Pick a cover under Per game."});
    }
  }
  return out;
}

std::vector<std::string> blockers(const Draft& d, const std::vector<GameFacts>& games) {
  std::vector<std::string> out;
  if (d.title.empty()) out.push_back("The bundle has no title.");
  if (!kg::id_is_safe(d.id)) {
    out.push_back("The bundle id '" + d.id + "' cannot be a directory name: letters, digits, '-', '_' and '.' only.");
  }
  if (!version_is_safe(d.version)) {
    out.push_back("The version '" + d.version + "' cannot be part of a file name: letters, digits, '-', '_' and '.' only.");
  }
  if (d.games.empty()) out.push_back("Choose at least one game.");
  std::vector<std::string> ids;
  for (size_t i = 0; i < d.games.size(); ++i) {
    const DraftGame& g = d.games[i];
    const std::string name = called(g);
    if (std::find(ids.begin(), ids.end(), g.id) != ids.end()) out.push_back(name + " is in the bundle twice.");
    ids.push_back(g.id);
    if (g.name.empty()) out.push_back(g.id + " has no name.");
    if (g.id.size() > kNameSize) out.push_back(g.id + " has an id longer than a bundle can name.");
    const GameFacts* f = i < games.size() ? &games[i] : nullptr;
    if (!f || !f->pack) {
      out.push_back(name + " is no longer on the shelf.");
    }
    for (const GameMeta::Dll& x : g.extra_dlls) {
      if (!kg::id_is_safe(x.name)) out.push_back(name + " has an extra file named '" + x.name + "', which cannot be placed.");
    }
    if (g.embed_key) {
      if (!f || f->vault_key.empty()) {
        out.push_back("You asked to embed your key for " + name + ", but the keys vault has none for it: "
                      "kretro key " + g.id + " <key> stores one.");
      }
      if (g.key_path.empty() || g.key_value.empty()) {
        out.push_back("Say where " + name + "'s key goes in the registry: a key path and a value name.");
      }
    }
  }
  if (d.out_dir.empty()) out.push_back("Choose a folder to write the bundle to.");
  return out;
}

bool ready_to_build(const Draft& d, const std::vector<Check>& checks, const std::vector<std::string>& blocking) {
  if (!blocking.empty() || !d.rights) return false;
  return std::all_of(checks.begin(), checks.end(), [&](const Check& c) { return d.acked(c.id); });
}

// ---- size ---------------------------------------------------------------------------------

SizeReport size_report(uint64_t base_bytes, uint64_t meta_bytes, const std::vector<const PackFacts*>& packs) {
  SizeReport r;
  r.runtime = {"runtime", base_bytes, base_bytes};
  r.meta = meta_bytes;
  // build_bundle's layout exactly: the base's payloads, then each payload of
  // ours on a page of its own, then a table of 128 bytes an entry and the
  // trailer - so the number is the one the author will see in a file manager.
  uint64_t entries = 4 + packs.size();  // tools, runtime, app, meta
  uint64_t tail = kTocHeaderSize + kRecordSize * entries + kTrailerSize;
  uint64_t with = align_up(base_bytes) + meta_bytes, without = with;
  for (const PackFacts* p : packs) {
    SizePart s{p->meta.name.empty() ? p->meta.id : p->meta.name, p->bytes, p->without_discs};
    with = align_up(with) + s.bytes;
    without = align_up(without) + s.without_discs;
    r.games.push_back(std::move(s));
  }
  r.total = with + tail;
  r.total_without_discs = without + tail;
  auto gib = [](uint64_t n) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f GiB", static_cast<double>(n) / (1ull << 30));
    return std::string(b);
  };
  if (r.total >= kWarn4G) {
    r.warnings.push_back("It is " + gib(r.total) + ", over 4 GiB: it cannot be copied to a FAT32 drive, "
                         "and GitHub Releases and itch.io's web upload will refuse it.");
  } else if (r.total >= kWarn2G) {
    r.warnings.push_back("It is " + gib(r.total) + ", over 2 GiB: GitHub Releases and itch.io's web upload "
                         "will refuse it.");
  }
  if (!r.warnings.empty() && r.total_without_discs < r.total) {
    uint64_t limit = r.total >= kWarn4G ? kWarn4G : kWarn2G;
    if (r.total_without_discs < limit) {
      r.warnings.push_back("Without their discs the games would come to about " + gib(r.total_without_discs) +
                           ": installing them again without \"include the discs\" would bring it under.");
    }
  }
  return r;
}

// ---- building -----------------------------------------------------------------------------

uint64_t player_base_bytes(const BaseSource& b) {
  std::error_code ec;
  uint64_t whole = fs::file_size(b.path, ec);
  if (ec) throw std::runtime_error("cannot open " + b.path.string() + ": " + ec.message());
  Toc t = read_toc(b.path, b.off, b.len ? *b.len : whole - std::min(b.off, whole));
  uint64_t end = 0;
  for (const Entry& e : t.entries) end = std::max(end, e.off + e.len);
  return end;
}

BaseSource find_player_base(const fs::path& self) {
  if (const char* p = std::getenv("KRETRO_PLAYER_BASE"); p && *p) {
    // A player base on its own, as `make player-base` links one: the whole file.
    return BaseSource{fs::path(p), 0, std::nullopt, std::nullopt};
  }
  static const std::string how =
      " A development build is linked without one when build/player-base is missing: run "
      "'make player-base', then 'make', and use the build/kretro that makes - or point "
      "KRETRO_PLAYER_BASE at a player base.";
  if (self.empty()) {
    throw std::runtime_error("kretro cannot find its own file (KRETRO_SELF is not set), so it has no player "
                             "base to build from: it was started outside its bootstrap." + how);
  }
  Toc t;
  try {
    t = read_toc(self);
  } catch (const std::exception& ex) {
    throw std::runtime_error("kretro cannot read its own file " + self.string() + " (" + ex.what() +
                             "), so it has no player base to build from." + how);
  }
  if (!t.find(Kind::PlayerBase)) {
    throw std::runtime_error("This kretro carries no player base, so it cannot build players." + how);
  }
  return extract_player_base(self);
}

std::string output_name(const Draft& d) { return d.id + "-" + d.version + ".run"; }

BundleMeta meta_from_draft(const Draft& d, const std::vector<install::StoredKey>& keys,
                           const std::string& built_at, const std::vector<std::string>& licenses,
                           const std::map<std::string, bool>& exe64) {
  BundleMeta m;
  m.id = d.id;
  m.title = d.title;
  m.version = d.version;
  m.built_at = built_at;
#ifdef KRETRO_VERSION
  m.kretro_version = KRETRO_VERSION;
#else
  m.kretro_version = "dev";
#endif
  m.banner = d.banner;
  m.icon = d.icon;
  m.rights_acknowledged = d.rights;
  m.licenses = licenses;
  for (const DraftGame& g : d.games) {
    GameMeta gm;
    gm.id = g.id;
    gm.name = g.name;
    gm.year = g.year;
    gm.cover = g.cover;
    gm.backend = g.backend;
    gm.needs_gpu = g.needs_gpu;
    gm.display = g.display;
    gm.fullscreen = g.fullscreen;
    gm.gamepad = g.gamepad;
    gm.extra_dlls = g.extra_dlls;
    if (g.embed_key) {
      std::string v = install::key_for(keys, g.id);
      if (v.empty()) throw std::runtime_error("the keys vault has no key for " + g.id + " to embed");
      auto bits = exe64.find(g.id);
      const bool is64 = bits != exe64.end() && bits->second;
      gm.key = GameMeta::Key{v, g.key_path, g.key_value, is64 ? "64" : "32"};
    }
    m.games.push_back(std::move(gm));
  }
  return m;
}

std::vector<std::string> runtime_licenses(const fs::path& root) {
  std::vector<std::string> out;
  if (root.empty()) return out;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(root / "usr/share/kretro/licenses", ec)) {
    std::string n = de.path().filename().string();
    if (n == "common-licenses") continue;  // Debian's full texts, which the others point into
    out.push_back(n);
  }
  std::sort(out.begin(), out.end());
  return out;
}

namespace {

// `len` bytes at `off` in `from`, into a new file `to`. copy_file_range lets a
// filesystem that can share blocks share them, so a copy of a 300 MB runtime
// out of a player base costs next to nothing on btrfs or XFS - the entries are
// page-aligned, which is what a shared extent needs - and a kernel-side copy
// elsewhere.
bool copy_range(const fs::path& from, uint64_t off, uint64_t len, const fs::path& to) {
  int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return false;
  int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    ::close(in);
    return false;
  }
  loff_t src = static_cast<loff_t>(off);
  uint64_t left = len;
  bool ok = true;
  while (left > 0) {
    ssize_t n = ::copy_file_range(in, &src, out, nullptr, left, 0);
    if (n <= 0) {
      // No copy_file_range here (an old kernel, a filesystem that refuses it
      // across files): the ordinary way, from where it stopped.
      std::vector<char> buf(1 << 20);
      while (left > 0) {
        ssize_t r = ::pread(in, buf.data(), std::min<uint64_t>(left, buf.size()), src);
        if (r <= 0 || ::write(out, buf.data(), static_cast<size_t>(r)) != r) {
          ok = false;
          break;
        }
        src += r;
        left -= static_cast<uint64_t>(r);
      }
      break;
    }
    left -= static_cast<uint64_t>(n);
  }
  ::close(in);
  if (::close(out) != 0) ok = false;
  return ok;
}

}  // namespace

std::vector<std::string> base_licenses(const BaseSource& b, const fs::path& tool, const fs::path& cache,
                                       bool compute) {
  std::vector<std::string> out;
  std::error_code ec;
  Toc t;
  try {
    uint64_t whole = fs::file_size(b.path, ec);
    if (ec) return out;
    t = read_toc(b.path, b.off, b.len ? *b.len : whole - std::min(b.off, whole));
  } catch (const std::exception&) {
    return out;
  }
  const Entry* rt = t.find(Kind::Runtime);
  if (!rt || cache.empty()) return out;
  const fs::path memo = cache / ("player-licenses-" + to_hex(rt->blake3) + ".txt");
  if (std::ifstream f{memo}) {
    for (std::string line; std::getline(f, line);) {
      if (!line.empty()) out.push_back(line);
    }
    return out;
  }
  if (!compute || tool.empty() || !fs::exists(tool, ec)) return out;

  fs::create_directories(cache, ec);
  const fs::path image = cache / ("player-runtime-" + to_hex(rt->blake3) + ".image");
  if (!copy_range(b.path, t.at(*rt), rt->len, image)) {
    fs::remove(image, ec);
    return out;
  }
  ProcResult r = kg::run({tool.string(), "--tool=dwarfsck", "-i", image.string(), "-l", "--log-level=error"});
  fs::remove(image, ec);
  if (!r.ok()) return out;
  const std::string dir = "usr/share/kretro/licenses/";
  std::istringstream in(r.out);
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.rfind(dir, 0) != 0) continue;
    std::string n = line.substr(dir.size());
    // Only what is directly in the directory; common-licenses is Debian's
    // full texts, which the others point into, as runtime_licenses skips it.
    if (n.empty() || n.find('/') != std::string::npos || n == "common-licenses") continue;
    out.push_back(n);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  if (out.empty()) return out;
  const fs::path tmp = fs::path(memo).concat(".new");
  {
    std::ofstream f(tmp, std::ios::trunc);
    for (const std::string& n : out) f << n << "\n";
  }
  fs::rename(tmp, memo, ec);
  if (ec) fs::remove(tmp, ec);
  return out;
}

Built build_from_draft(const Draft& d, const BuildInputs& in, const Callbacks& cb) {
  if (!d.rights) throw std::runtime_error("tick \"I have the right to distribute these games\" first");
  // The id is half of the output's name, joined onto the chosen folder: one
  // with a '/' or a '..' in it would write the player somewhere else.
  if (!kg::id_is_safe(d.id)) throw std::runtime_error("the bundle id '" + d.id + "' cannot be part of a file name");
  if (!version_is_safe(d.version)) throw std::runtime_error("the version '" + d.version + "' cannot be part of a file name");
  if (d.out_dir.empty()) throw std::runtime_error("choose a folder to write the bundle to");
  std::error_code ec;
  if (!fs::is_directory(d.out_dir, ec)) throw std::runtime_error(d.out_dir + " is not a folder");

  std::vector<fs::path> packs;
  for (const DraftGame& g : d.games) {
    fs::path p = pack_for(in.games_dir, g.id);
    if (p.empty()) throw std::runtime_error(called(g) + " is no longer on the shelf");
    packs.push_back(p);
  }
  BaseSource base = in.base.empty() ? find_player_base(in.self) : BaseSource{in.base, 0, std::nullopt, std::nullopt};

  char when[32] = {};
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);

  // Which half of the registry each embedded key is for: the game's own
  // executable says, in its PE header.
  std::map<std::string, bool> exe64;
  for (size_t i = 0; i < d.games.size(); ++i) {
    if (!d.games[i].embed_key || in.tool.empty()) continue;
    try {
      pe::Imports im = read_exe_imports(read_pack_facts(packs[i]), in.tool,
                                        (in.cache.empty() ? fs::path(d.out_dir) : in.cache) / ".exe-scratch");
      if (im.ok) exe64[d.games[i].id] = im.is64;
    } catch (const std::exception&) {
    }
  }

  BundleMeta meta = meta_from_draft(d, install::load_keys(in.keys_file), when,
                                    base_licenses(base, in.tool, in.cache), exe64);
  return build_bundle(base, meta, packs, in.out.empty() ? fs::path(d.out_dir) / output_name(d) : in.out, cb);
}

}  // namespace kg::bundle
