#include "session.h"

#include <cctype>
#include <map>
#include <sstream>

#include "../config/config.h"
#include "../config/scaling.h"
#include "../disc/drive.h"
#include "../install/install.h"
#include "../install/registry.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "../util/paths.h"

namespace kg::session {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

void say(const std::string& s) { std::fprintf(stderr, "  %s\n", s.c_str()); }

const char* env(const char* k) {
  const char* v = std::getenv(k);
  return (v && *v) ? v : nullptr;
}

fs::path dwarfs_tool() {
  if (const char* d = env("KRETRO_DWARFS")) return d;
  return {};
}

fs::path journal_dir(const std::string& id) { return game_saves_dir(id) / "journal"; }
fs::path gen_dir(const std::string& id) { return game_saves_dir(id) / "gen"; }
fs::path live_dir(const std::string& id) { return game_saves_dir(id) / "live"; }

bool wait_for(const std::function<bool()>& cond, int tries, int ms) {
  for (int i = 0; i < tries; ++i) {
    if (cond()) return true;
    usleep(static_cast<useconds_t>(ms) * 1000);
  }
  return false;
}

// Matches a name in a directory ignoring case. Wine is case-insensitive and
// the manifests record real case for the reader, so neither may be trusted to
// agree with the filesystem.
std::string resolve_ci(const fs::path& dir, const std::string& want) {
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    if (lower(de.path().filename().string()) == lower(want)) return de.path().filename().string();
  }
  return "";
}

std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o.push_back(c);
    }
  }
  return o;
}

std::string json_field(const std::string& src, const std::string& key) {
  std::string needle = "\"" + key + "\":";
  size_t p = src.find(needle);
  if (p == std::string::npos) return "";
  p += needle.size();
  while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
  if (p >= src.size()) return "";
  if (src[p] == '"') {
    ++p;
    std::string out;
    while (p < src.size() && src[p] != '"') {
      if (src[p] == '\\' && p + 1 < src.size()) ++p;
      out.push_back(src[p++]);
    }
    return out;
  }
  size_t end = src.find_first_of(",}\n", p);
  return src.substr(p, end == std::string::npos ? std::string::npos : end - p);
}

// Set while a session holds mounts, so a signal can still release them.
// Interrupting a game is normal - a timeout, a Ctrl-C, closing the terminal -
// and mounts left behind silently demote every later session to the slower
// path, which is a confusing way to be punished for pressing Ctrl-C.
Layers* g_active = nullptr;

void release_on_signal(int sig) {
  const char* msg = "\nkretro: interrupted, releasing mounts\n";
  ssize_t ignored = ::write(2, msg, __builtin_strlen(msg));
  (void)ignored;
  if (g_active) {
    // The game is still running with its working directory inside the mount,
    // so a plain unmount is refused as busy. The lazy form detaches anyway,
    // which is what we want: the mountpoint has to be usable next time even
    // though this session is being cut short.
    if (g_active->overlay_mounted) {
      kg::run({"fusermount3", "-u", g_active->merged.string()});
      kg::run({"fusermount3", "-u", "-z", g_active->merged.string()});
    }
    // Innermost first: the overlay sits on image/game, so the image cannot go
    // until the overlay has.
    if (g_active->image_mounted) {
      kg::run({"fusermount3", "-u", g_active->image.string()});
      kg::run({"fusermount3", "-u", "-z", g_active->image.string()});
    }
    g_active = nullptr;
  }
  _exit(128 + sig);
}

}  // namespace

bool link_tree(const fs::path& from, const fs::path& to, size_t* files) {
  std::error_code ec;
  bool ok = true;
  fs::create_directories(to, ec);
  if (ec) return false;
  auto it = fs::recursive_directory_iterator(from, ec);
  if (ec) return false;
  for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
    // A directory this walk cannot descend into - it went away underneath us,
    // or it is not ours to read - ends the walk. Whatever is under it is not
    // in `to`, and saying so is the whole point of the return value. The same
    // check again below the loop, because the increment that fails is also the
    // increment that reaches the end: the loop condition is tested before this
    // line is, and an error found on the way out would otherwise be read as a
    // walk that finished.
    if (ec) return false;
    std::error_code e;
    fs::path rel = fs::relative(it->path(), from, e);
    if (e) { ok = false; continue; }
    fs::path dst = to / rel;
    const bool dir = it->is_directory(e);
    if (e) { ok = false; continue; }
    if (dir) {
      fs::create_directories(dst, e);
      if (e) ok = false;
      continue;
    }
    fs::create_directories(dst.parent_path(), e);
    e.clear();
    fs::create_hard_link(it->path(), dst, e);
    if (e) {  // different filesystem, or a symlink
      e.clear();
      fs::copy(it->path(), dst,
               fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks, e);
      // Not counted, and not survivable: a generation missing a file is not
      // the state the game was in, and a caller about to delete the only other
      // copy has to hear about it.
      if (e) { ok = false; continue; }
    }
    if (files) ++*files;
  }
  if (ec) return false;
  return ok;
}

fs::path lock_file(const std::string& id) { return game_saves_dir(id) / "lock"; }

GameLock::~GameLock() { release(); }

GameLock::GameLock(GameLock&& other) noexcept : fd_(other.fd_), busy_(other.busy_) {
  other.fd_ = -1;
}

GameLock& GameLock::operator=(GameLock&& other) noexcept {
  if (this != &other) {
    release();
    fd_ = other.fd_;
    busy_ = other.busy_;
    other.fd_ = -1;
  }
  return *this;
}

void GameLock::release() {
  if (fd_ >= 0) {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
  }
}

GameLock lock_game(const std::string& id) {
  GameLock l;
  std::error_code ec;
  fs::create_directories(game_saves_dir(id), ec);
  // O_CLOEXEC: the game, the compositor and the screenshooter are all forked
  // from here, and a descriptor that survived into one of them would keep the
  // lock alive after this process had let it go.
  int fd = ::open(lock_file(id).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return l;  // no lock file, so no lock, and no accusation either
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    l.busy_ = true;
    return l;
  }
  l.fd_ = fd;
  return l;
}

Layers open_layers(const rt::Env& e, const Pack& pack, const Source* src) {
  Layers l;
  open_layers_into(l, e, pack, src);
  return l;
}

