// Turning a disc you own into a game you can play.
//
// The result is one file: state/games/<id>.kgpack, carrying the game's tree,
// its Merkle root, the recipe that produced it and the fingerprint of the disc
// it came from. That file is simultaneously the installed game and the
// shareable capsule; there is no separate export step and no install directory.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "../pack/kgpack.h"
#include "../rt/env.h"

namespace kg::install {

// Reads a game manifest. Manifests are the human-authored source; they are
// compiled into a pack's metadata at install time.
Meta load_manifest(const std::filesystem::path& toml);

// Where local manifests are looked for, in preference order. None is required.
std::vector<std::filesystem::path> manifest_dirs(const rt::Env& e);
std::filesystem::path find_manifest(const rt::Env& e, const std::string& id);
std::vector<std::string> known_games(const rt::Env& e);

// Where discs are looked for.
std::filesystem::path iso_dir();
// Whether a name is one this may look up in the collection directory: a bare
// filename, and nothing that walks out of it. A recipe someone sent decides
// which file this opens, and for installer_exe which file Wine then runs, so
// the name has to be answerable inside iso_dir() or not at all.
bool is_collection_name(const std::string& name);
// Case-insensitive, because a manifest records a disc's name as its owner
// typed it and the file on disk may differ. Empty for a name is_collection_name
// refuses.
std::filesystem::path find_iso(const std::string& name);
// Matches a disc by what it *is* rather than what it is called.
std::filesystem::path find_iso_by_fingerprint(const DiscFingerprint& want);

struct Options {
  bool force = false;        // reinstall over an existing pack
  bool keep_tree = false;    // leave the extracted tree for inspection
  // Whether the pack carries the discs it was built from. True is the honest
  // default and what the wizard's checkbox starts on: the disc is stored once
  // and deduplicated against the installed files, so a copy game pays almost
  // nothing for it. A disc that is mostly *not* this game - one volume holding
  // three of them - is the case where a caller wants it off, and then the
  // game's page says "needs the original disc" the way it does for a disc the
  // library cannot find.
  bool embed_discs = true;
  // Set when rebuilding from a recipe someone shared: the tree that comes out
  // is compared against this and the difference reported. It is not a refusal -
  // a person clicking through an installer twice need not produce the same
  // bytes, which is the promise the answer file used to keep.
  bool expect_root_set = false;
  Hash expect_root{};
};

// One disc, as it will appear in the body.
struct BodyDisc {
  std::filesystem::path tree;                 // the extracted disc
  std::string label;
  uint32_t serial = 0;
  std::vector<std::filesystem::path> audio;   // the ripped tracks
};

// Lays the body out under `root`, which mkdwarfs is then pointed at:
//
//   game/            the installed tree, byte for byte what a flat body was
//   system/          what the installer wrote to C: outside the game directory,
//                    relative to drive_c: windows/system32/msvcrt.dll and the
//                    rest of what a 1998 setup scatters around the machine
//   discs/<n>/       disc n, with .windows-label and .windows-serial already
//                    written - at play time the tree is a read-only mount and
//                    nothing can put them there
//   discs/<n>/audio/ the FLAC tracks ripped from that disc
//   registry.reg     the fragment, as text, for a person who mounts the image
//
// `tree`, `system` and each disc tree are moved where the filesystem allows and
// hardlinked otherwise: they are already gigabytes and already inside the
// staging directory, and a copy here would double the disk an install needs.
// Everything named is consumed - do not read `tree` or a disc's tree after.
//
// `system` may be empty, and is for every copy and unzip install: no installer
// ran, so nothing was written outside the tree to carry.
void lay_out_body(const std::filesystem::path& root, const std::filesystem::path& tree,
                  const std::vector<BodyDisc>& discs, const std::string& registry_fragment,
                  const std::filesystem::path& system = {});

// Whether a path the installer wrote, relative to drive_c, belongs in system/.
//
// Everything it wrote that is not under the game's own directory does - that is
// the whole point - except the scratch it left behind. An installer unpacks
// itself into C:\windows\Temp or the user's Temp and does not clean up; those
// megabytes are not part of the game by anybody's reading, and carrying them
// would put an InstallShield self-extraction into every pack.
//
// `install_dir` is compared case-insensitively and with either slash, because
// it is a Windows path that reached us through a Linux filesystem.
bool is_outside_the_game(const std::string& path, const std::string& install_dir);

// Collects `paths`, relative to `drive_c`, into `into`, keeping their layout.
// Returns how many files landed and how many bytes they are. Missing entries
// are skipped rather than fatal: the game's own directory has already been
// moved out of drive_c by the time this runs, and an installer's temporary file
// may have been deleted by the installer itself between the diff and here.
struct SystemFiles {
  size_t files = 0;
  uint64_t bytes = 0;
};
SystemFiles gather_system_files(const std::filesystem::path& drive_c,
                                const std::vector<std::string>& paths,
                                const std::filesystem::path& into);

// The other end: puts a pack's system/ back into a prefix's drive_c. Returns
// how many files it placed.
//
// It overwrites, and has to. What is here is what the installer put on the
// machine that built the pack, on top of what wineboot had just laid down
// there; the registry fragment that is imported immediately after names these
// files by path, so restoring anything less than what the installer left would
// register a COM class against a DLL that is not there.
size_t restore_system_files(const std::filesystem::path& system,
                            const std::filesystem::path& drive_c);

struct Result {
  std::filesystem::path pack;
  bool root_matched = false;  // meaningful only when Options::expect_root_set
  uint64_t tree_bytes = 0;
  uint64_t pack_bytes = 0;
  size_t entries = 0;
  Hash root{};
};

Result run(const rt::Env& e, Meta manifest, const Options& opt,
           const std::function<void(const std::string&)>& progress);

}  // namespace kg::install
