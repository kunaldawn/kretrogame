#include "meta.h"

#include <algorithm>
#include <stdexcept>

#include "../util/cbor.h"
#include "../util/safe_names.h"
#include "toc.h"

namespace kg::bundle {
namespace {

using cbor::Value;

[[noreturn]] void bad(const std::string& why) { throw std::runtime_error("bundle.meta: " + why); }

const char* type_name(const Value& v) {
  switch (v.type) {
    case Value::Type::Uint: return "an integer";
    case Value::Type::Int: return "a negative integer";
    case Value::Type::Bytes: return "bytes";
    case Value::Type::Text: return "text";
    case Value::Type::Array: return "an array";
    case Value::Type::Map: return "a map";
    case Value::Type::Bool: return "a boolean";
    case Value::Type::Null: return "null";
  }
  return "something else";
}

// One map being read, and the words for where it is: "game 2 (example-game)".
struct Fields {
  const Value& map;
  std::string where;

  const Value* want(std::string_view key, Value::Type type, const char* type_word, bool required) const {
    const Value* v = map.find(key);
    if (!v) {
      if (required) bad(where + " has no '" + std::string(key) + "'");
      return nullptr;
    }
    if (v->type != type) {
      bad("'" + std::string(key) + "' of " + where + " should be " + type_word + ", found " + type_name(*v));
    }
    return v;
  }