void open_layers_into(Layers& l, const rt::Env& e, const Pack& pack, const Source* src) {
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
      kg::run({"fusermount3", "-u", stale.string()});
      kg::run({"fusermount3", "-u", "-z", stale.string()});
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
      say("no writable layer (" + why + ")");
      no_mount = "the game mounts here, but no writable layer could be put over it (" + why + ")";
    }
  }

  if (l.overlay_mounted) return;

  // Either FUSE is unavailable or the overlay refused. Extract once and play
  // from a real tree; what the game wrote is worked out at exit instead.
  if (l.image_mounted) {
    kg::run({"fusermount3", "-u", l.image.string()});
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
    say("unpacking the game (once per install)");
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
    const fs::path layer = src->extra_layer;
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
    kg::run({"fusermount3", "-u", "-z", merged.string()});
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

  // What fuse-overlayfs said, when it said anything: it used to be the other
  // way round, and the one case with a reason printed "()".
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

fs::path extraction_stamp_file(const fs::path& dir) {
  fs::path d = dir;
  if (!d.has_filename()) d = d.parent_path();
  return d.parent_path() / (d.filename().string() + ".stamp");
}

bool may_unpack_into(const fs::path& dir, const std::string& id) {
  std::error_code ec;
  const fs::file_status st = fs::symlink_status(dir, ec);
  // Nowhere, or an empty directory: anybody's to fill.
  if (!fs::exists(st)) return true;
  if (fs::is_directory(st) && fs::is_empty(dir, ec) && !ec) return true;
  // A tree an unpack made has its stamp beside it - a finished one's, or the
  // placeholder an unfinished one leaves. The cache a game is unpacked into
  // when nobody says where is kretro's whatever is in it: one made before
  // there were stamps has none.
  if (fs::exists(extraction_stamp_file(dir), ec)) return true;
  return !id.empty() && dir.lexically_normal() == game_extract_dir(id).lexically_normal();
}

void unpack_body(const Pack& pack, const fs::path& dir) {
  // Before anything is looked at, let alone removed. The directory may be one
  // a person named - --extract-to ~/Games puts the game at ~/Games/<id> - and
  // the id is the bundle author's choice: ~/Games/<id> may be somebody's own
  // copy of the same game, and replacing it whole is not what "unpack into"
  // says.
  if (!may_unpack_into(dir, pack.meta().id)) {
    throw std::runtime_error(dir.string() + " is already there, and kretro did not unpack it, so it is "
                             "left alone. Unpack somewhere else, or move that directory aside.");
  }
  fs::path tool = dwarfs_tool();
  if (tool.empty()) throw std::runtime_error("no DwarFS tool to unpack with");
  const Header& h = pack.header();
  if (!h.has_body()) throw std::runtime_error(pack.meta().id + " carries no game to unpack");
  std::error_code ec;
  const fs::path stamp_file = extraction_stamp_file(dir);
  // The stamp goes first, so an unpack interrupted halfway leaves a cache
  // that describes nothing and is redone rather than played. Written over
  // rather than removed: a stamp that matches no pack still says the tree
  // beside it is an unpack's, which the next unpack may replace.
  write_extraction_stamp(stamp_file, "unpacking");
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (ec) throw std::runtime_error("cannot create " + dir.string() + ": " + ec.message());

  // A pack that is a file of its own ends with its body, so the tool can be
  // pointed at the offset. One inside a player is followed by the next game
  // or the table of contents, which dwarfsextract would read as more image:
  // it has no counterpart to the mount's imagesize.
  fs::path image = pack.path();
  uint64_t offset = h.body_off;
  fs::path copied;
  std::error_code sz;
  const uint64_t whole = fs::file_size(pack.path(), sz);
  if (pack.base() != 0 || sz || h.body_off + h.body_len != whole) {
    copied = fs::path(stamp_file).replace_extension(".image");
    pack.extract_body(copied);
    image = copied;
    offset = 0;
  }
  ProcResult r = kg::run({tool.string(), "--tool=dwarfsextract", "-i", image.string(), "-O",
                          std::to_string(offset), "-o", dir.string()});
  if (!copied.empty()) fs::remove(copied, ec);
  if (!r.ok()) {
    fs::remove_all(dir, ec);
    throw std::runtime_error("could not unpack the game:\n" + r.out);
  }
  write_extraction_stamp(stamp_file, extraction_stamp(pack.meta(), h));
}

// Three facts, one line, and every one of them is a reason to unpack again: the
// layout says where the game sits inside the unpacked tree, the body hash says
// which bytes were unpacked, and the Merkle root says which install they are.
// An id says none of that, and an id was all the cache used to hold.
std::string extraction_stamp(const Meta& m, const Header& h) {
  return (m.rooted() ? std::string("rooted") : std::string("flat")) + " " + to_hex(m.body.blake3) +
         " " + to_hex(h.blake3_root);
}

bool extraction_stamp_matches(const fs::path& stamp_file, const std::string& stamp) {
  std::ifstream f(stamp_file);
  if (!f) return false;
  std::string got;
  std::getline(f, got);
  return got == stamp;
}

void write_extraction_stamp(const fs::path& stamp_file, const std::string& stamp) {
  std::error_code ec;
  fs::create_directories(stamp_file.parent_path(), ec);
  std::ofstream f(stamp_file, std::ios::trunc);
  if (!f) throw std::runtime_error("cannot write the unpacked game's stamp");
  f << stamp << "\n";
}

void close_layers(Layers& l) {
  // Innermost first. The overlay's lowerdir is inside the image.
  if (l.overlay_mounted) {
    kg::run({"fusermount3", "-u", l.merged.string()});
    l.overlay_mounted = false;
  }
  if (l.image_mounted) {
    kg::run({"fusermount3", "-u", l.image.string()});
    l.image_mounted = false;
  }
}

fs::path snapshot(const std::string& id, const fs::path& upper) {
  std::error_code ec;
  if (!fs::exists(upper, ec) || fs::is_empty(upper, ec)) return {};

  fs::create_directories(gen_dir(id), ec);
  int next = 0;
  for (const fs::directory_entry& de : fs::directory_iterator(gen_dir(id), ec)) {
    next = std::max(next, std::atoi(de.path().filename().c_str()));
  }
  char name[16];
  std::snprintf(name, sizeof(name), "%04d", next + 1);
  fs::path dst = gen_dir(id) / name;
  size_t files = 0;
  if (!link_tree(upper, dst, &files)) {
    // Half a snapshot is worse than none: it looks like a generation, it is
    // offered as one, and restoring it would put the game back into a state it
    // was never in. So it is removed and the caller told - which matters most
    // to the caller that takes a snapshot precisely so that it may then delete
    // something.
    fs::remove_all(dst, ec);
    throw std::runtime_error("could not snapshot what " + id + " has written");
  }
  return files ? dst : fs::path{};
}

std::vector<std::string> generations(const std::string& id) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(gen_dir(id), ec)) {
    if (de.is_directory(ec)) out.push_back(de.path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

void restore(const std::string& id, const std::string& generation) {
  std::error_code ec;
  fs::path src = gen_dir(id) / generation;
  if (!fs::exists(src, ec)) throw std::runtime_error("no such generation: " + generation);
  // The live directory this is about to replace is the upper layer of a
  // running session's overlay, if there is one. Replacing it underneath a
  // playing game loses whatever that game has written since it started and
  // leaves it writing into a directory that is no longer there.
  GameLock lock = lock_game(id);
  if (lock.busy()) {
    throw std::runtime_error(id + " is being played by another kretro right now, and its "
                                  "saves are what would be replaced. Close that one first.");
  }
  fs::path live = live_dir(id);
  // The current state becomes a generation of its own first, so restoring is
  // never the move that loses something. This throws when that copy did not
  // complete, and it has to: the whole reason it is taken is that what comes
  // next may be the only other copy.
  snapshot(id, live);

  // Assembled beside the live directory and moved into place only once every
  // file has arrived. It used to be: delete the saves, then copy - and the
  // copy reported success whatever happened to it, so a generation on a
  // filesystem that had just filled up took the live saves with it and left
  // whatever part of itself had landed.
  fs::path staged = game_saves_dir(id) / "restoring";
  fs::remove_all(staged, ec);
  size_t n = 0;
  if (!link_tree(src, staged, &n)) {
    fs::remove_all(staged, ec);
    throw std::runtime_error("could not lay out " + generation + "; nothing was replaced");
  }

  // Two renames within one directory, so there is no moment where neither copy
  // exists. If the second fails the first is undone and the live saves are
  // exactly where they were.
  fs::path replaced = game_saves_dir(id) / "restoring.old";
  fs::remove_all(replaced, ec);
  ec.clear();
  const bool had_live = fs::exists(live, ec);
  if (had_live) {
    fs::rename(live, replaced, ec);
    if (ec) {
      // Read before the cleanup, which has an error_code of its own to report
      // and would otherwise overwrite the one being complained about.
      const std::string why = ec.message();
      std::error_code back;
      fs::remove_all(staged, back);
      throw std::runtime_error("could not move the live saves aside: " + why);
    }
  }
  ec.clear();
  fs::rename(staged, live, ec);
  if (ec) {
    const std::string why = ec.message();
    std::error_code back;
    if (had_live) fs::rename(replaced, live, back);
    fs::remove_all(staged, back);
    throw std::runtime_error("could not put " + generation + " in place: " + why);
  }
  fs::remove_all(replaced, ec);
}

void write_record(const std::string& id, const Record& r) {
  std::error_code ec;
  fs::create_directories(journal_dir(id), ec);
  char name[64];
  std::snprintf(name, sizeof(name), "%010lld.json", static_cast<long long>(r.started));
  std::ofstream f(journal_dir(id) / name);
  f << "{\n"
    << "  \"started\": " << r.started << ",\n"
    << "  \"ended\": " << r.ended << ",\n"
    << "  \"seconds\": " << (r.ended - r.started) << ",\n"
    << "  \"runtime\": \"" << json_escape(r.runtime_id) << "\",\n"
    << "  \"files_written\": " << r.files_written << ",\n"
    << "  \"status\": " << r.status << ",\n"
    << "  \"generation\": \"" << json_escape(r.generation) << "\",\n"
    << "  \"screenshot\": \"" << json_escape(r.screenshot) << "\",\n"
    << "  \"note\": \"" << json_escape(r.note) << "\"\n"
    << "}\n";
}

std::vector<Record> journal(const std::string& id) {
  std::vector<Record> out;
  std::error_code ec;
  std::vector<fs::path> files;
  for (const fs::directory_entry& de : fs::directory_iterator(journal_dir(id), ec)) {
    if (de.path().extension() == ".json") files.push_back(de.path());
  }
  std::sort(files.rbegin(), files.rend());
  for (const fs::path& p : files) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    Record r;
    r.started = std::atoll(json_field(s, "started").c_str());
    r.ended = std::atoll(json_field(s, "ended").c_str());
    r.runtime_id = json_field(s, "runtime");
    r.note = json_field(s, "note");
    r.screenshot = json_field(s, "screenshot");
    r.generation = json_field(s, "generation");
    r.files_written = static_cast<size_t>(std::atoll(json_field(s, "files_written").c_str()));
    r.status = std::atoi(json_field(s, "status").c_str());
    out.push_back(r);
  }
  return out;
}

std::string adopt_player_profile(const fs::path& prefix) {
  const fs::path users = prefix / "drive_c" / "users";
  const fs::path player = users / "player";
  std::error_code ec;
  if (!fs::is_directory(users, ec)) return "";
  // symlink_status: a "player" that is a dangling link is still a name taken.
  if (fs::exists(fs::symlink_status(player, ec))) return "";
  std::vector<fs::path> old;
  for (const fs::directory_entry& de : fs::directory_iterator(users, ec)) {
    const std::string n = de.path().filename().string();
    if (n == "Public" || n == "Default" || n == "All Users" || n == "player") continue;
    std::error_code le;
    if (de.is_symlink(le) || !de.is_directory(le)) continue;
    old.push_back(de.path());
  }
  if (old.size() != 1) return "";
  const fs::path was = old.front();
  fs::rename(was, player, ec);
  if (ec) return "";
  // Relative, so the prefix can move with its link still right.
  fs::create_directory_symlink("player", was, ec);
  return "the Wine profile C:\\users\\" + was.filename().string() + " is now C:\\users\\player" +
         (ec ? std::string() : ", and the old name still leads to it");
}

void prepare_prefix(const rt::Env& e, const fs::path& prefix, const fs::path& home,
                    const Meta& m, bool apply_registry,
                    const std::function<void(const std::string&)>& say,
                    const fs::path& system_tree) {
  std::error_code ec;
  fs::create_directories(prefix, ec);
  fs::create_directories(home / ".config", ec);
  if (std::string moved = adopt_player_profile(prefix); !moved.empty()) say(moved);

  rt::Env we = e;
  we.set("WINEPREFIX", prefix.string());
  rt::confine_home(we, home);
  if (!m.runtime.dlloverrides.empty()) we.set("WINEDLLOVERRIDES", m.runtime.dlloverrides);

  // A prefix that has never been initialised takes about a minute, and done
  // lazily at launch that is indistinguishable from a hang.
  if (!fs::exists(prefix / ".kretro-ready", ec)) {
    fs::path wine = rt::find_wine(e.root);
    rt::Env be = we;
    // Without these, wineboot puts up a dialog offering to install Mono and
    // Gecko and waits for someone to click it - which, run from a launcher,
    // looks exactly like a hang. None of these games are .NET or HTML.
    be.set("WINEDLLOVERRIDES", "mscoree,mshtml=");
    // A prefix copied from the runtime's template was initialised when the
    // runtime was built, and carries the Wine version it was made by. Running
    // wineboot --init over it again is a minute spent redoing that.
    if (!fs::exists(prefix / ".kretro-wine-version", ec)) {
      say("preparing the Wine prefix (first run, about a minute)");
      ProcResult r = rt::run(be, wine, {"wineboot", "--init"}, ProcOptions{{}, "", true, 600});
      if (!r.ok()) throw std::runtime_error("could not build the Wine prefix:\n" + r.out);
    }
    // The stamp is written only on success, so an interrupted init is
    // rebuilt rather than used.
    // The graphics driver is pinned to x11 because the game talks to our own
    // Xwayland. Left to itself Wine may pick winewayland, connect to nothing,
    // and render into a void that looks exactly like a black screen.
    rt::run(be, wine, {"reg", "add", "HKCU\\Software\\Wine\\Drivers", "/v", "Graphics",
                       "/d", "x11", "/f"});
    std::ofstream(prefix / ".kretro-ready") << m.runtime.id << "\n";
  }

  // The Windows version a game expects. Left at Wine's default a 2002
  // installer sees Windows 10 and refuses outright - one says "OS not
  // supported for this product" and quits - so this is applied on
  // every prepare, not only at first init, because it is per-game.
  if (!m.run.windows_version.empty()) {
    fs::path wine = rt::find_wine(e.root);
    ProcResult r = rt::run(we, wine, {"winecfg", "/v", m.run.windows_version});
    if (r.ok()) say("windows version: " + m.run.windows_version);
    else say("warning: could not set the windows version to " + m.run.windows_version);
  }

  // The virtual desktop is set in the registry rather than passed as
  // `explorer /desktop=`. The command-line form returns as soon as the desktop
  // exists, so the launcher sees a one-second session and the game is orphaned;
  // the registry form applies to the process itself, which then blocks until
  // the game actually quits.
  {
    fs::path wine = rt::find_wine(e.root);
    std::string geom = std::to_string(m.run.width ? m.run.width : 800) + "x" +
                       std::to_string(m.run.height ? m.run.height : 600);
    rt::run(we, wine, {"reg", "add", "HKCU\\Software\\Wine\\Explorer\\Desktops", "/v",
                       "kretro", "/d", geom, "/f"});
    rt::run(we, wine, {"reg", "add", "HKCU\\Software\\Wine\\Explorer", "/v", "Desktop",
                       "/d", "kretro", "/f"});
  }

  // The files the installer wrote outside the game directory, put back before
  // the fragment that names them. Ordering is the whole of it: wineboot has
  // just finished laying down its own C:, and the fragment below registers COM
  // classes and DLL paths against files that have to be there when it does.
  if (!system_tree.empty() && fs::exists(system_tree, ec)) {
    size_t n = install::restore_system_files(system_tree, prefix / "drive_c");
    if (n) say("restoring " + std::to_string(n) + " files the installer left outside the game");
  }

  // The keys the installer wrote, put back. install::apply_fragment has existed
  // and had no caller since it was written; this is the call. It runs after
  // wineboot, because there is no registry to import into before that, and it
  // runs once per prefix - the marker holds the fragment's hash, so a game that
  // writes its own settings between launches keeps them.
  if (apply_registry && !m.registry.fragment.empty() &&
      !install::registry_marker_matches(prefix, m.registry.fragment)) {
    say("restoring the registry keys the installer wrote");
    install::apply_fragment(e, prefix, m.registry.fragment);
    install::write_registry_marker(prefix, m.registry.fragment);
  }
}

namespace {

// Waits for `pid` and returns its status. A function rather than a bare
// waitpid because it is used twice: for the program we launched, and for the
// forked wineserver -w that outlives it and is the real "the installer has
// finished" signal.
//
// It used to poll the CPU burned by every process in the Wine prefix, on the
// theory that an installer sitting on "please insert disc 2" burns none and
// a copy burns fifty ticks a second, so the gap between them could be read as
// "it is waiting for a person, press the button for it". Three successive
// commits failed to find a threshold that was right on both sides of that gap.
// There is a person now, and the question does not arise.
int wait_watching_idle(pid_t pid) {
  int status = 0;
  while (true) {
    if (waitpid(pid, &status, WNOHANG) == pid) break;
    usleep(500000);
  }
  return status;
}

}  // namespace

bool set_title_art(const fs::path& frames, const fs::path& frame, bool provisional) {
  std::error_code ec;
  const fs::path title = frames / "title.png";
  // The install's own mark, and the only thing that lets a later frame in.
  const fs::path standin = frames / "title.provisional";
  const bool mine = !fs::exists(title, ec) || (!provisional && fs::exists(standin, ec));
  if (!mine) return false;
  fs::create_directories(frames, ec);
  fs::copy_file(frame, title, fs::copy_options::overwrite_existing, ec);
  if (ec) return false;
  if (provisional) {
    std::ofstream f(standin);
    f << "this frame is of the installer, not the game\n";
  } else {
    fs::remove(standin, ec);
  }
  return true;
}

CompositorResult run_in_compositor(const rt::Env& e, const rt::Env& wine_env,
                                  const fs::path& prog, const std::vector<std::string>& args,
                                  const fs::path& cwd, const CompositorOptions& opt,
                                  const std::function<void(const std::string&)>& say) {
  CompositorResult result;
  std::error_code ec;
  const fs::path& home = opt.home;

  const uint32_t w = opt.width ? opt.width : 800;
  const uint32_t h = opt.height ? opt.height : 600;
  const uint32_t s = opt.scale ? opt.scale : 1;

  // The desktop shell, with its helper clients pointed at our copies, and
  // Weston's own Xwayland switched off - we run our own, below.
  //
  // kiosk-shell would be the natural choice for a single fullscreen game, and
  // it is what the design called for, but Weston 14's Xwayland window manager
  // asserts on a map request from a shell that creates no frames
  // (weston_wm_handle_map_request: window->frame_id != XCB_WINDOW_NONE) and
  // takes the session down with it. The desktop shell is stable with X
  // clients; its only problem was that it spawns helpers from paths compiled
  // in at build time, and those are configurable.
  std::ofstream(home / ".config" / "weston.ini")
      << "[core]\nshell=desktop-shell.so\nxwayland=false\nidle-time=0\nrequire-input=false\n"
      << "[shell]\n"
      << "client=" << (e.root / "usr/libexec/weston-desktop-shell").string() << "\n"
      << "panel-position=none\nbackground-color=0xff000000\nanimation=none\n"
      << "[input-method]\n"
      << "path=" << (e.root / "usr/libexec/weston-keyboard").string() << "\n";

  std::string socket = "kretro-" + opt.socket_suffix;
  std::vector<std::string> wargs;
  // Weston runs as an ordinary client of whatever the host has, which is what
  // keeps this on the well-trodden path on both Wayland and X11 hosts - unless
  // nobody is meant to see it, in which case it renders into a buffer and no
  // window appears anywhere. Our own Xwayland is a client of this Weston
  // either way, for the reason given above the weston.ini: we want a root
  // window that is one surface, and no X window manager in the picture.
  wargs.push_back(opt.headless
                      ? "--backend=headless"
                      : (env("WAYLAND_DISPLAY") ? "--backend=wayland" : "--backend=x11"));
  wargs.push_back("--socket=" + socket);
  wargs.push_back("--debug");  // enables the capture protocol the journal uses
  if (opt.headless) {
    // No --scale here. A headless output is read back pixel for pixel by
    // whoever is drawing it, and scaling it in the compositor would only make
    // the same picture cost more to copy across the X connection.
    wargs.push_back("--width=" + std::to_string(w));
    wargs.push_back("--height=" + std::to_string(h));
    say("display: headless " + std::to_string(w) + "x" + std::to_string(h) +
        " - drawn inside our own window");
  } else if (opt.fullscreen) {
    wargs.push_back("--fullscreen");
    say("display: fullscreen, the game renders at " + std::to_string(w) + "x" + std::to_string(h));
  } else {
    wargs.push_back("--width=" + std::to_string(w));
    wargs.push_back("--height=" + std::to_string(h));
    wargs.push_back("--scale=" + std::to_string(s));
    say("display: " + std::to_string(w) + "x" + std::to_string(h) + " at " + std::to_string(s) +
        "x -> " + std::to_string(w * s) + "x" + std::to_string(h * s) + " window");
  }

  fs::path weston = rt::which(e, "weston");
  if (weston.empty()) throw std::runtime_error("no weston in this runtime");
  fs::path weston_log = home / "weston.log";
  fs::remove(weston_log, ec);
  // A Weston that was killed rather than shut down leaves its socket behind.
  // Left there, we would see the socket, believe the compositor is up, and
  // then watch Xwayland fail to connect to a dead endpoint.
  {
    const char* xr = env("XDG_RUNTIME_DIR");
    fs::path base = xr ? fs::path(xr) : fs::path("/tmp");
    fs::remove(base / socket, ec);
    fs::remove(base / (socket + ".lock"), ec);
  }
  rt::Env wenv = wine_env;
  wenv.set("WESTON_CONFIG_FILE", (home / ".config" / "weston.ini").string());

  std::time_t started = std::time(nullptr);

  pid_t wpid = kg::fork_tied(SIGTERM);
  if (wpid < 0) throw std::runtime_error("cannot fork");
  if (wpid == 0) {
    int fd = open(weston_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    setsid();
    rt::exec(wenv, weston, wargs);
  }

  struct WestonGuard {
    pid_t pid;
    ~WestonGuard() {
      if (pid > 0) { kill(pid, SIGTERM); int st; waitpid(pid, &st, 0); }
    }
  } wguard{wpid};

  const char* xdg = env("XDG_RUNTIME_DIR");
  fs::path sock = fs::path(xdg ? xdg : "/tmp") / socket;
  // The socket appearing is necessary but not sufficient: check the process is
  // still alive, so a compositor that started and immediately died is reported
  // as such rather than as a puzzling failure two steps later.
  bool up = wait_for([&] {
    int st = 0;
    if (waitpid(wpid, &st, WNOHANG) == wpid) { wguard.pid = 0; return true; }
    return fs::exists(sock, ec);
  }, 120, 250);
  if (!up || wguard.pid == 0) {
    std::ifstream log(weston_log);
    std::stringstream ss; ss << log.rdbuf();
    throw std::runtime_error("Weston did not start:\n" + ss.str().substr(0, 2000));
  }

  // Our own Xwayland, run as an ordinary client of that Weston and *not*
  // rootless, so its root window is a single surface: the game's private
  // screen, integer-scaled by Weston.
  //
  // Weston's built-in Xwayland is not used, because its window manager asserts
  // on a map request from a window it did not frame
  // (weston_wm_handle_map_request: window->frame_id != XCB_WINDOW_NONE) and
  // takes the whole session down. Running the X server ourselves means there is
  // no X window manager in the picture at all, which is exactly right for a
  // game that owns its whole screen.
  int dispnum = -1;
  for (int n = 8; n < 100; ++n) {
    if (!fs::exists("/tmp/.X11-unix/X" + std::to_string(n), ec)) { dispnum = n; break; }
  }
  if (dispnum < 0) throw std::runtime_error("no free X display number");
  std::string display = ":" + std::to_string(dispnum);

  rt::Env xenv = wine_env;
  xenv.set("WAYLAND_DISPLAY", socket);
  fs::path xwayland = rt::which(e, "Xwayland");
  if (xwayland.empty()) throw std::runtime_error("no Xwayland in this runtime");

  pid_t xpid = kg::fork_tied(SIGTERM);
  if (xpid < 0) throw std::runtime_error("cannot fork");
  if (xpid == 0) {
    int fd = open((home / "xwayland.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    setsid();
    rt::exec(xenv, xwayland,
             {display, "-geometry", std::to_string(w) + "x" + std::to_string(h), "-noreset"});
  }
  struct XGuard {
    pid_t pid;
    ~XGuard() { if (pid > 0) { kill(pid, SIGTERM); int st; waitpid(pid, &st, 0); } }
  } xguard{xpid};

  if (!wait_for([&] { return fs::exists("/tmp/.X11-unix/X" + std::to_string(dispnum), ec); }, 120, 250)) {
    std::ifstream log(home / "xwayland.log");
    std::stringstream ss; ss << log.rdbuf();
    throw std::runtime_error("Xwayland did not start:\n" + ss.str().substr(0, 2000));
  }
  say("private display " + display);

  // The earliest moment anyone else may open this display, and therefore the
  // right one to hand it out. Not after the program starts: the stage wants to
  // be showing a black nested screen before the installer paints its first
  // window, because a person looking at an empty panel for four seconds
  // concludes it is broken and reaches for the mouse.
  //
  // This runs on whatever thread called run_in_compositor - the worker - and
  // the callback's only job is to hand the string to the UI thread.
  if (opt.on_display_ready) opt.on_display_ready(display);

  {
    std::ifstream log(weston_log);
    std::string line;
    while (std::getline(log, line)) {
      size_t p = line.find("GL renderer:");
      if (p == std::string::npos) continue;
      std::string r = line.substr(p + 12);
      while (!r.empty() && r.front() == ' ') r.erase(r.begin());
      if (r.rfind("llvmpipe", 0) == 0) {
        say("warning: software rendering (" + r + ") - run kretro doctor");
      } else {
        say("renderer: " + r);
      }
      break;
    }
  }

  rt::Env genv = wine_env;
  genv.set("DISPLAY", display);
  // Wine must not find a Wayland display, or it may talk to the host
  // compositor directly and escape the nested screen entirely.
  genv.set("WAYLAND_DISPLAY", "");

  // A helper that photographs the nested screen while the game runs. The first
  // stable frame becomes the game's tile art - unless an install left a
  // stand-in there, which the first play replaces; the most recent one is what
  // you are shown when you come back months later and cannot remember where
  // you were. Neither needs anything from the game itself, because we own the
  // compositor it is drawing into.
  fs::path frames = opt.capture_dir;
  fs::create_directories(frames, ec);
  pid_t cpid = -1;
  if (opt.capture && !frames.empty()) {
    fs::path shooter = rt::which(e, "weston-screenshooter");
    if (!shooter.empty()) {
      cpid = kg::fork_tied(SIGKILL);
      if (cpid == 0) {
        setsid();
        rt::Env se = wine_env;
        se.set("WAYLAND_DISPLAY", socket);
        fs::path scratch = frames / ".capture";
        for (int i = 0;; ++i) {
          sleep(i == 0 ? static_cast<unsigned>(opt.first_capture_after) : 60);
          fs::remove_all(scratch, ec);
          fs::create_directories(scratch, ec);
          ProcOptions po2;
          po2.capture = true;
          po2.cwd = scratch.string();
          if (!rt::run(se, shooter, {}, po2).ok()) continue;
          for (const fs::directory_entry& de : fs::directory_iterator(scratch, ec)) {
            if (de.path().extension() != ".png") continue;
            fs::copy_file(de.path(), frames / "last.png",
                          fs::copy_options::overwrite_existing, ec);
            if (opt.keep_every_frame) {
              char stamp[32];
              std::snprintf(stamp, sizeof(stamp), "frame-%03d.png", i);
              fs::copy_file(de.path(), frames / stamp, fs::copy_options::overwrite_existing, ec);
            }
            // Write-once, except over a stand-in. The install leaves a marker
            // beside the frame it took of the installer; the first play finds
            // it, replaces the tile with a frame of the actual game and takes
            // the marker away, and every play after that leaves both alone.
            const fs::path title = frames / "title.png";
            const fs::path standin = frames / "title.provisional";
            const bool take_it = !fs::exists(title, ec) ||
                                 (!opt.title_is_provisional && fs::exists(standin, ec));
            if (take_it) {
              fs::copy_file(de.path(), title, fs::copy_options::overwrite_existing, ec);
              if (opt.title_is_provisional) std::ofstream(standin) << "the installer\n";
              else fs::remove(standin, ec);
            }
          }
        }
        _exit(0);
      }
    }
  }
  struct CaptureGuard {
    pid_t pid;
    ~CaptureGuard() { if (pid > 0) { kill(-pid, SIGKILL); int st; waitpid(pid, &st, 0); } }
  } cguard{cpid};

  say("launching " + prog.filename().string());
  std::vector<std::string> gargs = args;
  // The game is forked here rather than through the process helper, because a
  // helper beside it needs its pid: that is what pause-on-focus-loss stops and
  // what tells the gamepad mapper when to go away.
  //
  // A group of its own only for the caller that has to kill one. `on_pgid` is
  // that caller and the only one: "abandon this install" cannot wait for an
  // installer that is waiting for a person, so it signals the whole group.
  // Playing a game asks for no such thing, and must not be given it - a game
  // in a group of its own is no longer in the terminal's foreground group, so
  // Ctrl-C during `kretro play` reaches kretro and never the game. That is the
  // one key everybody uses to stop a program, and it stopped working the day
  // this became unconditional.
  const bool own_group = static_cast<bool>(opt.on_pgid);
  pid_t gpid = fork();
  if (gpid < 0) throw std::runtime_error("cannot fork");
  if (gpid == 0) {
    // setpgid rather than setsid: the pid is unchanged, which is what waitpid
    // below and the input helper's --pid both address, and a new session would
    // give away the controlling terminal a CLI install still prints to.
    if (own_group) setpgid(0, 0);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    rt::exec(genv, prog, gargs);
  }
  if (own_group) {
    // Raced with the child on purpose: whichever runs first, the group exists.
    setpgid(gpid, gpid);
    opt.on_pgid(gpid);
  }

  pid_t ipid = -1;
  // Not during an install. The stage is already injecting XTest events into
  // this display from the GUI process, which is also the process holding the
  // gamepad, and two writers on one pointer is a cursor that fights the hand
  // moving it.
  if (const char* app = env("KRETRO_APP"); app && opt.input_helper) {
    ipid = kg::fork_tied(SIGTERM);
    if (ipid == 0) {
      setsid();
      int fd = open((home / "input.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
      rt::Env ie = genv;
      // The state this process resolved, handed down: the helper is kretro or
      // the player again, started with the game's HOME, and a state chosen
      // afresh from that HOME is an empty one under the game's home - with no
      // pack in it for kretro's helper to read the bindings from, and no
      // settings.toml for the player's.
      ie.set("KRETRO_STATE", state_dir().string());
      std::vector<std::string> helper = {"input", "--display", display, "--pid",
                                        std::to_string(gpid)};
      if (!opt.socket_suffix.empty()) {
        helper.push_back("--game");
        helper.push_back(opt.socket_suffix);
      }
      if (!opt.pause_on_blur) helper.push_back("--no-pause");
      rt::exec(ie, app, helper);
    }
  }

  int gstatus = 0;

  if (opt.stop_after > 0) {
    std::time_t deadline = std::time(nullptr) + opt.stop_after;
    while (true) {
      pid_t w = waitpid(gpid, &gstatus, WNOHANG);
      if (w == gpid) break;
      if (std::time(nullptr) >= deadline) {
        kill(gpid, SIGCONT);  // a stopped process cannot act on SIGTERM
        kill(gpid, SIGTERM);
        waitpid(gpid, &gstatus, 0);
        break;
      }
      usleep(100000);
    }
  } else {
    waitpid(gpid, &gstatus, 0);
  }
  if (ipid > 0) { kill(ipid, SIGTERM); int st; waitpid(ipid, &st, 0); }
  result.status = WIFEXITED(gstatus) ? WEXITSTATUS(gstatus) : -1;

  // The program we launched may have been a stub that handed off to another
  // Windows process. wineserver -w blocks until every process in the prefix
  // has finished, which is the only reliable "the installer is done" signal.
  if (opt.wait_for_processes) {
    fs::path ws = rt::which(e, "wineserver");
    if (!ws.empty()) {
      say("waiting for the installer to finish");
      // Forked rather than run, because this is where an InstallShield install
      // actually spends its time - and therefore where a stalled one has to be
      // noticed.
      pid_t wp = kg::fork_tied(SIGTERM);
      if (wp == 0) {
        setsid();
        rt::exec(wine_env, ws, {"-w"});
      }
      if (wp > 0) wait_watching_idle(wp);
    }
  }

  // Wine keeps running after the game exits unless its server is told to stop.
  {
    fs::path ws = rt::which(e, "wineserver");
    if (!ws.empty()) rt::run(wine_env, ws, {"-k"});
  }

  result.seconds = static_cast<uint64_t>(std::time(nullptr) - started);
  return result;
}

// Puts a backend decision into the prefix and the environment: the DLLs it
// brings, the overrides that make Wine load them, and its registry values -
// all written as one .reg file and imported with one Wine start, because each
// `wine reg add` is a wineserver start of its own.
//
// DLLs are copied on every launch rather than once. wineboot -u, which a
// newer runtime runs over an old prefix, puts Wine's own builtins back in
// system32 and would silently turn DXVK off.
static void apply_backend(const rt::Env& e, rt::Env& we, const fs::path& prefix,
                          const player::Settings& s) {
  if (s.refused()) throw std::runtime_error(s.decision.reason);
  const std::string name = player::backend_name(s.decision.backend);
  say("graphics: " + name + " - " + s.decision.reason);
  for (const auto& kv : s.env) we.set(kv.first, kv.second);

  std::error_code ec;
  bool dlls_in = s.dlls.empty();
  if (!s.dlls.empty()) {
    fs::path src;
    for (const std::string& d : s.dll_dirs) {
      if (fs::is_directory(e.root / d, ec)) {
        src = e.root / d;
        break;
      }
    }
    // New WoW64 lays a prefix out as Windows does: 64-bit DLLs in system32,
    // 32-bit ones in syswow64. A 32-bit-only prefix has system32 alone.
    fs::path win = prefix / "drive_c" / "windows";
    const bool wow = fs::is_directory(win / "syswow64", ec);
    const fs::path dir32 = win / (wow ? "syswow64" : "system32");
    size_t n = 0;
    for (const std::string& dll : s.dlls) {
      if (src.empty()) break;
      const std::string f = dll + ".dll";
      for (const auto& [from, to] : {std::pair{src / "x32" / f, dir32},
                                     std::pair{src / "x64" / f, wow ? win / "system32" : fs::path()},
                                     std::pair{src / f, dir32}}) {
        if (to.empty() || !fs::exists(from, ec)) continue;
        fs::copy_file(from, to / f, fs::copy_options::overwrite_existing, ec);
        if (!ec) ++n;
      }
    }
    dlls_in = n > 0;
    if (!dlls_in) say("warning: " + name + " is not in this runtime; Wine's own Direct3D draws instead");
  }
  // Overriding a DLL that was not put there would make Wine look for a native
  // copy, find none, and fall back - or, for ddraw, fail outright.
  if (dlls_in && !s.dll_overrides.empty()) {
    std::string cur;
    for (const auto& kv : we.vars) {
      if (kv.first == "WINEDLLOVERRIDES") cur = kv.second;
    }
    we.set("WINEDLLOVERRIDES", cur.empty() ? s.dll_overrides : cur + ";" + s.dll_overrides);
  }

  if (!s.registry.empty()) {
    // Inside drive_c, so Wine is handed a path it can name without a Z:
    // drive, which a player's prefix does not have.
    fs::path reg = prefix / "drive_c" / "kretro-backend.reg";
    std::ofstream(reg) << player::registry_file(s.registry);
    ProcResult r = rt::run(we, rt::find_wine(e.root), {"regedit", "/S", "C:\\kretro-backend.reg"});
    if (!r.ok()) say("warning: the " + name + " settings could not be written to the registry");
  }
}

Outcome play(const rt::Env& e, const std::string& id, const Options& opt) {
  // A refusal is said before anything is locked, mounted or started: there
  // is nothing for the person to wait through when the answer is already no.
  if (opt.backend && opt.backend->refused()) throw std::runtime_error(opt.backend->decision.reason);
  Outcome outcome;
  auto plan = [&](const std::string& k, const std::string& v) { outcome.plan.emplace_back(k, v); };
  std::error_code ec;
  fs::path pack_path = opt.source ? opt.source->file : games_dir() / (id + ".kgpack");
  if (!fs::exists(pack_path, ec)) {
    throw std::runtime_error(id + " is not installed. Try: kretro install " + id);
  }
  // Before open_layers, because open_layers' first act is to unmount whatever
  // it finds at this game's mountpoints - which, if another kretro is playing,
  // is that game's live filesystem.
  GameLock lock;
  if (!opt.lock_held) {
    lock = lock_game(id);
    if (lock.busy()) {
      throw std::runtime_error(id + " is already being played by another kretro. Close that "
                                    "one first - two of them share one set of saves.");
    }
  }
  Pack pack = opt.source ? Pack::open(pack_path, opt.source->off, opt.source->len)
                         : Pack::open(pack_path);
  const Meta& m = pack.meta();
  // A player's table names each pack by the game it is; a pack that says it
  // is another game would have its saves and prefix filed under the wrong one.
  if (m.id != id) throw std::runtime_error("the pack for " + id + " says it is " + m.id);
  plan("game", m.name.empty() ? id : m.name);
  plan("pack", opt.source ? pack_path.filename().string() + " at " + std::to_string(pack.base())
                          : pack_path.string());

  // The handler is in place before the first mount, not after the last:
  // mounting the image waits up to two seconds, and the overlay after it as
  // long again, and a Ctrl-C or a Stop in that time took the default action
  // and left both mounted - under a scratch HOME's state, for a preview,
  // where no later session would find them to sweep.
  Layers layers;
  struct Guard {
    Layers* l;
    ~Guard() { close_layers(*l); g_active = nullptr; }
  } guard{&layers};
  g_active = &layers;
  struct sigaction sa {};
  sa.sa_handler = release_on_signal;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGINT, SIGTERM, SIGHUP}) sigaction(sig, &sa, nullptr);
  open_layers_into(layers, e, pack, opt.source ? &*opt.source : nullptr);

  say(std::string("game directory: ") +
      (layers.writes_isolated() ? "overlay (writes captured live)" : "extracted (writes found at exit)"));

  plan("game directory", layers.writes_isolated() ? "overlay, writes captured live"
                                                   : "unpacked, writes found at exit");
  plan("mounted at", layers.merged.string());

  std::string exe = resolve_ci(layers.merged, m.run.exe);
  if (exe.empty()) throw std::runtime_error("no " + m.run.exe + " in the installed game");
  plan("exe", exe);

  // How the game is drawn, decided from its own imports when the caller asked
  // for that. Before the prefix is touched: a refusal is still said before
  // anything is written.
  std::optional<player::Settings> backend = opt.backend;
  if (!backend && opt.backend_for) backend = opt.backend_for(layers.merged / exe);
  if (backend && backend->refused()) throw std::runtime_error(backend->decision.reason);

  fs::path prefix = game_prefix_dir(id);
  fs::path home = game_home_dir(id);
  plan("prefix", prefix.string());
  fs::create_directories(prefix, ec);
  fs::create_directories(home / ".config", ec);

  // The fragment records where the game was installed to on the machine that
  // built the pack: C:\Program Files (x86)\<Publisher>\<Game>, and some games
  // read their install directory straight back out of HKLM. At play time the
  // game is not there - it is on the overlay, which Wine reaches as Z:\ - so
  // applying the fragment verbatim would point the game at a directory that
  // does not exist.
  //
  // So the recorded path is made real, as a symlink to the game directory,
  // before prepare_prefix imports anything. Then the fragment applies
  // unaltered, run.exe is still resolved under merged and the cwd is still
  // merged, and nothing else has to move. This is why Meta::Install::install_dir
  // survived the cull of the other five install fields.
  //
  // Meta::decode refuses an install_dir that is not a relative path under
  // drive_c, but this is the line that removes a file and puts a symlink where
  // it was, and a Meta does not only ever come from a decoded pack. The check
  // is made again where the damage would be done.
  if (!m.install.install_dir.empty() && install_dir_is_safe(m.install.install_dir)) {
    fs::path link = prefix / "drive_c" / fs::path(m.install.install_dir);
    fs::create_directories(link.parent_path(), ec);
    // remove, not remove_all: a real directory there is somebody's data and a
    // symlink is all we are entitled to replace.
    fs::remove(link, ec);
    ec.clear();
    fs::create_directory_symlink(layers.merged, link, ec);
    if (ec) say("warning: C:\\" + m.install.install_dir + " could not be pointed at the game");
  }

  rt::Env we = e;
  we.set("WINEPREFIX", prefix.string());
  rt::confine_home(we, home);
  if (!m.runtime.dlloverrides.empty()) we.set("WINEDLLOVERRIDES", m.runtime.dlloverrides);

  // image/system for a rooted pack, and nothing at all for a flat one: revision
  // 1 had nowhere to put these files, so there is nothing to look for.
  session::prepare_prefix(e, prefix, home, m, /*apply_registry=*/true, say,
                          m.rooted() ? layers.image / "system" : fs::path{});

  // Meta.discs has been filled at install time since the format existed, and
  // until now only the CBOR codec and the shelf ever read it. This is what it
  // was for: a game that checks for its disc at runtime - and the whole of this
  // era does - gets the same drives back that it was installed with.
  //
  // The trees come out of the pack, at image/discs/<n>, each already carrying
  // the .windows-label and .windows-serial written when the pack was built.
  // Nothing is written into the saves layer, so nothing new enters a snapshot
  // or a save export.
  if (!m.discs.empty()) {
    // The letter comes from the disc's position, not from how many have been
    // attached so far, because install assigns 'd' + i to every disc it mounts.
    // Advancing only on success would hand disc 2 the letter disc 1 had, and a
    // game that recorded D: at install time would look at the wrong drive.
    for (size_t i = 0; i < m.discs.size() && 'd' + i <= 'z'; ++i) {
      const char letter = static_cast<char>('d' + i);
      fs::path tree = layers.image / "discs" / std::to_string(i + 1);
      if (!m.discs[i].embedded || !fs::exists(tree, ec)) {
        // Honest, and the same state the library already shows for a disc it
        // cannot find: the pack was built without this one, or predates packs
        // carrying their discs at all.
        say("disc " + std::to_string(i + 1) + " (" + m.discs[i].label +
            ") is not in this pack; rebuild it to include the discs");
        continue;
      }
      disc::attach_cdrom(e, prefix, letter, tree);
      say(std::string("  ") + static_cast<char>(std::toupper(letter)) + ": " + m.discs[i].label);
      plan(std::string(1, static_cast<char>(std::toupper(letter))) + ":", "CD-ROM " + m.discs[i].label);
    }
  }

  // The game's own drive, for a prefix with no Z:. A plain symlink, as the
  // discs are; Wine finds the drive a working directory is on by walking up
  // it and comparing each directory with each drive's root, so a game started
  // in merged is started on this drive, at its root.
  std::string dos_dir;
  if (opt.game_drive) {
    const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(opt.game_drive)));
    disc::repoint(prefix, letter, layers.merged);
    dos_dir = std::string(1, static_cast<char>(std::toupper(letter))) + ":\\";
    plan(dos_dir.substr(0, 2), "the game");
  }

  if (opt.after_prefix) opt.after_prefix(we, prefix, layers.merged);

  // dgVoodoo2 wraps DirectX 1 through 7 and Glide, which is the era every game
  // here belongs to - DXVK cannot stand in, it starts at Direct3D 9. It is also
  // not redistributable on the terms this project ships under, so it is not in
  // the runtime. Saying so is better than accepting the flag and doing nothing,
  // which is what happened until now.
  if (opt.dgvoodoo || m.runtime.dgvoodoo) {
    fs::path dg = e.root / "opt" / "dgvoodoo";
    if (fs::exists(dg / "MS" / "x86" / "D3D8.dll", ec)) {
      for (const char* dll : {"D3D8.dll", "DDraw.dll", "D3DImm.dll"}) {
        fs::copy_file(dg / "MS" / "x86" / dll, fs::path(layers.merged) / dll,
                      fs::copy_options::overwrite_existing, ec);
      }
      fs::copy_file(dg / "dgVoodoo.conf", fs::path(layers.merged) / "dgVoodoo.conf",
                    fs::copy_options::overwrite_existing, ec);
      std::string ov = "d3d8,ddraw,d3dim=n";
      if (!m.runtime.dlloverrides.empty()) ov = m.runtime.dlloverrides + ";" + ov;
      we.set("WINEDLLOVERRIDES", ov);
      say("dgVoodoo2: wrapping DirectX with the bundled copy");
    } else {
      say("dgVoodoo2 was asked for but is not in this runtime.");
      say("  It is not redistributable on the terms kretro ships under. Put its");
      say("  MS/x86 DLLs and dgVoodoo.conf under opt/dgvoodoo in the runtime and");
      say("  this will pick them up. Playing without it.");
    }
  }

  // Before the renderer below, so the bake-off's explicit choice still wins.
  if (backend) {
    apply_backend(e, we, prefix, *backend);
    plan("graphics", std::string(player::backend_name(backend->decision.backend)) + " - " +
                         backend->decision.reason);
    plan("display path", player::display_path_name(backend->display));
  }

  if (!opt.renderer.empty()) {
    fs::path wine = rt::find_wine(e.root);
    rt::run(we, wine, {"reg", "add", "HKCU\\Software\\Wine\\Direct3D", "/v", "renderer",
                       "/d", opt.renderer, "/f"});
    say("renderer backend: " + opt.renderer);
  }

  // Where this game goes on this panel: the manifest says what the game renders
  // at, the settings say what to do with it, and the panel says what will fit.
  // The panel size comes from the caller. This layer draws nothing and knows
  // about no display server, and linking SDL into it would drag a window
  // toolkit into the packing tool and the unit tests.
  const uint32_t panel_w = opt.panel_w;
  const uint32_t panel_h = opt.panel_h;
  config::Display want = opt.display_set
                             ? opt.display
                             : config::for_game(config::load(config::config_file()), id);
  config::Geometry geo =
      config::compute_geometry(m.run.width, m.run.height, panel_w, panel_h, want);

  // Native asks the game itself to render at the panel's resolution, so the
  // virtual desktop has to be re-sized after prepare_prefix set it from the
  // manifest.
  if (geo.desktop_is_panel) {
    fs::path wine = rt::find_wine(e.root);
    std::string g = std::to_string(geo.logical_w) + "x" + std::to_string(geo.logical_h);
    rt::run(we, wine, {"reg", "add", "HKCU\\Software\\Wine\\Explorer\\Desktops", "/v",
                       "kretro", "/d", g, "/f"});
    say("native: the game is asked to render at " + g);
  }

  CompositorOptions co;
  co.width = geo.logical_w;
  co.height = geo.logical_h;
  co.scale = geo.scale;
  co.fullscreen = geo.fullscreen || opt.fullscreen;
  co.socket_suffix = id;
  co.home = home;
  co.capture_dir = journal_dir(id);
  co.capture = !opt.no_journal;
  co.pause_on_blur = m.present.pause_on_blur;
  co.stop_after = opt.stop_after;

  fs::path wine2 = rt::find_wine(e.root);
  std::vector<std::string> gargs = {dos_dir.empty() ? exe : dos_dir + exe};
  if (!m.run.args.empty()) {
    std::istringstream as(m.run.args);
    std::string a;
    while (as >> a) gargs.push_back(a);
  }
  {
    std::string cmd;
    for (const std::string& a : gargs) cmd += (cmd.empty() ? "" : " ") + a;
    plan("command", cmd);
    plan("screen", std::to_string(co.width) + "x" + std::to_string(co.height) + " at " +
                       std::to_string(co.scale) + "x" + (co.fullscreen ? ", fullscreen" : ""));
  }
  if (opt.dry_run) {
    say("dry run: everything is ready, and the game is not started");
    return outcome;
  }

  std::time_t started = std::time(nullptr);
  CompositorResult cres;
  if (backend && backend->display == player::DisplayPath::GamescopeDirect) {
    // gamescope is already the compositor, already fullscreen and already
    // scaling; Wine goes straight onto its display. No frames are harvested
    // on this road, because there is no compositor of ours to read them from.
    say("display: gamescope, without a nested compositor");
    ProcOptions po;
    po.cwd = layers.merged.string();
    po.capture = false;
    po.timeout_sec = opt.stop_after;
    ProcResult pr = rt::run(we, wine2, gargs, po);
    cres.status = pr.status;
    // As run_in_compositor does: Wine's server and whatever the game left
    // running outlive the game, with their working directory on the overlay,
    // and the overlay cannot be unmounted or snapshotted cleanly under them.
    fs::path ws = rt::which(e, "wineserver");
    if (!ws.empty()) rt::run(we, ws, {"-k"});
  } else {
    cres = run_in_compositor(e, we, wine2, gargs, layers.merged, co, say);
  }
  outcome.status = cres.status;
  fs::path frames = journal_dir(id);

  if (!opt.capture_to.empty()) {
    fs::create_directories(opt.capture_to.parent_path(), ec);
    fs::copy_file(frames / "last.png", opt.capture_to, fs::copy_options::overwrite_existing, ec);
  }


  std::time_t ended = std::time(nullptr);
  outcome.seconds = static_cast<double>(ended - started);

  if (!opt.no_journal) {
    Record rec;
    rec.started = started;
    rec.ended = ended;
    rec.runtime_id = m.runtime.id;
    rec.note = opt.note;
    rec.status = outcome.status;

    // A snapshot that could not be completed is now said out loud rather than
    // silently filed as a generation, and it must not take the journal entry
    // with it: the session happened, it lasted this long, and that is worth
    // recording whether or not the copy of what it wrote succeeded. Nothing
    // has been deleted here, so there is nothing to undo.
    auto keep = [&](const fs::path& from) {
      try {
        fs::path g = snapshot(id, from);
        if (!g.empty()) { outcome.generation = g; rec.generation = g.filename().string(); }
      } catch (const std::exception& ex) {
        say(std::string("warning: ") + ex.what() + " - no snapshot of this session");
      }
    };

    if (layers.writes_isolated()) {
      // The filesystem already told us exactly what changed.
      Tree after = Tree::from_directory(layers.upper);
      outcome.diff.added = after.entries();
      rec.files_written = after.size();
      keep(layers.upper);
    } else {
      // No overlay, so compare the tree against what the pack says it was.
      Tree after = Tree::from_directory(layers.merged);
      outcome.diff = pack.meta().tree.diff_to(after);
      // The extra layer's files were put there for the game, not by it.
      if (!layers.layered.empty()) {
        auto ours = [&](const TreeEntry& te) {
          return std::find(layers.layered.begin(), layers.layered.end(), te.path) != layers.layered.end();
        };
        for (auto* list : {&outcome.diff.added, &outcome.diff.changed}) {
          list->erase(std::remove_if(list->begin(), list->end(), ours), list->end());
        }
      }
      rec.files_written = outcome.diff.added.size() + outcome.diff.changed.size();
      if (rec.files_written) {
        fs::path staging = game_saves_dir(id) / "staging";
        fs::remove_all(staging, ec);
        for (const auto& list : {outcome.diff.added, outcome.diff.changed}) {
          for (const TreeEntry& te : list) {
            if (te.is_dir()) continue;
            fs::path dst = staging / te.path;
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(layers.merged / te.path, dst, fs::copy_options::overwrite_existing, ec);
          }
        }
        keep(staging);
        fs::remove_all(staging, ec);
      }
    }
    write_record(id, rec);
    outcome.journal_written = true;
  }
  return outcome;
}

}  // namespace kg::session
