// A pack's metadata: who the game is, how it was installed, how it runs, the
// discs it came from and the tree it is. Encoded as CBOR and stored
// zstd-compressed after the header.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "../util/hash.h"
#include "tree.h"

namespace kg {

// Meta::Body::packing for a body made with `mkdwarfs --categorize -S 22`:
// what is already compressed stored raw, in 4 MiB blocks, so a random read
// from a mount decompresses one block's worth. A body without it was packed
// before, with mkdwarfs's defaults, and reads more slowly; the Bundles page
// offers to repack it.
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

  // Where the game is inside the body.
  //
  //   "flat"    the DwarFS image root *is* the game tree. Every pack written
  //             before container revision 2 is this, and stays this.
  //   "rooted"  the image root holds game/ , system/ , discs/<n>/ and
  //             registry.reg.
  //
  // `tree` covers game/ in both cases, with paths relative to it, so the Merkle
  // root keeps the one meaning it has always had: the identity of the installed
  // game. A pack built with its discs and one built without therefore have the
  // same root and answer to the same recipe. What is outside game/ is covered
  // by body.blake3 instead - two hashes, two jobs.
  std::string layout = "flat";

  bool rooted() const { return layout == "rooted"; }

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
    std::string label;
    uint32_t serial = 0;
    std::string ref;   // "archive#LABEL", to find it again in a collection
    // The absolute path it was actually opened from. find_iso and
    // find_iso_by_fingerprint only look at the top level of iso_dir(), so a
    // disc the wizard was handed from anywhere else on the disk would resolve
    // to nothing on a rebuild. This is how it is found again.
    std::string source;
    // True when this disc's tree is inside the body, at discs/<n>/. Every pack
    // this kretro writes embeds every disc it installed from, so today the
    // false case means a revision 1 pack, which had a flat body and nowhere to
    // put them. The field is wider than the writer on purpose: a pack that was
    // built somewhere else, or by a later kretro that can be told to leave the
    // gigabytes out, describes itself here and is read correctly. Play
    // attaches a CD-ROM only for the embedded ones; for the rest the honest
    // answer is that the game needs the original disc, and `ref` says which.
    bool embedded = false;
  };
  std::vector<Disc> discs;

  // Per-game gamepad bindings, over the default map. A 1997 game with the wrong
  // keys can be fixed without changing the default for every other game.
  std::map<std::string, std::string> input;

  // Describes the body so corruption is detectable without extracting it.
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

  std::string encode() const;                     // CBOR
  static Meta decode(std::string_view cbor_data);
};

}  // namespace kg
