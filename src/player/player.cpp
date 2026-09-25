#include "player.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "../backend/policy.h"
#include "../bundle/build.h"
#include "../session/saves_transfer.h"
#include "../util/env.h"
#include "../util/format.h"
#include "../util/paths.h"
#include "../util/pe.h"

namespace kg::player {
namespace fs = std::filesystem;

// ---- the player -------------------------------------------------------------

Player::Player(Bundle b, rt::Env env, StateChoice st, gpu::Report gpu)
    : b_(std::move(b)), env_(std::move(env)), st_(std::move(st)), gpu_(std::move(gpu)) {}

bool Player::no_fuse() const {
  const char* m = env_nonempty("KRETRO_MOUNT_MODE");
  return m && std::string(m) == "extract";
}

fs::path Player::launcher_file() const { return state_dir() / "launcher.toml"; }
fs::path Player::settings_file(const std::string& id) const { return state_dir() / id / "settings.toml"; }
fs::path Player::memo_file() const { return state_dir() / "verified"; }
fs::path Player::unpack_pointer(const std::string& id) const { return state_dir() / id / "unpacked-at"; }

GameSettings Player::settings(const std::string& id) const {
  const bundle::GameMeta& g = game(id);
  return load_game_settings(settings_file(id), default_settings(g.display, g.fullscreen));
}

void Player::save_settings(const std::string& id, const GameSettings& s) const {
  save_game_settings(settings_file(id), s);
}

const bundle::Entry& Player::entry(const std::string& id) const {
  const bundle::Entry* e = b_.pack(id);
  if (!e || !b_.game(id)) {
    throw std::runtime_error("This file has no game called '" + id + "'. It has: " + b_.game_list());
  }
  return *e;
}

const bundle::GameMeta& Player::game(const std::string& id) const {
  entry(id);
  return *b_.game(id);
}

Pack Player::open_pack(const std::string& id) const {
  const bundle::Entry& e = entry(id);
  return Pack::open(b_.self, b_.toc.at(e), e.len);
}

bool Player::verified(const std::string& id) const {
  const bundle::Entry& e = entry(id);
  return pack_verified(memo_file(), id, b_.meta.version, e.blake3);
}

void Player::verify(const std::string& id, const std::function<void(uint64_t, uint64_t)>& progress) const {
  const bundle::Entry& e = entry(id);
  if (pack_verified(memo_file(), id, b_.meta.version, e.blake3)) return;
  bundle::Callbacks cb;
  if (progress) cb.progress = [&](const bundle::Progress& p) { progress(p.done, p.total); };
  Hash got = bundle::hash_range(b_.self, b_.toc.at(e), e.len, cb);
  if (got != e.blake3) {
    std::string msg = "Game " + game(id).name + " is damaged inside this file.";
    msg += b_.meta.games.size() > 1 ? " The other games in it still play; download it again to get "
                                      "this one back."
                                    : " Download it again.";
    throw Damaged(id, msg);
  }
  remember_verified(memo_file(), id, b_.meta.version, e.blake3);
}

UnpackPlan Player::unpack_plan(const std::string& id) const {
  UnpackPlan u;
  Pack pk = open_pack(id);
  std::ifstream ptr(unpack_pointer(id));
  std::string where;
  std::getline(ptr, where);
  std::error_code ec;
  // --extract-to's copy while it is there. Once it is gone - deleted, as
  // --extract-to says undoes it, or on a drive that is not plugged in - the
  // game goes back to where it would have been without it, rather than to a
  // directory that may no longer have anywhere to be made.
  if (!where.empty() && !fs::is_directory(where, ec)) where.clear();
  u.where = where.empty() ? game_extract_dir(id) : fs::path(where);
  const std::string stamp = session::extraction_stamp(pk.meta(), pk.header());
  const fs::path game_tree = pk.meta().rooted() ? u.where / "game" : u.where;
  u.ready = session::extraction_stamp_matches(session::extraction_stamp_file(u.where), stamp) &&
            fs::exists(game_tree, ec);
  // The body is copied out beside the tree first and removed after, so the
  // peak is both.
  u.need = unpacked_estimate(pk.meta()) + pk.header().body_len;
  u.free = free_bytes(u.where);
  return u;
}

fs::path Player::unpack(const std::string& id, const fs::path& dir) const {
  // The game's lock, the one a session holds while it plays. An unpack
  // removes the tree it goes into, and on a machine without FUSE that tree
  // is what a running session plays from and writes into - a newer version
  // of this player shares the older one's state, and so its cache. Two
  // Extracts at once would each remove the other's half-made tree.
  session::GameLock lock = session::lock_game(id);
  if (lock.busy()) {
    throw std::runtime_error(game(id).name + " is being played, or unpacked, by another copy of this "
                             "player. Close that one first.");
  }
  Pack pk = open_pack(id);
  fs::path where = dir.empty() ? unpack_plan(id).where : dir;
  const uint64_t need = unpacked_estimate(pk.meta()) + pk.header().body_len;
  const uint64_t have = free_bytes(where);
  if (have < need) {
    throw std::runtime_error("Unpacking " + game(id).name + " needs " + fmt::bytes_si(need) + " in " +
                             where.parent_path().string() + ", and there is " + fmt::bytes_si(have) + " free.");
  }
  session::unpack_body(pk, where);
  std::error_code ec;
  fs::create_directories(unpack_pointer(id).parent_path(), ec);
  std::ofstream(unpack_pointer(id), std::ios::trunc) << where.string() << "\n";
  return where;
}

const doctor::Inputs& Player::machine() const {
  if (!machine_) {
    doctor::Inputs in = doctor::gather(env_, gpu_);
    in.bundle_id = b_.meta.id;
    in.bundle_title = b_.meta.title;
    in.bundle_version = b_.meta.version;
    in.kretro_version = b_.meta.kretro_version;
    machine_ = std::move(in);
  }
  return *machine_;
}

std::string Player::last_log(size_t lines) const {
  std::vector<fs::path> logs;
  logs.reserve(b_.meta.games.size());
  for (const bundle::GameMeta& g : b_.meta.games) logs.push_back(game_home_dir(g.id) / "weston.log");
  fs::path newest = doctor::newest_file(logs);
  return newest.empty() ? "" : doctor::tail_lines(newest, lines);
}

doctor::Report Player::doctor_report() const {
  doctor::Inputs in = machine();
  doctor::Section where{"this player", {}};
  where.lines.push_back({"file", b_.self.string()});
  where.lines.push_back({"games", std::to_string(b_.meta.games.size()) + " (" + b_.game_list() + ")"});
  where.lines.push_back({"state", state_dir().string() + (st_.portable ? " (beside the file)" : "")});
  if (!st_.refused_portable.empty()) {
    where.lines.push_back({"note", st_.refused_portable.string() + " is there but " + st_.refused_why});
  }
  where.lines.push_back({"runtime", env_.valid() ? env_.root.string() : "none"});
  if (fs::path t = template_dir(env_); !t.empty()) {
    where.lines.push_back({"prefix template", read_stamp(t).value_or("unstamped")});
  } else {
    where.lines.push_back({"prefix template", "none in this runtime"});
  }
  in.extra.push_back(where);
  if (!env_.valid()) {
    in.caps.problems.push_back({gpu::Problem::Severity::Blocking, "this player was started without its runtime",
                                "Run the player file itself, not the program inside it"});
  } else if (rt::find_wine(env_.root).empty()) {
    in.caps.problems.push_back({gpu::Problem::Severity::Blocking, "this player's runtime has no Wine",
                                "The file is damaged; download it again"});
  }
  in.log_tail = last_log(40);
  return doctor::collect(in);
}

session::Outcome Player::play(const std::string& id, const PlayOverrides& ov) const {
  const bundle::GameMeta& g = game(id);
  const bundle::Entry& e = entry(id);
  std::vector<std::pair<std::string, std::string>> pre;

  // A refusal needs no exe: no usable GPU, and the author said this game
  // needs one. Said before anything is hashed, mounted or copied.
  const doctor::Inputs& m = machine();
  const backend::AuthorBackend author = backend::parse_author_backend(g.backend);
  {
    backend::Plan s = backend::plan(author, pe::Imports{}, m.caps, g.needs_gpu);
    if (s.refused()) throw std::runtime_error(s.decision.reason);
  }

  const bool was_verified = verified(id);
  verify(id);
  pre.emplace_back("pack check", was_verified ? "done before, for version " + b_.meta.version
                                              : "BLAKE3 matches the table");

  session::Source src;
  src.file = b_.self;
  src.off = b_.toc.at(e);
  src.len = e.len;
  src.cache_size = kPackCacheSize;
  src.no_fuse = no_fuse();
  src.may_unpack = false;
  // Where an unpacked copy is, or would be: an overlay that fails on a
  // machine that can mount falls back to one too, and it must find the one
  // --extract-to made rather than look in the cache.
  const UnpackPlan u = unpack_plan(id);
  src.extract_dir = u.where;
  if (src.no_fuse) {
    if (!u.ready) {
      throw session::NeedsUnpack(
          "This machine cannot mount " + g.name + " (no FUSE), so it has to be unpacked to play. That "
          "needs " + fmt::bytes_si(u.need) + " in " + u.where.parent_path().string() + " (" + fmt::bytes_si(u.free) +
          " free).");
    }
  }

  // No system bus for anything this game starts. Wine's mount manager asks
  // it for the machine's own drives, gives an optical drive the first free
  // letter - D:, in a fresh prefix, the letter the pack's disc is given - and
  // from then on removes that letter's link whenever the drive is empty: the
  // game's disc vanishes, and a CD check fails on a machine that happens to
  // have a drive. A game this player runs sees the drives it was given and no
  // others, as it sees no Z: to the rest of the machine.
  rt::Env wine_env = env_;
  wine_env.set("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/kretro-no-system-bus");

  // The game's lock, from before anything of the game's is written until the
  // session is over. Seeding copies the template for as long as a minute, and
  // a second start in that time - a desktop entry clicked twice - would see
  // no prefix either, remove the first one's half-made copy and rename its
  // own over it. The extra layer is the lowest the running game's overlay
  // has, and a newer build of this bundle shares the state: restaging it for
  // another build's files rewrote or removed DLLs under a running game. And
  // the lock is not let go for session::play to take again, which let
  // another start in between.
  session::GameLock lock = session::lock_game(id);
  if (lock.busy()) {
    throw std::runtime_error(g.name + " is already being played, or made ready to play, by another "
                             "copy of this player. Close that one first.");
  }

  // The author's files - dgVoodoo's, say - beside the game's executable, as a
  // read-only layer of their own under <state>/<game>/extra: never in the
  // writable layer, so never in the saves, a snapshot or a saves export.
  std::string extra_overrides;
  if (!g.extra_dlls.empty()) {
    const fs::path layer = state_dir() / id / "extra";
    extra_overrides = stage_extra_layer(g.extra_dlls, exe_dir_in_tree(open_pack(id).meta()), layer);
    src.extra_layer = layer;
    pre.emplace_back("the author's files", std::to_string(g.extra_dlls.size()) + ", beside the game's executable");
  } else {
    std::error_code ec;
    fs::remove_all(state_dir() / id / "extra", ec);
  }

  // Every line goes to stderr, and to the launcher's window when it asked.
  const std::function<void(const std::string&)> say = [&ov](const std::string& line) {
    log_line(line);
    if (ov.say) ov.say(line);
  };
  PrefixResult pr = ensure_prefix(wine_env, game_prefix_dir(id), state_dir() / id / "prefix-before", say);
  pre.emplace_back("prefix action", prefix_action_name(pr.action));
  if (!pr.snapshot.empty()) pre.emplace_back("prefix snapshot", pr.snapshot.string());

  const GameSettings gs = settings(id);
  session::PlayRequest req;
  req.id = id;
  req.source = src;
  req.held_lock = &lock;
  req.display.scaling = gs.display;
  req.display.fullscreen = ov.fullscreen_set ? ov.fullscreen : gs.fullscreen;
  req.display.panel_w = ov.panel_w;
  req.display.panel_h = ov.panel_h;
  req.display.usable_w = ov.usable_w;
  req.display.usable_h = ov.usable_h;
  req.dry_run = ov.dry_run;
  const gpu::HostCaps caps = m.caps;
  const bool needs_gpu = g.needs_gpu;
  req.backend.for_exe = [author, caps, needs_gpu](const fs::path& exe) {
    return backend::plan(author, pe::parse_file(exe), caps, needs_gpu);
  };

  // After the discs, which take d: onwards in the order the pack names them.
  size_t discs = 0;
  try {
    discs = open_pack(id).meta().discs.size();
  } catch (const std::exception&) {
  }
  req.game_drive = static_cast<char>(std::max<size_t>('g', 'd' + discs));
  if (req.game_drive > 'y') req.game_drive = 'y';

  const bundle::GameMeta* gm = &g;
  req.hooks.after_prefix = [gm, extra_overrides, say](rt::Env& we, const fs::path& prefix, const fs::path&) {
    drop_host_device_links(prefix);
    if (!extra_overrides.empty()) we.append("WINEDLLOVERRIDES", extra_overrides, ';');
    if (gm->key) apply_embedded_key(we, prefix, *gm->key, say);
  };
  // session::play writes its own lines to stderr, so these are only the rest.
  req.hooks.say = ov.say;
  req.hooks.screen_up = ov.screen_up;

  session::Outcome out = session::play(wine_env, req);
  out.plan.insert(out.plan.begin(), pre.begin(), pre.end());
  out.plan.insert(out.plan.begin(), {"state", state_dir().string()});
  return out;
}

fs::path Player::export_saves(const std::string& id, const fs::path& out) const {
  game(id);
  return session::export_saves(id, out);
}

void Player::import_saves(const std::string& id, const fs::path& in) const {
  game(id);
  session::import_saves(id, in);
}

std::vector<fs::path> Player::license_files() const {
  std::vector<fs::path> out;
  if (!env_.valid()) return out;
  fs::path dir = env_.root / "usr" / "share" / "kretro" / "licenses";
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) out.push_back(de.path());
  std::sort(out.begin(), out.end());
  return out;
}

