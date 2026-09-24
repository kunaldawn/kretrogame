#include "layers.h"

#include <stdexcept>
#include <string>
#include <vector>

#include "../pack/dwarfs.h"
#include "../util/format.h"
#include "../util/paths.h"
#include "../util/proc.h"
#include "internal.h"
#include "saves_layout.h"
#include "unpack.h"

namespace kg::session {
namespace fs = std::filesystem;

using detail::unmount;
using detail::Unmount;
using detail::wait_for;

void open_layers(Layers& l, const rt::Env& e, const Pack& pack, const Source* src) {
  const std::string id = pack.meta().id;
  const bool rooted = pack.meta().rooted();
  ensure_state_dirs();

  // The writable layer and its workdir stay with the saves - fuse-overlayfs
  // renames between the two, so they have to share a filesystem - and the
  // mount points go wherever game_mount_dir says. For kretro that is the same
  // directory; for a player it is the session's runtime directory, never the
  // state beside a binary on a USB stick.
  fs::path root = game_saves_dir(id);
  fs::path mounts = game_mount_dir(id);
  l.upper = live_dir(id);
  l.work = root / "work";
  l.merged = mounts / "merged";
  l.image = mounts / "image";
  fs::path legacy_base = root / "base";
  std::error_code ec;
  for (const fs::path& p : {l.upper, l.work, l.merged, l.image}) fs::create_directories(p, ec);

  // A session that was killed rather than closed leaves its mounts behind, and
  // mounting over them fails with EBUSY - which would silently demote every
  // later session to the extracted path. Clear them first; unmounting something
  // that is not mounted is harmless. `base` is in the list because builds
  // before the rooted layout mounted the body there, and one of those mounts
  // outliving its process would sit in this directory forever otherwise.
  for (const fs::path& stale : {l.merged, l.image, legacy_base}) {
    ec.clear();
    const bool occupied = fs::exists(stale, ec) && !fs::is_empty(stale, ec);
    // A mountpoint whose FUSE daemon has died is exactly the case this sweep
    // exists for, and it is the one case that answers neither question: both
    // calls fail with ENOTCONN, is_empty comes back false with ec set, and
    // reading that as "nothing here" skips the unmount and leaves the
    // mountpoint wedged for every session after this one. So a failed question
    // counts as a yes.
    if (occupied || ec) {
      // -z detaches a mount whose FUSE process is already gone, which a plain
      // -u cannot do and which otherwise leaves the mountpoint permanently
      // unusable.
      unmount(stale, Unmount::PlainThenLazy);
      // A lazy unmount detaches immediately but the mountpoint can take a
      // moment to become usable again; remounting into it too early is what
      // silently demotes the session to the extracted path. An empty directory
      // that can still be asked the question is the whole condition: a
      // detached mountpoint answers with an error until the kernel is done
      // with it.
      wait_for([&] {
        std::error_code we;
        bool empty = fs::is_empty(stale, we);
        return empty && !we;
      }, 40, 50);
    }
  }
  ec.clear();
  fs::remove_all(l.work, ec);
  fs::create_directories(l.work, ec);

  fs::path tool = dwarfs_tool();
  const Header& h = pack.header();
  const bool no_fuse = src && src->no_fuse;

  // Mount the body straight out of the .kgpack at its offset. Installing was
  // the last time these bytes moved - and building a player did not move them
  // either: a pack inside one is mounted at the player's offset of the pack
  // plus the pack's own offset of its body.
  if (!tool.empty() && h.has_body() && !no_fuse) {
    std::string opts = "offset=" + std::to_string(pack.base() + h.body_off) +
                       ",imagesize=" + std::to_string(h.body_len) + ",ro";
    if (src && !src->cache_size.empty()) opts += ",cachesize=" + src->cache_size;
    // Taken as mounted while it goes up, for a signal that comes in the two
    // seconds this may wait: its handler unmounts what is marked mounted, and
    // unmounting what never went up is harmless.
    l.image_mounted = true;
    ProcResult r = kg::run({tool.string(), "--tool=dwarfs", pack.path().string(), l.image.string(),
                            "-o", opts});
    l.image_mounted = r.ok() && wait_for([&] { return !fs::is_empty(l.image, ec); }, 40, 50);
  }

  // The game is a directory inside the image for a rooted pack and the image
  // itself for a flat one. Either way the overlay is built over exactly the
  // game tree, so merged is precisely the game and upper is precisely what the
  // game wrote - unchanged from every session before this one.
  l.base = rooted ? l.image / "game" : l.image;

  // Why this is not going to be a mount, in a person's words, for when the
  // unpacked copy it falls back to may not be made without asking.
  std::string no_mount = no_fuse ? "this machine cannot mount games (it has no FUSE)"
                                 : "the game could not be mounted here";
  if (l.image_mounted && !rt::which(e, "fuse-overlayfs").empty()) {
    // The extra layer goes leftmost, which in an overlay is uppermost of the
    // read-only ones: its files win over the pack's, and the game's own writes
    // still land in upper and nowhere else.
    std::string lower = l.base.string();
    if (src && !src->extra_layer.empty() && fs::is_directory(src->extra_layer, ec)) {
      lower = src->extra_layer.string() + ":" + lower;
    }
    l.overlay_mounted = true;  // while it goes up, as the image above
    std::string why = mount_overlay(e, lower, l.upper, l.work, l.merged);
    l.overlay_mounted = why.empty();
    if (!l.overlay_mounted) {
      log_line("no writable layer (" + why + ")");
      no_mount = "the game mounts here, but no writable layer could be put over it (" + why + ")";
    }
  }

  if (l.overlay_mounted) return;

  // Either FUSE is unavailable or the overlay refused. Extract once and play
  // from a real tree; what the game wrote is worked out at exit instead.
  if (l.image_mounted) {
    unmount(l.image, Unmount::Plain);
    l.image_mounted = false;
  }
  fs::path extracted = src && !src->extract_dir.empty() ? src->extract_dir : game_extract_dir(id);
  // Beside the tree, never inside it: for a flat pack the tree *is* the game
  // directory, and a stamp within it is a phantom written file in every
  // exit-time diff against meta.tree.
  fs::path stamp_file = extraction_stamp_file(extracted);
  const std::string stamp = extraction_stamp(pack.meta(), h);
  // The whole image is unpacked, discs and all, so the discs are reachable by
  // the same path as on the FUSE side. merged is the game tree and only the
  // game tree, which is what the exit-time diff against meta.tree needs: had it
  // been the image root, every session would have reported the entire disc set
  // as newly written.
  fs::path game = rooted ? extracted / "game" : extracted;

  const bool may_unpack = !src || src->may_unpack;
  auto unpack = [&] {
    if (!may_unpack) {
      throw NeedsUnpack(pack.meta().name + " has to be unpacked before it can be played: " + no_mount);
    }
    if (tool.empty()) throw std::runtime_error("no DwarFS tool, and no extracted copy to fall back to");
    log_line("unpacking the game (once per install)");
    unpack_body(pack, extracted);
  };

  bool fresh = false;
  if (!extraction_stamp_matches(stamp_file, stamp)) { unpack(); fresh = true; }
  // A stamp that matches a tree which is not where the layout says it is is
  // still not a tree. Believing it hands back a Layers pointing at nothing, and
  // the failure surfaces two steps later as "no <exe> in the installed game" -
  // about a pack that is perfectly good.
  if (!fresh && !fs::exists(game, ec)) unpack();
  if (!fs::exists(game, ec)) {
    throw std::runtime_error("unpacked the game but there is nothing at " + game.string());
  }

  l.image = extracted;
  l.base = l.merged = game;
  l.extracted = true;

  // No overlay to put the extra layer in, so its files are copied into the
  // tree - once, as place_extra_files does, when they differ - and remembered,
  // so the exit-time diff does not report them as the game's writes.
  if (src && !src->extra_layer.empty()) {
    const fs::path& layer = src->extra_layer;
    for (auto it = fs::recursive_directory_iterator(layer, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const std::string rel = fs::relative(it->path(), layer, ec).generic_string();
      if (ec || rel.empty() || rel.rfind("..", 0) == 0) continue;
      const fs::path dst = game / rel;
      std::error_code cec;
      fs::create_directories(dst.parent_path(), cec);
      fs::copy_file(it->path(), dst, fs::copy_options::update_existing, cec);
      l.layered.push_back(rel);
    }
    ec.clear();
  }
}

std::string mount_overlay(const rt::Env& e, const fs::path& lower, const fs::path& upper,
                          const fs::path& work, const fs::path& merged) {
  fs::path fo = rt::which(e, "fuse-overlayfs");
  const std::vector<std::string> args = {
      "-o", "lowerdir=" + lower.string() + ",upperdir=" + upper.string() + ",workdir=" + work.string(),
      merged.string()};
  std::error_code ec;
  auto mounted = [&] { return wait_for([&] { return !fs::is_empty(merged, ec); }, 40, 50); };
  // An attempt that exited 0 mounted something, whatever merged shows, and
  // close_layers is only told about a mount that is reported: so one that
  // shows nothing is detached here rather than left under the next session.
  auto settled = [&](const ProcResult& p) {
    if (!p.ok()) return false;
    if (mounted()) return true;
    unmount(merged, Unmount::Lazy);
    return false;
  };
  ProcResult r = rt::run(e, fo, args);
  if (settled(r)) return "";

  // Once more, from a loader with no name. Ubuntu 25.04 and later confine
  // fusermount3 with an AppArmor profile whose only unix-socket rules are for
  // its own label and for unconfined peers. Started from inside a snap with
  // classic confinement - VS Code's terminal is the common one - every process
  // of ours carries the snap's label, so does the socket fuse-overlayfs makes
  // to be handed /dev/fuse on, and the kernel closes that socket as
  // fusermount3 enters its profile: "file descriptor 6 is not a socket". The
  // same program executed from an anonymous memfd is one the snap's
  // complain-mode profile cannot name, so it runs in a learning profile of its
  // own, fusermount3 started from there keeps what it inherits, and the mount
  // goes through. It is how the DwarFS mounts have always got through on such
  // a machine: the bootstrap runs dwarfs-universal from a memfd for its own
  // reasons. Unconfined, the first attempt is the one that works and this is
  // never reached; confined for real, this fails as the first did, and what
  // is reported is the first attempt's reason, which is the true one.
  //
  // Only after a refusal, though: fusermount3 refused ends in an exit status.
  // A first attempt that exited 0 was not refused, and a second over it would
  // stack an overlay on an overlay.
  if (!r.ok()) {
    ProcResult again = rt::run_unnamed(e, fo, args);
    if (settled(again)) return "";
  }

  // What fuse-overlayfs said, when it said anything, rather than an empty
  // "()" in the one case that has a reason.
  std::string why = r.out;
  while (!why.empty() && (why.back() == '\n' || why.back() == ' ')) why.pop_back();
  if (why.empty()) why = "fuse-overlayfs failed";
  // The one thing fusermount3's message leaves out is why the descriptor was
  // not a socket, and the label is the whole of that answer.
  if (why.find("is not a socket") != std::string::npos) {
    if (std::string label = rt::confinement(); !label.empty()) {
      why += "; this process runs confined by the AppArmor profile " + label +
             ", whose sockets fusermount3's profile does not accept";
    }
  }
  return why;
}

void close_layers(Layers& l) {
  // Innermost first. The overlay's lowerdir is inside the image.
  if (l.overlay_mounted) {
    unmount(l.merged, Unmount::Plain);
    l.overlay_mounted = false;
  }
  if (l.image_mounted) {
    unmount(l.image, Unmount::Plain);
    l.image_mounted = false;
  }
}

}  // namespace kg::session
