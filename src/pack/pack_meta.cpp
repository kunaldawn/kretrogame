#include "pack_meta.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "../util/cbor.h"
#include "../util/safe_names.h"

namespace kg {

namespace {

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

// One game's map, inside a set's "games". Its discs are named by key; the set
// lists each disc once.
void encode_game(cbor::Encoder& e, const Meta& m) {
  const Meta::Recipe& recipe = m.recipe;
  const Meta::Run& run = m.run;
  const Meta::Runtime& runtime = m.runtime;
  const Meta::Present& present = m.present;
  e.map(14);

  e.text("id"); e.text(m.id);
  e.text("name"); e.text(m.name);
  e.text("year"); e.uint_val(m.year);

  e.text("who");
  e.map(2);
  e.text("developer"); e.text(m.developer);
  e.text("publisher"); e.text(m.publisher);

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

  e.text("install");
  e.map(1);
  e.text("install_dir"); e.text(m.install.install_dir);

  e.text("registry");
  e.map(1);
  e.text("fragment"); e.text(m.registry.fragment);

  e.text("system");
  e.map(2);
  e.text("files"); e.uint_val(m.system.files);
  e.text("bytes"); e.uint_val(m.system.bytes);

  e.text("input");
  e.map(m.input.size());
  for (const auto& kv : m.input) { e.text(kv.first); e.text(kv.second); }

  // The game's own "discs" are keys into the set's "discs2", in drive order.
  // The recipe map has a "discs" too, of references; the two never share a map.
  e.text("discs");
  e.array(m.discs.size());
  for (const Meta::Disc& d : m.discs) e.text(d.key);

  // The tree travels as its canonical text. It is the hash input, so shipping
  // anything else would mean two representations that could disagree.
  e.text("tree"); e.text(m.tree.canonical());
}

void encode_disc(cbor::Encoder& e, const Meta::Disc& d) {
  e.map(6);
  e.text("key"); e.text(d.key);
  e.text("label"); e.text(d.label);
  e.text("serial"); e.uint_val(d.serial);
  e.text("ref"); e.text(d.ref);
  e.text("source"); e.text(d.source);
  e.text("bytes"); e.uint_val(d.bytes);
}

// One game's map. `set_discs` is the set's list, which the game's keys must
// all be in: a game naming a disc its pack does not carry would be handed a
// drive that is not there.
Meta decode_game(const cbor::Value& v, const std::vector<Meta::Disc>& set_discs) {
  if (!v.is_map()) throw std::runtime_error("a game in this pack is not a map");
  Meta m;
  m.id = v.find("id") ? v.find("id")->text_or() : "";
  // The id is a path component on whoever's machine opens this: it names
  // games/<id>.kgpack, saves/<id> and prefixes/<id>. A pack is a file people
  // send each other, so an id of "../../../.config/autostart" would let the
  // sender pick the directory as well as the name - on import, on install and
  // on play. The pack is refused whole rather than sanitised,
  // because a pack that has to be corrected to be safe is not one to trust with
  // an executable either.
  if (!id_is_safe(m.id)) {
    throw std::runtime_error("this pack calls itself " + as_shown(m.id) +
                             ", which is not a name a game can have here");
  }
  m.name = v.find("name") ? v.find("name")->text_or() : "";
  m.year = static_cast<uint32_t>(v.find("year") ? v.find("year")->uint_or() : 0);

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
  // puts a symlink in its place (session/play.cpp). "../../../.bashrc" is
  // therefore a pack that deletes a file in the importer's home the first time
  // the game is started.
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
    for (const auto& kv : im->map) m.input[kv.key.text_or()] = kv.value.text_or();
  }