  std::string text(std::string_view key, bool required = false, const std::string& def = "") const {
    const Value* v = want(key, Value::Type::Text, "text", required);
    return v ? v->s : def;
  }
  std::string bytes(std::string_view key) const {
    const Value* v = want(key, Value::Type::Bytes, "bytes", false);
    return v ? v->s : std::string();
  }
  bool boolean(std::string_view key) const {
    const Value* v = want(key, Value::Type::Bool, "a boolean", false);
    return v ? v->b : false;
  }
  uint64_t uint(std::string_view key, bool required = false) const {
    const Value* v = want(key, Value::Type::Uint, "a whole number", required);
    return v ? v->u : 0;
  }
  const Value* array(std::string_view key) const { return want(key, Value::Type::Array, "an array", false); }
  const Value* submap(std::string_view key) const { return want(key, Value::Type::Map, "a map", false); }
};

void require_map(const Value& v, const std::string& where) {
  if (!v.is_map()) bad(where + " should be a map, found " + type_name(v));
}

template <size_t N>
bool one_of(std::string_view s, const std::array<std::string_view, N>& set) {
  return std::find(set.begin(), set.end(), s) != set.end();
}

std::string game_where(size_t i, const std::string& id) {
  std::string s = "game " + std::to_string(i + 1);
  if (!id.empty()) s += " (" + id + ")";
  return s;
}

}  // namespace

void BundleMeta::validate() const {
  if (format != kMetaFormat) bad("format " + std::to_string(format) + " is not one this build writes or reads");
  // The id is the name of the player's state directory, and so of every save
  // anybody makes with it: the same one-component rule a pack's id is held to.
  if (!kg::id_is_safe(id)) bad("the bundle id '" + id + "' is not a name a directory can have");
  if (title.empty()) bad("the bundle has no title");
  if (games.empty()) bad("the bundle carries no games");
  std::vector<std::string_view> seen;
  for (size_t i = 0; i < games.size(); ++i) {
    const GameMeta& g = games[i];
    std::string where = game_where(i, g.id);
    // The table of contents names each pack by this id in 64 bytes.
    if (!kg::id_is_safe(g.id) || g.id.size() > kNameSize) bad(where + " has an id that cannot be used here");
    if (std::find(seen.begin(), seen.end(), g.id) != seen.end()) bad("two games are called " + g.id);
    seen.push_back(g.id);
    if (g.name.empty()) bad(where + " has no name");
    if (!one_of(g.backend, kBackendNames)) bad(where + " asks for graphics backend '" + g.backend + "', which is not one");
    if (!one_of(g.display, kDisplayModes)) bad(where + " asks for display mode '" + g.display + "', which is not one");
    for (const GameMeta::Dll& d : g.extra_dlls) {
      // Written into the game's own directory by name: one plain component.
      if (!kg::id_is_safe(d.name)) bad(where + " carries a file named '" + d.name + "', which cannot be placed");
    }
    if (g.key && g.key->value.empty()) bad(where + " embeds an empty key");
    if (g.key && !g.key->view.empty() && g.key->view != "32" && g.key->view != "64") {
      bad(where + " puts its key in registry view '" + g.key->view + "', which is neither 32 nor 64");
    }
  }
}

namespace {

// A game's embedded key, when it has one. Its own function because
// clang-tidy's optional-access analysis gives up on one as long as encode()
// and then cannot see the check before the access.
void encode_key(cbor::Encoder& e, const std::optional<GameMeta::Key>& key) {
  if (!key) return;
  e.text("key");
  e.map(key->view.empty() ? 3 : 4);
  e.text("value"); e.text(key->value);
  e.text("registry_path"); e.text(key->registry_path);
  e.text("registry_value"); e.text(key->registry_value);
  if (!key->view.empty()) { e.text("view"); e.text(key->view); }
}

}  // namespace

std::string BundleMeta::encode() const {
  cbor::Encoder e;
  size_t fields = 9 + (banner.empty() ? 0 : 1) + (icon.empty() ? 0 : 1);
  e.map(fields);
  e.text("format"); e.uint_val(format);
  e.text("id"); e.text(id);
  e.text("title"); e.text(title);
  e.text("version"); e.text(version);
  e.text("built_at"); e.text(built_at);
  e.text("kretro_version"); e.text(kretro_version);
  if (!banner.empty()) { e.text("banner"); e.bytes(banner.data(), banner.size()); }
  if (!icon.empty()) { e.text("icon"); e.bytes(icon.data(), icon.size()); }
  e.text("rights_acknowledged"); e.boolean(rights_acknowledged);
  e.text("licenses");
  e.array(licenses.size());
  for (const std::string& l : licenses) e.text(l);

  e.text("games");
  e.array(games.size());
  for (const GameMeta& g : games) {
    size_t n = 8 + (g.cover.empty() ? 0 : 1) + (g.gamepad.empty() ? 0 : 1) + (g.key ? 1 : 0);
    e.map(n);
    e.text("id"); e.text(g.id);
    e.text("name"); e.text(g.name);
    e.text("year"); e.uint_val(g.year);
    if (!g.cover.empty()) { e.text("cover"); e.bytes(g.cover.data(), g.cover.size()); }
    e.text("backend"); e.text(g.backend);
    e.text("needs_gpu"); e.boolean(g.needs_gpu);
    e.text("display"); e.text(g.display);
    e.text("fullscreen"); e.boolean(g.fullscreen);
    if (!g.gamepad.empty()) { e.text("gamepad"); e.text(g.gamepad); }
    e.text("extra_dlls");
    e.array(g.extra_dlls.size());
    for (const GameMeta::Dll& d : g.extra_dlls) {
      e.map(2);
      e.text("name"); e.text(d.name);
      e.text("data"); e.bytes(d.data.data(), d.data.size());
    }
    encode_key(e, g.key);
  }
  return e.take();
}

BundleMeta BundleMeta::decode(std::string_view data) {
  Value root;
  try {
    root = cbor::decode(data);
  } catch (const std::exception& ex) {
    bad(ex.what());
  }
  require_map(root, "the top level");
  Fields top{root, "the bundle"};

  BundleMeta m;
  m.format = top.uint("format", true);
  // Refused before anything else is read: a newer format may have changed what
  // the fields below mean, and reading it on a guess is how a player shows the
  // wrong game.
  if (m.format != kMetaFormat) {
    bad("format " + std::to_string(m.format) + " is newer than this player understands");
  }
  m.id = top.text("id", true);
  m.title = top.text("title", true);
  m.version = top.text("version");
  m.built_at = top.text("built_at");
  m.kretro_version = top.text("kretro_version");
  m.banner = top.bytes("banner");
  m.icon = top.bytes("icon");
  m.rights_acknowledged = top.boolean("rights_acknowledged");
  if (const Value* ls = top.array("licenses")) {
    for (size_t i = 0; i < ls->arr.size(); ++i) {
      const Value& l = ls->arr[i];
      if (!l.is_text()) bad("licence " + std::to_string(i + 1) + " should be text, found " + type_name(l));
      m.licenses.push_back(l.s);
    }
  }

  const Value* gs = top.want("games", Value::Type::Array, "an array", true);
  for (size_t i = 0; i < gs->arr.size(); ++i) {
    std::string where = game_where(i, "");
    const Value& gv = gs->arr[i];
    require_map(gv, where);
    Fields f{gv, where};
    GameMeta g;
    g.id = f.text("id", true);
    f.where = game_where(i, g.id);
    g.name = f.text("name", true);
    uint64_t year = f.uint("year");
    if (year > UINT32_MAX) bad("'year' of " + f.where + " is " + std::to_string(year) + ", which is not a year");
    g.year = static_cast<uint32_t>(year);
    g.cover = f.bytes("cover");
    g.backend = f.text("backend", false, "auto");
    g.needs_gpu = f.boolean("needs_gpu");
    g.display = f.text("display", false, "integer");
    g.fullscreen = f.boolean("fullscreen");
    g.gamepad = f.text("gamepad");
    if (const Value* ds = f.array("extra_dlls")) {
      for (size_t k = 0; k < ds->arr.size(); ++k) {
        std::string dwhere = "extra file " + std::to_string(k + 1) + " of " + f.where;
        const Value& dv = ds->arr[k];
        require_map(dv, dwhere);
        Fields df{dv, dwhere};
        g.extra_dlls.push_back({df.text("name", true), df.want("data", Value::Type::Bytes, "bytes", true)->s});
      }
    }
    if (const Value* k = f.submap("key")) {
      Fields kf{*k, "the key of " + f.where};
      g.key = GameMeta::Key{kf.text("value", true), kf.text("registry_path"), kf.text("registry_value"),
                            kf.text("view")};
    }
    m.games.push_back(std::move(g));
  }

  m.validate();
  return m;
}

}  // namespace kg::bundle
