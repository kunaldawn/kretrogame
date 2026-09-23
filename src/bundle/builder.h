// What the Bundles page decides, with the page taken away.
//
// The page is ImGui and SDL, and a test binary cannot link either; everything
// on it that could be quietly wrong - the id a title turns into, what is
// remembered and what comes back, which checks a game raises, how big the
// file will be, which backend "auto" means, what a preview is allowed to
// inherit - is here instead, as functions of plain data. The page draws a
// Draft, edits it, and hands it to build_from_draft on a worker thread; that
// is the same call the tests make.
//
// A Draft is the author's bundle as the author left it: every field of every
// step, the checks already acknowledged, where the last build went. It is
// remembered in <state>/bundles/<id>.cbor, one file per bundle, so building
// the next version is opening the page and pressing Build. The pictures and
// author-supplied files are held as bytes, not as paths: a banner that moved
// on the author's disk must not turn a one-click rebuild into a search.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../install/keys.h"
#include "../install/registry.h"
#include "../pack/kgpack.h"
#include "../util/pe.h"
#include "build.h"
#include "meta.h"

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

std::string encode_draft(const Draft& d);
// Throws std::runtime_error naming what is wrong. Keys it does not know are
// skipped, as bundle.meta's are, so an older kretro opens a newer one's draft.
Draft decode_draft(std::string_view cbor);

// <state>/bundles. A parameter everywhere below, so a test has its own.
std::filesystem::path bundles_dir();

// Every remembered bundle in `dir`, by title. A file that does not decode is
// skipped and named in `unreadable`, rather than hiding the rest.
std::vector<Draft> load_drafts(const std::filesystem::path& dir,
                               std::vector<std::string>* unreadable = nullptr);
// Written beside and renamed over, so a crash mid-write leaves the old draft.
// `was` is the id it was remembered under before, when the author renamed it
// before publishing: that file goes, or one bundle would be two. A file
// already there under d.id that is not `was` is another bundle, and is
// refused rather than written over: a new bundle left at its default title,
// or a title that happens to slug to an older bundle's id, would otherwise
// silently replace that bundle and everything the author set on it.
void save_draft(const std::filesystem::path& dir, const Draft& d, const std::string& was = "");
void forget_draft(const std::filesystem::path& dir, const std::string& id);

// `base`, or base-2, base-3... : the first no bundle in `dir` is remembered
// under. What a new bundle starts as, so it never starts as an old one.
std::string unused_id(const std::filesystem::path& dir, const std::string& base);

// Records a finished build on the bundle remembered as `id`, read fresh from
// `dir`: the page may be holding an older copy, or the author may have renamed
// the bundle while it built. False, and nothing written, when no bundle is
// remembered under that id any more - writing one back would bring a renamed
// bundle back as a second one.
bool stamp_built(const std::filesystem::path& dir, const std::string& id, const std::string& path,
                 uint64_t size, const std::string& when);

struct Check;
// Remembers a bundle `kretro bundle build` made, as the page would hold it
// after its Build: each check the build printed acknowledged - --rights and
// those notes are the acknowledgement a script makes, and `rebuild` is held
// to them - and the build stamped. A bundle already remembered under that id
// is the page's, with everything the author set on it, and is left as it is:
// false, and nothing written.
bool remember_built(const std::filesystem::path& dir, Draft d, const std::vector<Check>& checks,
                    const std::string& path, uint64_t size, const std::string& when);

// ---- what a pack on the shelf says ----------------------------------------------

struct PackFacts {
  std::filesystem::path path;
  Meta meta;
  uint64_t bytes = 0;           // the .kgpack as it sits on the shelf
  // Roughly what the pack would be without its discs: its bytes shared out
  // between the game and the discs by their unpacked sizes. The body is one
  // compressed image and does not say how much of it each directory is, so
  // this is an estimate and the page says "about".
  uint64_t without_discs = 0;
  size_t discs_carried = 0;
  size_t discs_named = 0;       // named by the pack but not inside it
};

// Reads the pack's header and metadata; the body is not touched.
PackFacts read_pack_facts(const std::filesystem::path& pack);

