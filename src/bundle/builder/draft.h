// The author's bundle as the author left it, and the name it goes by.
//
// A Draft is every field of every step of the Bundles page, the checks already
// acknowledged and where the last build went. draft_store.h remembers it; the
// bytes it is remembered as are encode_draft's, and their key order is part of
// the format.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../meta.h"

namespace kg::bundle {

// ---- identity ---------------------------------------------------------------

// "Retro Shelf Classics" -> "retro-shelf-classics": the rule the wizard names
// games by, held to the one-component rule a bundle id must pass, and capped,
// because the id is also half of the output file's name. A title with nothing
// in it that can be a name gives "bundle" rather than nothing.
std::string id_from_title(std::string_view title);

// Whether a version can sit in a file name after the id: the same characters
// an id may have, and short.
bool version_is_safe(std::string_view v);

// ---- the draft --------------------------------------------------------------

struct DraftGame {
  std::string id;               // the pack on the shelf this game is
  std::string name;
  uint32_t year = 0;
  std::string cover;            // PNG bytes; empty for none
  std::string cover_from;       // where it was picked, to say on the page
  std::string backend = "auto";
  bool needs_gpu = false;
  std::string display = "integer";
  bool fullscreen = false;
  std::string gamepad;          // empty: kretro's own map
  std::vector<GameMeta::Dll> extra_dlls;
  // Off unless the author ticked it, past the warning. The key itself is never
  // remembered here: it is read out of the keys vault at build time, so the
  // one copy of it on this machine stays the vault's.
  bool embed_key = false;
  std::string key_path;         // HKEY_LOCAL_MACHINE\Software\...
  std::string key_value;        // the value name under it

  // The name the page uses for the game in a sentence: its name, or its id
  // when it has none.
  std::string display_name() const;

  bool operator==(const DraftGame&) const = default;
};

struct Draft {
  std::string id;
  // The id follows the title until the author types one of their own.
  bool id_typed = false;
  // Marked by the author once a build has gone out. From then on the id is
  // fixed: every player's saves live under it, and a new id is a new bundle
  // whose players cannot find the old one's saves.
  bool published = false;
  std::string title;
  std::string version = "1.0";
  std::string banner, icon;     // PNG bytes
  std::string banner_from, icon_from;
  std::vector<DraftGame> games;
  bool rights = false;
  std::vector<std::string> acknowledged;  // Check::id of each one waved through
  std::string out_dir;
  // The last build, to say on the page and to preview.
  std::string last_built;
  uint64_t last_size = 0;
  std::string last_built_at;

  bool acked(std::string_view check_id) const;
  bool operator==(const Draft&) const = default;
};

inline constexpr uint64_t kDraftFormat = 1;

// In draft_codec.cpp.
std::string encode_draft(const Draft& d);
// Throws std::runtime_error naming what is wrong. Keys it does not know are
// skipped, as bundle.meta's are, so an older kretro opens a newer one's draft.
Draft decode_draft(std::string_view cbor);

}  // namespace kg::bundle
