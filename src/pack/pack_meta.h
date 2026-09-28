// A pack's metadata: the media set it is - its discs, each once, and the games
// installed from them - and for each game who it is, how it was installed, how
// it runs and the tree it is. Encoded as CBOR and stored zstd-compressed after
// the header.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "../util/hash.h"
#include "tree.h"

namespace kg {

// Meta::Body::packing for a body made with `mkdwarfs --categorize -S 22`:
// what is already compressed stored raw, in 4 MiB blocks, so a random read
// from a mount decompresses one block's worth. Every body this build packs is
// packed this way; the string says so in the pack, so a pack that reads slowly
// can be told apart if the flags ever change again.
//
// mkdwarfs_body (dwarfs.h) runs exactly those flags. The two are a pair and
// change together.
inline constexpr const char* kBodyPacking = "categorize,S22";

// One characteristic file on a disc. A handful of these let us recognise a
// differently-mastered but content-equivalent disc that the whole-image hash
// would miss.
struct Anchor {
  std::string path;
  uint64_t size = 0;
  Hash hash{};
};

struct DiscFingerprint {
  std::string filename;   // as it was named locally; advisory only
  uint64_t size = 0;
  // Of the whole image where it was read whole, and of the first
  // iso::kPrefixBytes where it was not: install::run hashes the whole disc
  // because it is already reading it, the wizard records what opening the disc
  // read. find_iso_by_fingerprint accepts either, and says so.
  Hash blake3{};
  std::string volume_id;  // from the primary volume descriptor
  std::string created;    // PVD creation timestamp, as recorded on the disc
  std::vector<Anchor> anchors;
};

struct Meta {
  // identity
  std::string id, name, developer, publisher;
  uint32_t year = 0;

  struct Recipe {
    std::string method;  // unzip | copy | wine_setup | installer_exe
    std::string member, subdir, setup;
    std::string setup_ref;  // which disc the installer is on, as a DiscRef
    std::vector<std::string> discs, verify;
    std::vector<DiscFingerprint> fingerprints;
  } recipe;

  struct Run {
    std::string exe, args, windows_version;
    uint32_t width = 0, height = 0;
  } run;

  struct Runtime {
    std::string id;
    Hash blake3{};
    std::string dlloverrides;
    std::vector<std::string> winetricks;
    bool dgvoodoo = false;
  } runtime;

  // Note: there is no `filter` field. Older packs carry one, written on every
  // install and read by nothing, and a field like that is a lie a file format
  // tells its reader. Readers tolerate it in packs that carry it.
  struct Present {
    std::string dar = "4:3";
    bool pause_on_blur = true;
  } present;

  // Where under C: the installer put the game. It is the one thing the old
  // record/replay block leaves behind that anything still reads: the six
  // others - the family, the answer file's name and its contents, replayable,
  // the reason it was not, and from_record - all described an installer being
  // driven with nobody present, and nothing writes them now. They go rather
  // than stay as what the note above calls a lie a file format tells its
  // reader.
  struct Install {
    std::string install_dir;
  } install;

  // The registry values the installer created. A capsule without these is a
  // capsule of files that will not start.
  struct Registry {
    std::string fragment;   // REGEDIT4 text
  } registry;

  // Everything the installer wrote outside the game's own directory: the DLLs,
  // the OCXs, the shared runtime a 1998 setup dropped into C:\windows\system32
  // and the redistributable it unpacked beside it. Those files never lived in
  // install_dir, so a body that carries only game/ throws them away - while the
  // registry fragment that names them travels intact and is applied, verbatim,
  // against a prefix that has only ever seen `wineboot --init`. The pack was
  // then a set of registered COM classes pointing at files that are not there.
  //
  // They are in the body at system/, laid out relative to drive_c, and are put
  // back into the prefix before the fragment is imported. `files` being zero is
  // the ordinary case for a copy or unzip install, which runs no installer and
  // writes nothing to C: at all.
  struct System {
    uint32_t files = 0;
    uint64_t bytes = 0;
  } system;

  // The discs this install used, so a restored capsule can present the same
  // CD-ROM drives to a game that checks for its disc at runtime.
  struct Disc {
    // Its directory in the body, discs/<key>/: disc_key() of the image, so the
    // same disc has the same key whichever game brought it into the set.
    std::string key;
    std::string label;
    uint32_t serial = 0;
    std::string ref;   // "archive#LABEL", to find it again in a collection
    // The absolute path it was actually opened from. find_iso and
    // find_iso_by_fingerprint only look at the top level of iso_dir(), so a
    // disc the wizard was handed from anywhere else on the disk would resolve
    // to nothing on a rebuild. This is how it is found again.
    std::string source;
    // Its tree and CD audio unpacked, for "how much room would unpacking take".
    uint64_t bytes = 0;
  };
  // This game's discs in drive order: discs[i] is drive 'd'+i. Filled from the
  // set's list on decode.
  std::vector<Disc> discs;

  // Per-game gamepad bindings, over the default map. A 1997 game with the wrong
  // keys can be fixed without changing the default for every other game.
  std::map<std::string, std::string> input;

  // Describes the body so corruption is detectable without extracting it. It
  // is the set's body, one per pack and shared by every game in it; decode
  // fills it in each game's view.
  struct Body {
    uint64_t length = 0;
    Hash blake3{};
    // How mkdwarfs laid the body out: kBodyPacking for a pack packed the way
    // install packs one now, empty for one packed before that was said. Only
    // how the body stores the tree, never what is in it, so never part of the
    // Merkle root.
    std::string packing;
  } body;

  Tree tree;
};

// A pack's whole metadata: one media set. A DVD holding three games is one set
// of three games and one disc, so the disc is stored once however many games
// came off it.
struct SetMeta {
  // Names the pack on the shelf, packs/<set_id>.kgpack: set_id_for() of the
  // discs it was made from, so it never names a game.
  std::string set_id;
  // Each disc once, in the order the set first carried them.
  std::vector<Meta::Disc> discs;
  // Sorted by id when encoded. Each one's `discs` and `body` are filled from
  // the set on decode.
  std::vector<Meta> games;
  Meta::Body body;

  const Meta* find(std::string_view id) const;
  // The header's root: BLAKE3 over the games' Merkle roots in id order. Each
  // game's own root keeps meaning the installed game and nothing else.
  Hash root() const;

  std::string encode() const;  // CBOR
  static SetMeta decode(std::string_view cbor_data);
};

// A disc's key: the first 16 hex digits of BLAKE3 over its size (8 bytes,
// little endian) and its 64 MiB prefix hash. Both are read by every path that
// opens a disc, so a disc gets the same key whichever of them opened it; the
// fingerprint's hash cannot serve, because the wizard records the prefix's
// there and install::run the whole image's.
std::string disc_key(uint64_t size, const Hash& prefix);
// "s-" and 16 hex digits of BLAKE3 over the sorted keys, one per line, or over
// "game:<id>" for a game with no disc at all. Titles never reach a set's name.
std::string set_id_for(std::vector<std::string> disc_keys, std::string_view game_id);
// Where a game and a disc are inside a set's body.
std::filesystem::path body_game_dir(std::string_view id);   // games/<id>
std::filesystem::path body_disc_dir(std::string_view key);  // discs/<key>
// A set of one game, named after that game's discs.
SetMeta set_of(Meta m);

}  // namespace kg
