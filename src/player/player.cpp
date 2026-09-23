#include "player.h"

#include <sys/statvfs.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "../bundle/build.h"
#include "../install/share.h"
#include "../util/paths.h"
#include "../util/pe.h"
#include "policy.h"

namespace kg::player {
namespace fs = std::filesystem;

namespace {

const char* var(const char* k) {
  const char* v = std::getenv(k);
  return (v && *v) ? v : nullptr;
}

std::string read_range(const fs::path& p, uint64_t off, uint64_t len) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("cannot read " + p.string());
  f.seekg(static_cast<std::streamoff>(off));
  std::string s(len, '\0');
  f.read(s.data(), static_cast<std::streamsize>(len));
  if (static_cast<uint64_t>(f.gcount()) != len) throw std::runtime_error(p.string() + " is shorter than its table says");
  return s;
}

std::string human(uint64_t n) {
  const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(n);
  int i = 0;
  while (v >= 1000.0 && i < 4) {
    v /= 1000.0;
    ++i;
  }
  char b[32];
  std::snprintf(b, sizeof(b), i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
  return b;
}

void say(const std::string& s) { std::fprintf(stderr, "  %s\n", s.c_str()); }

}  // namespace

// ---- the file -------------------------------------------------------------

Bundle Bundle::open(const fs::path& self, const std::string& toc_env) {
  Bundle b;
  b.self = self;
  try {
    b.toc = bundle::read_toc(self);
  } catch (const bundle::FormatError& ex) {
    std::error_code ec;
    throw std::runtime_error("This file is damaged or incomplete (" + std::string(ex.what()) +
                             ", and it is " + std::to_string(fs::file_size(self, ec)) +
                             " bytes). Download it again.");
  }
  if (!toc_env.empty()) {
    unsigned long long off = 0, len = 0;
    if (std::sscanf(toc_env.c_str(), "%llu:%llu", &off, &len) != 2 || off != b.toc.toc_off ||
        len != b.toc.toc_len) {
      throw std::runtime_error("This file changed while it was starting. Run it again.");
    }
  }
  const bundle::Entry* m = b.toc.find(bundle::Kind::Meta);
  if (!m) {
    throw std::runtime_error("This is not a player: it carries no games. A player is built by "
                             "kretro, on its Bundles page.");
  }
  std::string raw = read_range(self, b.toc.at(*m), m->len);
  // The bootstrap checked the table; the table's hash of bundle.meta is how
  // the table vouches for these bytes.
  if (hash_string(raw) != m->blake3) {
    throw std::runtime_error("This file is damaged: its description of its games does not match "
                             "itself. Download it again.");
  }
  b.meta = bundle::BundleMeta::decode(raw);
  // Every game bundle.meta lists has a pack, and verify_bundle proved so when
  // the file was built; a file that has lost one since is a damaged file.
  for (const bundle::GameMeta& g : b.meta.games) {
    if (!b.toc.pack(g.id)) {
      throw std::runtime_error("This file is damaged: it names " + g.name + " and does not carry it. "
                               "Download it again.");
    }
  }
  return b;
}

const bundle::GameMeta* Bundle::game(const std::string& id) const {
  for (const bundle::GameMeta& g : meta.games) {
    if (g.id == id) return &g;
  }
  return nullptr;
}

const bundle::Entry* Bundle::pack(const std::string& id) const { return toc.pack(id); }

std::string Bundle::game_list() const {
  std::string s;
  for (const bundle::GameMeta& g : meta.games) s += (s.empty() ? "" : ", ") + g.id;
  return s;
}

StateChoice settle_state(const Bundle& b, const fs::path& exe) {
  StateChoice c = choose_state(exe, b.meta.id, var("XDG_DATA_HOME") ? var("XDG_DATA_HOME") : "",
                               var("HOME") ? var("HOME") : "");
  setenv("KRETRO_STATE", c.dir.c_str(), 1);
  use_bundle_layout(b.meta.id);
  if (!c.private_root.empty()) claim_private_dir(c.private_root);
  std::error_code ec;
  fs::create_directories(c.dir, ec);
  if (ec) {
    throw std::runtime_error("cannot make a place for this player's saves at " + c.dir.string() + ": " +
                             ec.message());
  }
  return c;
}

StateChoice inherited_state(const Bundle& b, const fs::path& exe) {
  const char* s = var("KRETRO_STATE");
  if (!s) return settle_state(b, exe);
  StateChoice c;
  c.dir = s;
  use_bundle_layout(b.meta.id);
  return c;
}

std::map<std::string, std::string> gamepad_bindings(const std::map<std::string, std::string>& pack_input,
                                                    const bundle::GameMeta& g, const GameSettings& s) {
  std::map<std::string, std::string> binds = pack_input;
  if (s.gamepad == "author") {
    for (const auto& [k, v] : bundle::parse_gamepad(g.gamepad)) binds[k] = v;
  }
  return binds;
}

std::vector<std::string> unseen_warnings(const doctor::Report& rep, LauncherState& seen) {
  std::vector<std::string> out;
  for (const gpu::Problem& pr : rep.problems) {
    if (pr.blocking()) continue;
    if (seen.warnings_seen.insert(warning_key(pr.line())).second) out.push_back(pr.line());
  }
  return out;
}

uint64_t unpacked_estimate(const Meta& m) {
  uint64_t n = m.tree.total_bytes() + m.system.bytes;
  bool carries_discs = false;
  for (const Meta::Disc& d : m.discs) carries_discs = carries_discs || d.embedded;
  // A carried disc's tree is about its image's size; the fingerprints are
  // the only sizes the pack records for them.
  if (carries_discs) {
    for (const DiscFingerprint& f : m.recipe.fingerprints) n += f.size;
  }
  return n;
}

uint64_t free_bytes(const fs::path& p) {
  fs::path at = p;
  std::error_code ec;
  while (!at.empty() && !fs::exists(at, ec)) {
    fs::path up = at.parent_path();
    if (up == at) break;
    at = up;
  }
  struct statvfs s {};
  if (::statvfs(at.empty() ? "/" : at.c_str(), &s) != 0) return 0;
  return static_cast<uint64_t>(s.f_bavail) * s.f_frsize;
}

// ---- the player -------------------------------------------------------------

Player::Player(Bundle b, rt::Env env, StateChoice st, gpu::Report gpu)
    : b_(std::move(b)), env_(std::move(env)), st_(std::move(st)), gpu_(std::move(gpu)) {}

bool Player::no_fuse() const {
  const char* m = var("KRETRO_MOUNT_MODE");
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
    throw std::runtime_error("Unpacking " + game(id).name + " needs " + human(need) + " in " +
                             where.parent_path().string() + ", and there is " + human(have) + " free.");
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
  fs::path newest;
  fs::file_time_type when{};
  std::error_code ec;
  for (const bundle::GameMeta& g : b_.meta.games) {
    fs::path log = game_home_dir(g.id) / "weston.log";
    if (!fs::exists(log, ec)) continue;
    fs::file_time_type t = fs::last_write_time(log, ec);
    if (newest.empty() || t > when) {
      newest = log;
      when = t;
    }
  }
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

session::Outcome Player::play(const std::string& id, const PlayRequest& req) const {
  const bundle::GameMeta& g = game(id);
  const bundle::Entry& e = entry(id);
  std::vector<std::pair<std::string, std::string>> pre;

  // A refusal needs no exe: no usable GPU, and the author said this game
  // needs one. Said before anything is hashed, mounted or copied.
  const doctor::Inputs& m = machine();
  const AuthorBackend author = parse_author_backend(g.backend);
  {
    Settings s = plan(author, pe::Imports{}, m.caps, g.needs_gpu);
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
          "needs " + human(u.need) + " in " + u.where.parent_path().string() + " (" + human(u.free) +
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

  PrefixResult pr = ensure_prefix(wine_env, game_prefix_dir(id), state_dir() / id / "prefix-before", say);
  pre.emplace_back("prefix action", prefix_action_name(pr.action));
  if (!pr.snapshot.empty()) pre.emplace_back("prefix snapshot", pr.snapshot.string());

  const GameSettings gs = settings(id);
  session::Options so;
  so.source = src;
  so.lock_held = true;
  so.display = gs.display;
  so.display_set = true;
  so.fullscreen = req.fullscreen_set ? req.fullscreen : gs.fullscreen;
  so.panel_w = req.panel_w;
  so.panel_h = req.panel_h;
  so.dry_run = req.dry_run;
  const gpu::HostCaps caps = m.caps;
  const bool needs_gpu = g.needs_gpu;
  so.backend_for = [author, caps, needs_gpu](const fs::path& exe) {
    return plan(author, pe::parse_file(exe), caps, needs_gpu);
  };

  // After the discs, which take d: onwards in the order the pack names them.
  size_t discs = 0;
  try {
    discs = open_pack(id).meta().discs.size();
  } catch (const std::exception&) {
  }
  so.game_drive = static_cast<char>(std::max<size_t>('g', 'd' + discs));
  if (so.game_drive > 'y') so.game_drive = 'y';

  const bundle::GameMeta* gm = &g;
  so.after_prefix = [gm, extra_overrides](rt::Env& we, const fs::path& prefix, const fs::path&) {
    // A device link a mount manager made before the system bus was taken
    // away - by a player older than this one - still claims its letter, and
    // Wine removes the drive link beside it whenever that device is empty.
    // No game this player runs has a host device, so none of them stay.
    std::error_code dl;
    for (const fs::directory_entry& de : fs::directory_iterator(prefix / "dosdevices", dl)) {
      const std::string n = de.path().filename().string();
      if (n.size() == 3 && n.substr(1) == "::") fs::remove(de.path(), dl);
    }
    if (!extra_overrides.empty()) {
      std::string cur;
      for (const auto& kv : we.vars) {
        if (kv.first == "WINEDLLOVERRIDES") cur = kv.second;
      }
      we.set("WINEDLLOVERRIDES", cur.empty() ? extra_overrides : cur + ";" + extra_overrides);
    }
    if (gm->key) {
      // Once per prefix, like the installer's own keys: a game that later
      // rewrites the value keeps what it wrote.
      std::string reg = key_registry(*gm->key);
      fs::path marker = prefix / ".kretro-key";
      std::string want = to_hex(hash_string(reg));
      std::string have;
      std::ifstream(marker) >> have;
      if (have != want) {
        std::ofstream(prefix / "drive_c" / ".kretro-key.reg", std::ios::trunc) << reg;
        ProcResult r = rt::run(we, rt::find_wine(we.root), {"regedit", "/S", "C:\\.kretro-key.reg"});
        std::error_code ec;
        fs::remove(prefix / "drive_c" / ".kretro-key.reg", ec);
        if (!r.ok()) throw std::runtime_error("could not put the author's key into the registry");
        std::ofstream(marker, std::ios::trunc) << want << "\n";
        say("the author's key is in the registry");
      }
    }
  };

  session::Outcome out = session::play(wine_env, id, so);
  out.plan.insert(out.plan.begin(), pre.begin(), pre.end());
  out.plan.insert(out.plan.begin(), {"state", state_dir().string()});
  return out;
}

fs::path Player::export_saves(const std::string& id, const fs::path& out) const {
  game(id);
  return install::export_saves(id, out, env_);
}

void Player::import_saves(const std::string& id, const fs::path& in) const {
  game(id);
  install::import_saves(id, in);
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
