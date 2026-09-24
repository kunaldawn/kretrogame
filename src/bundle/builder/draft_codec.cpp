// The draft's CBOR: what <state>/bundles/<id>.cbor holds. The key order and
// which keys are left out when empty are the format, pinned by a golden in
// test_builder; change neither here.
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../util/cbor.h"
#include "draft.h"

namespace kg::bundle {

namespace {

// ---- the draft's CBOR ------------------------------------------------------------

using cbor::Value;

[[noreturn]] void bad(const std::string& why) { throw std::runtime_error("remembered bundle: " + why); }

const Value* field(const Value& m, std::string_view key, Value::Type t, const char* word) {
  const Value* v = m.find(key);
  if (!v) return nullptr;
  if (v->type != t) bad("'" + std::string(key) + "' should be " + word);
  return v;
}
std::string text(const Value& m, std::string_view key, const std::string& def = "") {
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

}  // namespace kg::bundle
