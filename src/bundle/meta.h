// bundle.meta: what a player says about itself and its games.
//
// It is the one payload that makes a kretro file a player, and it is CBOR, a
// map with text keys, because it is read only by the player app - which has a
// decoder - and because a newer builder will add to it. Keys this build does
// not know are skipped. Keys it does know must hold the type they are defined
// to hold; a year that arrives as text is refused with a message naming it,
// not read as zero, because a player that quietly shows the wrong thing is
// worse than one that says which field is wrong.
//
// Absent optional things are empty: an empty banner, icon or cover is no
// picture, an empty gamepad is the default map, and a key only exists when the
// author opted in to embedding one.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kg::bundle {

inline constexpr uint64_t kMetaFormat = 1;

// The graphics backends and display modes a game may ask for, in the order the
// Bundles page offers them. validate() refuses anything else.
inline constexpr std::array<std::string_view, 5> kBackendNames = {"auto", "dxvk", "wined3d-vk", "wined3d-gl", "cnc-ddraw"};
inline constexpr std::array<std::string_view, 3> kDisplayModes = {"integer", "fit", "native"};

struct GameMeta {
  std::string id;
  std::string name;
  uint32_t year = 0;
  // The set the game plays from: the name of the table entry whose pack holds
  // it. A disc of three games is one pack in the file, and three games name it.
  std::string set;
  std::string cover;              // PNG bytes; empty for none
  std::string backend = "auto";   // auto | dxvk | wined3d-vk | wined3d-gl | cnc-ddraw
  bool needs_gpu = false;
  std::string display = "integer";  // integer | fit | native
  bool fullscreen = false;
  std::string gamepad;            // a default map, as format_gamepad writes it; empty for kretro's own

  // Files the author supplied for this one game - dgVoodoo's, for instance,
  // which may be shipped by an author but never by us.
  struct Dll {
    std::string name;
    std::string data;
    bool operator==(const Dll&) const = default;
  };
  std::vector<Dll> extra_dlls;

  // Only when the author ticked "embed my key in this bundle", past the warning
  // that it will be in every copy and readable by anyone.
  //
  // `view` is which half of the registry the game reads: "32" or "64". A
  // player's prefix is 64-bit Windows under new WoW64, where a 32-bit
  // program asking for HKLM\Software\X is handed HKLM\Software\Wow6432Node\X;
  // a key put at the path as written would be in the half the game never
  // looks at. The builder reads the game executable's PE header and says
  // which; the player puts the key where that view will find it. Empty - a
  // meta from before the field - means the path is used exactly as written.
  struct Key {
    std::string value;
    std::string registry_path;
    std::string registry_value;
    std::string view;
    bool operator==(const Key&) const = default;
  };
  std::optional<Key> key;

  bool operator==(const GameMeta&) const = default;
};

struct BundleMeta {
  uint64_t format = kMetaFormat;
  std::string id;       // names the state directory, so it outlives versions
  std::string title;
  std::string version;
  std::string built_at;
  std::string kretro_version;
  std::string banner;   // PNG bytes; empty for none
  std::string icon;     // PNG bytes; empty for none
  bool rights_acknowledged = false;
  std::vector<std::string> licenses;
  std::vector<GameMeta> games;

  // Throws std::runtime_error naming the first thing that is wrong: an id that
  // cannot be a directory name, a game without a name, a backend or display
  // mode nobody implements, two games with one id.
  void validate() const;

  std::string encode() const;
  // Decodes and validates. Throws std::runtime_error.
  static BundleMeta decode(std::string_view cbor);

  bool operator==(const BundleMeta&) const = default;
};

}  // namespace kg::bundle