// A pack with a body that was packed before install packed with --categorize
// and 4 MiB blocks (kBodyPacking): it plays, from a slower mount. The page
// offers "Repack for faster loading" for it.
bool packed_before_faster_loading(const PackFacts& f);

// "Repack for faster loading": the body unpacked under `scratch`, its game
// tree held to the pack's Merkle root, packed again the way install packs one
// now, and the pack rewritten beside itself and renamed over. The tree - and
// so the Merkle root, the saves, a recipe's proof - is the same; only the body
// and its hash change. Throws std::runtime_error, or Cancelled, and leaves the
// pack as it was; `scratch` is removed either way.
void repack_for_faster_loading(const std::filesystem::path& pack, const std::filesystem::path& tool,
                               const std::filesystem::path& scratch, const Callbacks& cb = {});

// A game as the page first shows it: name and year from the pack, the cover
// from `cover_png` when one is given (the shelf's title screenshot).
DraftGame game_from_pack(const PackFacts& f, const std::filesystem::path& cover_png = {});

// ---- graphics: what "auto" would do ---------------------------------------------

struct AutoBackend {
  bool known = false;       // the executable was read
  std::string backend;      // dxvk | wined3d-vk | wined3d-gl | cnc-ddraw | native OpenGL
  std::string reason;       // player::choose_backend's own sentence
};

// What the player's policy picks from these imports on a machine with a GPU
// and Vulkan 1.4: the question the author is asking is what the game wants,
// not what this machine has.
AutoBackend auto_backend(const pe::Imports& exe);

// The game's executable, read out of the pack's body with dwarfsextract, and
// its imports. `tool` is dwarfs-universal; `scratch` a directory this may
// fill and empties again. Never throws: a pack that cannot be read gives
// Imports with ok false and the reason.
pe::Imports read_exe_imports(const PackFacts& f, const std::filesystem::path& tool,
                             const std::filesystem::path& scratch);

// The executable imports Glide and no Direct3D, DirectDraw or OpenGL: there
// is nothing for the player to draw it with except a Glide wrapper.
bool glide_only(const pe::Imports& exe);

// ---- keys -------------------------------------------------------------------------

// A pack's registry fragment is the REGEDIT4 text registry.cpp writes, not the
// Wine hive format parse_reg reads; this reads it back into values, strings
// decoded and everything else left as written.
std::vector<install::RegValue> read_fragment(std::string_view regedit4);

// A value in the fragment that is a key: serial-shaped by registry.cpp's rule,
// or exactly the key the vault holds for this game. Install keeps keys out of
// packs; a pack made elsewhere, or before that rule, may still carry one.
std::optional<install::RegValue> key_in_fragment(std::string_view regedit4, const std::string& vault_key);

// Where an embedded key would be written. The key's own old place when the
// fragment still shows it; otherwise the game's own key under Software - the
// one its installer wrote most values to - with the value name left for the
// author, because no pack records what the key it held back was called.
struct KeySpot {
  std::string path;   // with the hive: HKEY_LOCAL_MACHINE\Software\...
  std::string value;
};
KeySpot suggest_key_spot(std::string_view regedit4, const std::string& vault_key);

// ---- the check list -----------------------------------------------------------------

struct GameFacts {
  const PackFacts* pack = nullptr;
  std::optional<pe::Imports> imports;  // absent until read
  std::string vault_key;               // empty when the vault has none
};

struct Check {
  std::string id;     // stable, so an acknowledgement survives a restart
  std::string game;   // empty for the bundle as a whole
  std::string text;   // what was found, in a sentence
  std::string fix;    // what fixing it would be; empty when only acknowledging makes sense
};

// SafeDisc or SecuROM, the author's key and registry.reg, dgVoodoo, a Glide-only
// renderer, no cover art. `games` is in draft order, one per draft game.
std::vector<Check> run_checks(const Draft& d, const std::vector<GameFacts>& games);

// What stops a build outright, whatever is acknowledged: nothing to build, a
// field bundle.meta would refuse, a key to embed that the vault does not have.
std::vector<std::string> blockers(const Draft& d, const std::vector<GameFacts>& games);

// Build is offered when there are no blockers, every check is acknowledged,
// and the rights box is ticked.
bool ready_to_build(const Draft& d, const std::vector<Check>& checks,
                    const std::vector<std::string>& blocking);

