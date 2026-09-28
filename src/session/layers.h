// The game directory a session gives the game: the pack's body mounted
// read-only, a writable layer over it, or an unpacked tree where there is no
// FUSE.
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "../pack/kgpack.h"
#include "../rt/env.h"

namespace kg::session {

struct Layers {
  // The whole body: games/<id>/ for each game of the set and discs/<key>/ for
  // each disc. This is the mountpoint; `base` is a path inside it, not a mount
  // of its own.
  //
  // Which of those exist varies and none of them is promised: a copy install
  // has no system/, an installer with no disc has no discs/. Every reader here
  // looks before it reads.
  //
  // Mounting the image here rather than at `base` is what makes the discs
  // reachable at all, and it leaves the saves layer exactly as it was: had the
  // overlay been built over the image with the game at merged/game, every write
  // the game made would have landed under upper/game/ and silently changed the
  // layout that live_dir, snapshot, restore and the saves export all assume.
  std::filesystem::path image;
  std::filesystem::path base;    // the game, read-only: image/games/<id>/game
  std::filesystem::path upper;   // where the game's writes land
  std::filesystem::path work;
  std::filesystem::path merged;  // what the game is given as its directory
  bool image_mounted = false;
  bool overlay_mounted = false;
  bool extracted = false;        // no FUSE: base is a real tree, merged == base
  // The files a Source's extra_layer put into an unpacked tree, relative to
  // it: not the game's writes, however the exit-time diff sees them.
  std::vector<std::string> layered;

  bool writes_isolated() const { return overlay_mounted; }
};

// Where a pack's bytes are, when they are not state/games/<id>.kgpack: a pack
// carried inside a player, `len` bytes at `off` in `file`, and how it may be
// reached there.
struct Source {
  std::filesystem::path file;
  uint64_t off = 0;
  uint64_t len = 0;
  // DwarFS's block cache for this mount, as its -o cachesize takes it. A
  // player caps it, because it may mount a game on a machine it knows nothing
  // about; empty is DwarFS's own default.
  std::string cache_size;
  // The bootstrap already found that FUSE does not work here: do not try.
  bool no_fuse = false;
  // Whether a failed mount may fall back to unpacking the game. A player asks
  // first, with the size and the place, and only then unpacks; this is false
  // for it, and a mount that fails anyway is NeedsUnpack rather than gigabytes
  // written without a word.
  bool may_unpack = true;
  // Where an unpacked copy is, or goes: set_extract_dir(set) when empty. A
  // player unpacked with --extract-to somewhere roomier remembers where.
  std::filesystem::path extract_dir;
  // A directory laid out like the game's own whose files are put over the
  // game, read-only: an author's dgVoodoo beside the executable. With an
  // overlay it is a lower layer above the pack's, so none of it is ever in
  // the writable layer - the saves, a snapshot, a saves export. Unpacked, the
  // files are copied into the tree and left out of what the session says the
  // game wrote. Empty for none.
  std::filesystem::path extra_layer;
};

// Thrown when a game has to be unpacked to be played and the Source said not
// to without asking.
class NeedsUnpack : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Mounts the pack's body straight out of the .kgpack at its offset - no copy,
// no temporary file - and puts a writable layer over game `id`'s tree in it,
// into `l` as it goes.
// `src`, when given, is where the pack came from and how it may be mounted;
// the offsets are the pack's own plus pack.base().
//
// Each mount is marked in `l` from before it is attempted, so a caller that
// hands `l` to a signal handler first has every mount released by it, however
// far this got.
void open_layers(Layers& l, const rt::Env& e, const Pack& pack, const std::string& id,
                 const Source* src = nullptr);

// Puts the runtime's fuse-overlayfs over `lower` at `merged`. Empty when it is
// mounted; otherwise what went wrong, in fuse-overlayfs's words.
std::string mount_overlay(const rt::Env& e, const std::filesystem::path& lower,
                          const std::filesystem::path& upper, const std::filesystem::path& work,
                          const std::filesystem::path& merged);
void close_layers(Layers& l);

}  // namespace kg::session