std::string Player::licenses_text() const {
  std::ostringstream o;
  o << b_.meta.title << " is played by kretro's player"
    << (b_.meta.kretro_version.empty() ? "" : " (kretro " + b_.meta.kretro_version + ")") << ".\n\n";
  o << kSourceNote << "\n\n";
  std::vector<fs::path> files = license_files();
  if (files.empty()) {
    o << "This runtime carries no licence directory.\n";
    return o.str();
  }
  o << "Notices, in " << files.front().parent_path().string() << ":\n";
  for (const fs::path& f : files) o << "  " << f.filename().string() << (fs::is_directory(f) ? "/" : "") << "\n";
  // The hand-written half of SOURCES.txt: each pinned part, its version, and
  // its source. The generated half is one line per Ubuntu package and is in
  // the file for whoever wants all of it.
  std::ifstream src(files.front().parent_path() / "SOURCES.txt");
  if (src) {
    o << "\nWhere the source of each part is (SOURCES.txt):\n";
    std::string line;
    while (std::getline(src, line)) {
      if (line.rfind("# Ubuntu packages", 0) == 0) break;
      if (!line.empty() && line[0] == '#') continue;
      if (!line.empty()) o << "  " << line << "\n";
    }
  }
  o << "\nThe games themselves are distributed by whoever built this file"
    << (b_.meta.rights_acknowledged ? ", who recorded that they have the right to." : ".") << "\n";
  return o.str();
}

}  // namespace kg::player