// ---- size -----------------------------------------------------------------------

inline constexpr uint64_t kWarn2G = 2ull << 30;  // GitHub Releases, itch.io web upload
inline constexpr uint64_t kWarn4G = 4ull << 30;  // FAT32

struct SizePart {
  std::string label;
  uint64_t bytes = 0;
  uint64_t without_discs = 0;
};

struct SizeReport {
  SizePart runtime;             // the player base: bootstrap, tools, runtime, app
  uint64_t meta = 0;            // bundle.meta: pictures and extra files
  std::vector<SizePart> games;
  uint64_t total = 0;
  uint64_t total_without_discs = 0;
  std::vector<std::string> warnings;
};

// The file build_bundle would write, byte for byte when the sizes given are
// the real ones: what the author uploads and what a FAT32 stick has to hold.
// `base_bytes` is player_base_bytes of the base.
SizeReport size_report(uint64_t base_bytes, uint64_t meta_bytes,
                       const std::vector<const PackFacts*>& packs);

// ---- building ----------------------------------------------------------------------

// Where the player base comes from: kretro's own file, the kind 6 entry. A
// development tree can point KRETRO_PLAYER_BASE at a linked player base
// instead. Throws with what to do when there is none.
BaseSource find_player_base(const std::filesystem::path& self);

// What of the base goes into a player: everything up to the end of its last
// payload, its own table and trailer being replaced. The runtime line of the
// size report.
uint64_t player_base_bytes(const BaseSource& b);

// <id>-<version>.run
std::string output_name(const Draft& d);

// bundle.meta, from the draft and the vault. `keys` is the vault's contents;
// a game's key is read from it only when the author asked for it embedded.
// `exe64` says, per game id, whether its executable is 64-bit, which decides
// the registry view an embedded key is written for; a game not in it is taken
// as 32-bit, which every game of this era is.
BundleMeta meta_from_draft(const Draft& d, const std::vector<install::StoredKey>& keys,
                           const std::string& built_at, const std::vector<std::string>& licenses,
                           const std::map<std::string, bool>& exe64 = {});

// The licence notices a runtime tree carries, by file name.
std::vector<std::string> runtime_licenses(const std::filesystem::path& runtime_root);

// The licence notices the player base's own runtime carries: the runtime a
// player ships, not the one kretro runs on, which carries tools no player has.
// Read with dwarfsck out of a copy of the base's runtime entry - DwarFS reads
// past an image's end for its index, so it cannot be pointed into the middle
// of a larger file - and remembered in `cache` by the entry's BLAKE3, so the
// copy is made once per base. With `compute` false only what is remembered is
// returned. Empty when the list cannot be read; it is a record in bundle.meta,
// and the player lists its own runtime's notices when asked.
std::vector<std::string> base_licenses(const BaseSource& base, const std::filesystem::path& tool,
                                       const std::filesystem::path& cache, bool compute = true);

struct BuildInputs {
  std::filesystem::path self;        // kretro's own file
  std::filesystem::path games_dir;   // the shelf
  std::filesystem::path keys_file;   // the vault
  // dwarfs-universal: reads the base's licence list and a keyed game's
  // executable. May be empty; the licence list is then empty and a key is
  // written for the 32-bit view.
  std::filesystem::path tool;
  std::filesystem::path cache;       // where base_licenses remembers; scratch goes under it
  // A player base file to build from instead of the one kretro carries: the
  // command line's --base, for a base just linked by make player-base.
  std::filesystem::path base;
  // The file itself, in place of <out_dir>/<id>-<version>.run: the command
  // line's -o FILE. Written there directly, so build_bundle's refusal to
  // write over the base or a pack covers it, and no file of that other name
  // in the folder is replaced on the way.
  std::filesystem::path out;
};

// Everything the page's Build button does: the base out of kretro, the packs
// off the shelf in draft order, bundle.meta, build_bundle into
// <out_dir>/<id>-<version>.run.partial, verified and renamed. Throws as
// build_bundle does, and leaves no .partial behind.
Built build_from_draft(const Draft& d, const BuildInputs& in, const Callbacks& cb = {});

}  // namespace kg::bundle