  for (const std::string& key : decode_strings(v.find("discs"))) {
    auto it = std::find_if(set_discs.begin(), set_discs.end(), [&](const Meta::Disc& d) { return d.key == key; });
    if (it == set_discs.end()) {
      throw std::runtime_error("game " + as_shown(m.id) + " names disc " + as_shown(key) +
                               ", which this pack does not list");
    }
    m.discs.push_back(*it);
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

  if (const cbor::Value* t = v.find("tree"); t && t->is_text()) {
    m.tree = Tree::from_canonical(t->s);
  }

  return m;
}

}  // namespace

std::string SetMeta::encode() const {
  cbor::Encoder e;
  e.map(4);
  e.text("set_id"); e.text(set_id);

  e.text("discs2");
  e.array(discs.size());
  for (const Meta::Disc& d : discs) encode_disc(e, d);

  // Sorted, so the bytes are the set's and not the order its games came in.
  std::vector<const Meta*> sorted;
  sorted.reserve(games.size());
  for (const Meta& g : games) sorted.push_back(&g);
  std::sort(sorted.begin(), sorted.end(), [](const Meta* a, const Meta* b) { return a->id < b->id; });
  e.text("games");
  e.array(sorted.size());
  for (const Meta* g : sorted) encode_game(e, *g);

  e.text("body");
  // A key only when there is something to say, so a body that says nothing
  // about its packing has no packing key at all.
  e.map(body.packing.empty() ? 2 : 3);
  e.text("length"); e.uint_val(body.length);
  e.text("blake3"); e.bytes(body.blake3.data(), body.blake3.size());
  if (!body.packing.empty()) { e.text("packing"); e.text(body.packing); }
  return e.take();
}

SetMeta SetMeta::decode(std::string_view data) {
  cbor::Value v = cbor::decode(data);
  if (!v.is_map()) throw std::runtime_error("kgpack metadata is not a map");

  SetMeta s;
  s.set_id = v.find("set_id") ? v.find("set_id")->text_or() : "";
  // Like a game's id, the set's names a file on whoever's machine opens it:
  // packs/<set_id>.kgpack, and the unpacked copy beside the cache.
  if (!id_is_safe(s.set_id)) {
    throw std::runtime_error("this pack's set is called " + as_shown(s.set_id) +
                             ", which is not a name a set can have here");
  }

  if (const cbor::Value* r = v.find("body")) {
    if (const cbor::Value* x = r->find("length")) s.body.length = x->uint_or();
    s.body.blake3 = decode_hash(r->find("blake3"));
    if (const cbor::Value* x = r->find("packing")) s.body.packing = x->text_or("");
  }

  if (const cbor::Value* ds = v.find("discs2"); ds && ds->is_array()) {
    for (const cbor::Value& d : ds->arr) {
      Meta::Disc di;
      if (const cbor::Value* x = d.find("key")) di.key = x->text_or();
      if (const cbor::Value* x = d.find("label")) di.label = x->text_or();
      if (const cbor::Value* x = d.find("serial")) di.serial = static_cast<uint32_t>(x->uint_or());
      if (const cbor::Value* x = d.find("ref")) di.ref = x->text_or();
      if (const cbor::Value* x = d.find("source")) di.source = x->text_or();
      if (const cbor::Value* x = d.find("bytes")) di.bytes = x->uint_or();
      // The key is a directory in the body, and a path on the machine that
      // unpacks it: the same fence as an id.
      if (!id_is_safe(di.key)) {
        throw std::runtime_error("disc " + as_shown(di.key) + " is not a name a disc can have here");
      }
      for (const Meta::Disc& seen : s.discs) {
        if (seen.key == di.key) throw std::runtime_error("disc " + as_shown(di.key) + " is listed twice");
      }
      s.discs.push_back(std::move(di));
    }
  }

  if (const cbor::Value* gs = v.find("games"); gs && gs->is_array()) {
    for (const cbor::Value& g : gs->arr) {
      Meta m = decode_game(g, s.discs);
      if (s.find(m.id)) throw std::runtime_error("game " + as_shown(m.id) + " is in this pack twice");
      m.body = s.body;
      s.games.push_back(std::move(m));
    }
  }
  if (s.games.empty()) throw std::runtime_error("this pack holds no games");
  return s;
}

const Meta* SetMeta::find(std::string_view id) const {
  for (const Meta& g : games) {
    if (g.id == id) return &g;
  }
  return nullptr;
}

Hash SetMeta::root() const {
  std::vector<std::pair<std::string, Hash>> roots;
  roots.reserve(games.size());
  for (const Meta& g : games) roots.emplace_back(g.id, g.tree.root());
  std::sort(roots.begin(), roots.end());
  Hasher h;
  for (const auto& r : roots) h.update(r.second.data(), r.second.size());
  return h.finish();
}

std::string disc_key(uint64_t size, const Hash& prefix) {
  std::string in;
  for (int i = 0; i < 8; ++i) in.push_back(static_cast<char>((size >> (8 * i)) & 0xff));
  in.append(reinterpret_cast<const char*>(prefix.data()), prefix.size());
  return to_hex(hash_string(in)).substr(0, 16);
}

std::string set_id_for(std::vector<std::string> keys, std::string_view game_id) {
  std::string in;
  if (keys.empty()) {
    in = "game:" + std::string(game_id);
  } else {
    std::sort(keys.begin(), keys.end());
    for (const std::string& k : keys) in += k + "\n";
  }
  return "s-" + to_hex(hash_string(in)).substr(0, 16);
}

std::filesystem::path body_game_dir(std::string_view id) { return std::filesystem::path("games") / std::string(id); }
std::filesystem::path body_disc_dir(std::string_view key) { return std::filesystem::path("discs") / std::string(key); }

SetMeta set_of(Meta m) {
  SetMeta s;
  std::vector<std::string> keys;
  keys.reserve(m.discs.size());
  for (const Meta::Disc& d : m.discs) keys.push_back(d.key);
  s.set_id = set_id_for(keys, m.id);
  s.discs = m.discs;
  s.body = m.body;
  s.games.push_back(std::move(m));
  return s;
}

}  // namespace kg
