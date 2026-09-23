// Playing a game, and remembering that you did.
//
// The game directory the game sees is a read-only DwarFS base with a writable
// layer on top, so every file the game touches is isolated by the filesystem
// rather than by a per-game list of save paths that somebody has to maintain.
// That single fact is what makes saves visible, snapshottable and exportable.
//
// Where FUSE is unavailable the base is extracted instead and the same answer
// is computed at exit by comparing the tree against the manifest. Slower,
// identical result, works everywhere.
#pragma once

#include <ctime>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <vector>

#include "../pack/kgpack.h"
#include "../pack/tree.h"
#include <functional>

#include "../config/scaling.h"

#include "../rt/env.h"

#include <optional>

#include "../player/policy.h"

namespace kg::session {

struct Layers {
  // The whole body: game/ , system/ , discs/<n>/ and registry.reg for a rooted
  // pack, and the game tree itself for a flat one. This is the mountpoint;
  // `base` is a path inside it, not a mount of its own.
  //
  // Which of those exist varies and none of them is promised: a copy install
  // has no system/, a pack built with the discs left out has no discs/. Every
  // reader here looks before it reads.
  //
  // Mounting the image here rather than at `base` is what makes the discs
  // reachable at all, and it leaves the saves layer exactly as it was: had the
  // overlay been built over the image with the game at merged/game, every write
  // the game made would have landed under upper/game/ and silently changed the
  // layout that live_dir, snapshot, restore and the saves export all assume.
  std::filesystem::path image;
  std::filesystem::path base;    // the game, read-only: image/game when rooted
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

// One kretro per game, for as long as it is playing it.
//
// open_layers begins by sweeping stale mounts: it unmounts whatever is at
// saves/<id>/merged and saves/<id>/image and empties saves/<id>/work. On a
// machine where nothing else is running that sweep is a repair, and it has to
// be - a session killed rather than closed leaves exactly those mounts behind.
// Run while another kretro is playing the same game, it is not a repair at
// all: it pulls the filesystem out from under a running game, and the workdir
// it empties is the one that game's overlay is writing every save through.
// The same holds for restoring a snapshot or importing saves over a live
// layer.
//
// So a session takes an exclusive advisory lock - flock on saves/<id>/lock -
// and holds it until it is done, and a second kretro is told what is going on
// rather than served. The lock is per game id, because two different games
// share nothing here: playing one game while another installs is fine and
// stays fine.
class GameLock {
 public:
  GameLock() = default;
  ~GameLock();
  GameLock(GameLock&& other) noexcept;
  GameLock& operator=(GameLock&& other) noexcept;
  GameLock(const GameLock&) = delete;
  GameLock& operator=(const GameLock&) = delete;

  // Whether another kretro holds this game right now. This is the question to
  // ask; it is not the negation of "we got a file descriptor". A state
  // directory nobody can write to yields no lock file and no lock, and that is
  // a broken installation rather than somebody else playing - refusing to
  // start the game over it would be inventing a second problem.
  bool busy() const { return busy_; }
  void release();

 private:
  friend GameLock lock_game(const std::string& id);
  int fd_ = -1;
  bool busy_ = false;
};

// Where the lock for a game lives. Inside the game's saves directory, beside
// the layers it guards, so removing a game removes its lock with it.
std::filesystem::path lock_file(const std::string& id);

// Takes the lock, or comes back saying who could not have it.
GameLock lock_game(const std::string& id);

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
  // Where an unpacked copy is, or goes: game_extract_dir(id) when empty. A
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
// no temporary file - and puts a writable layer over it. `src`, when given,
// is where the pack came from and how it may be mounted; the offsets are the
// pack's own plus pack.base().
Layers open_layers(const rt::Env& e, const Pack& pack, const Source* src = nullptr);
// The same, into `l` as it goes: each mount is marked in `l` from before it
// is attempted, so a caller that hands `l` to a signal handler first has
// every mount released by it, however far this got.
void open_layers_into(Layers& l, const rt::Env& e, const Pack& pack, const Source* src = nullptr);

// Puts the runtime's fuse-overlayfs over `lower` at `merged`. Empty when it is
// mounted; otherwise what went wrong, in fuse-overlayfs's words.
std::string mount_overlay(const rt::Env& e, const std::filesystem::path& lower,
                          const std::filesystem::path& upper, const std::filesystem::path& work,
                          const std::filesystem::path& merged);
void close_layers(Layers& l);

// Unpacks the pack's whole body into `dir` and stamps it, as the no-FUSE path
// does on its own. The body is first copied out to a file of its own beside
// `dir` when the pack sits inside a larger file: dwarfsextract has no image
// size option and reads whatever follows the image as more of it.
//
// `dir` is replaced whole, so one that holds anything no unpack put there is
// refused: it may be a directory a person named.
void unpack_body(const Pack& pack, const std::filesystem::path& dir);
// Whether an unpack of game `id` may replace `dir`: nothing is there, or an
// empty directory, or a tree with an extraction stamp beside it, or the
// game's own cache directory.
bool may_unpack_into(const std::filesystem::path& dir, const std::string& id);
// Where the stamp for an unpacked tree lives: beside it, never inside it.
std::filesystem::path extraction_stamp_file(const std::filesystem::path& dir);

// The no-FUSE path unpacks the body once and plays out of the unpacked tree.
// The stamp is what tells one unpacked tree from another: the body's layout,
// the body's hash and the tree's Merkle root, on one line.
//
// Without it the cache is keyed on the game's id alone, and an id is the one
// thing a rebuild does not change. Reinstalling a game - or installing it under
// a build that lays the body out differently - would then be played from the
// bytes of the install before it, and a flat tree reached through a rooted
// path, or the reverse, is not reached at all: the game directory silently does
// not exist and the exe is reported missing from a pack that is perfectly good.
//
// The stamp lives beside the unpacked tree, at extracted/<id>.stamp, and never
// inside it. Inside, it is a file the game did not write sitting in the very
// tree that is diffed against meta.tree at exit, and every session would report
// it as written and cut a snapshot generation containing nothing else.
std::string extraction_stamp(const Meta& m, const Header& h);
bool extraction_stamp_matches(const std::filesystem::path& stamp_file, const std::string& stamp);
void write_extraction_stamp(const std::filesystem::path& stamp_file, const std::string& stamp);

struct Options {
  bool fullscreen = false;
  bool dgvoodoo = false;
  bool no_journal = false;
  std::string note;  // recorded against this session

  // Used by the renderer bake-off: run one backend for a fixed time and keep
  // the frame, so a person can look at the results side by side instead of
  // guessing which one is right.
  std::string renderer;                  // wined3d backend: gl, vulkan, gdi
  int stop_after = 0;                    // seconds; 0 plays until you quit
  // How this session should reach the panel. When unset the setting for this
  // game is read from config.toml, so the shelf and the terminal agree.
  config::Display display;
  bool display_set = false;
  // The panel this session will cover, from whoever knows: the shelf has a
  // window, the terminal asks SDL once. Zero means unknown, and then nothing
  // is assumed.
  uint32_t panel_w = 0;
  uint32_t panel_h = 0;
  std::filesystem::path capture_to;      // copy the last frame here

  // How this game is drawn and shown on this machine, as player::plan decided
  // it: DXVK or WineD3D or cnc-ddraw, the Wine settings each implies, and
  // whether to skip the nested compositor under gamescope. Unset, a session
  // runs exactly as it always has. A refusal is thrown before anything runs.
  std::optional<player::Settings> backend;

  // The same decision, made once the game's executable can be read: a player
  // decides "auto" from the exe's imports, and the exe is only there once the
  // pack is mounted. Used when `backend` is unset. A refusal it returns is
  // thrown before anything is started; a caller that can refuse without the
  // exe - no GPU, and the author said the game needs one - should do so
  // before calling play, so nothing is mounted to be told no.
  std::function<player::Settings(const std::filesystem::path& exe)> backend_for;

  // The pack is not state/games/<id>.kgpack but this.
  std::optional<Source> source;

  // The caller already holds lock_game(id), and holds it until play returns:
  // a player takes it before it touches the game's prefix and extra layer,
  // and letting it go for play to take again would let another copy in
  // between. play does not take it a second time - a second flock of the same
  // file from this process would be refused as the other copy's.
  bool lock_held = false;

  // Called once the prefix is ready - registry imported, discs attached - and
  // before the backend and the game. A player applies its author's embedded
  // key and the overrides for its extra DLLs here (the files themselves are
  // Source::extra_layer). `we` is the environment Wine will run under and
  // may be added to; `game` is the directory the game runs in.
  std::function<void(rt::Env& we, const std::filesystem::path& prefix,
                     const std::filesystem::path& game)>
      after_prefix;

  // Give the game directory a drive letter of its own and start the game
  // from it. Zero is kretro's way: the game is reached through Z:, which maps
  // /. A player's prefix has no Z: - a Windows program has no business seeing
  // the whole of somebody's machine - and a game whose working directory is
  // on no drive at all is started in C:\windows by Wine, which most of these
  // games take as their data being missing.
  char game_drive = 0;

  // Everything up to the game: lock, mount, prefix, registry, discs, backend.
  // Then the plan is put in Outcome::plan and nothing is started. The mounts
  // are released on the way out as they would be after a game.
  bool dry_run = false;
};

// One entry in a game's timeline.
struct Record {
  std::time_t started = 0;
  std::time_t ended = 0;
  std::string runtime_id;
  std::string note;
  std::string screenshot;   // relative to the game's journal directory
  size_t files_written = 0;
  int status = 0;
  std::string generation;   // the snapshot this session produced, if any
};

struct Outcome {
  int status = 0;
  double seconds = 0;
  Tree::Diff diff;
  std::filesystem::path generation;
  bool journal_written = false;
  // What was decided, in order, as label and value: filled on every run, and
  // the whole of the result of a dry run.
  std::vector<std::pair<std::string, std::string>> plan;
};

// Everything a Wine prefix needs before a Windows program runs in it: the
// wineboot, the x11 graphics pin, and the Mono/Gecko suppression that stops
// wineboot putting up a dialog nobody is there to click.
//
// `apply_registry` imports Meta.registry.fragment, once per prefix. Play passes
// true - that fragment is the difference between a capsule of files and a game
// that starts. The installer passes false, and must: install::run diffs the
// staging prefix's registry across the installer to *produce* that fragment, and
// a fragment already present on both sides of the diff cancels itself out,
// leaving a rebuilt pack with no registry at all.
//
// `system_tree` is the pack's system/ - what the installer wrote to C: outside
// the game directory. It is copied into the prefix here rather than by the
// caller, because the only correct moment is this one: after wineboot has laid
// down its own C:, and before the fragment that names those files is imported.
// Empty for a prefix with no pack behind it, and for the staging install.
//
// It begins with adopt_player_profile, before anything of Wine's runs in it.
void prepare_prefix(const rt::Env& e, const std::filesystem::path& prefix,
                    const std::filesystem::path& home, const Meta& m, bool apply_registry,
                    const std::function<void(const std::string&)>& say,
                    const std::filesystem::path& system_tree = {});

// Every Wine kretro and the player start is told its user is "player"
// (WINEUSERNAME, rt::make), because the player's prefix template was made as
// "player" and one fixed name is one fewer place a login ends up in a saves
// export. A prefix kretro made before that named its profile after the person
// - C:\users\<login> - and Wine, now running as "player", would make a new
// empty C:\users\player beside it and point the registry's profile there: a
// game's saves in My Documents would seem to vanish.
//
// So the old profile is moved, not abandoned: when drive_c/users holds exactly
// one real profile directory besides Public, and no "player" yet, it is
// renamed to "player" and a symlink with the old name is left pointing at it,
// so a path the registry or a game's own settings recorded in full
// (C:\users\<login>\...) still reaches the same files. Anything else - no old
// profile, several, or a "player" already there - is left exactly as it is:
// guessing which of two profiles holds somebody's saves is not this code's to
// do, and both stay on disk. Returns what it did, in a sentence; empty when it
// did nothing.
std::string adopt_player_profile(const std::filesystem::path& prefix);

// Puts `frame` in as the game's tile art, at frames/title.png, if it is this
// run's to take. Returns whether it did.
//
// Write-once: the first stable frame of a session becomes the tile and every
// frame after it leaves the tile alone, because a tile that changed every
// session would not be a tile. `provisional` is the one exception. An install
// photographs the installer rather than the game, and write-once meant an
// InstallShield dialog was a game's tile for as long as the game was
// installed; so the install marks what it leaves as a stand-in - still better
// than a coloured rectangle for a game not yet played - and the first frame of
// the first real play takes the tile back and removes the mark.
//
// The mark is a file beside the tile rather than a field in the journal: the
// journal is one JSON file per session and this outlives all of them.
bool set_title_art(const std::filesystem::path& frames, const std::filesystem::path& frame,
                   bool provisional);

struct CompositorOptions {
  uint32_t width = 800;
  uint32_t height = 600;
  uint32_t scale = 1;
  bool fullscreen = false;
  std::string socket_suffix;              // distinguishes concurrent sessions
  std::filesystem::path home;             // where weston.ini and the logs go
  std::filesystem::path capture_dir;      // where harvested frames go
  bool capture = true;
  int first_capture_after = 15;           // seconds before the first frame
  bool keep_every_frame = false;          // an install keeps them all, for evidence
  // The first stable frame becomes title.png, the game's tile on the shelf,
  // and then stays: a tile that changed every session would not be a tile.
  //
  // An install is the one run where that frame is not of the game. It is of
  // InstallShield, and write-once meant an InstallShield dialog was the tile
  // for as long as the game was installed. So the install marks what it leaves
  // as a stand-in - better than a coloured rectangle for a game not yet played
  // - and the first frame of the first real play takes the tile back.
  bool title_is_provisional = false;
  bool pause_on_blur = true;
  int stop_after = 0;                     // seconds; 0 waits for the program
  // Some Windows programs are stubs: InstallShield's setup.exe extracts its
  // engine, launches it and exits within seconds. Waiting only for the process
  // we launched would tear the compositor down on top of the real installer.
  bool wait_for_processes = false;         // wait until the prefix is idle

  // Weston with no output of its own. The nested screen still exists, is still
  // drawn into and can still be read - it is simply never presented to the
  // host. This is what lets an installer be a panel inside our own window
  // instead of a second window somebody has to go and find.
  bool headless = false;

  // Fired once, from inside run_in_compositor, the moment Xwayland answers on
  // its socket. That is the earliest instant anything else may open this
  // display, and it is the whole of the coupling between the engine and the
  // stage: the caller's UI thread opens its own connection to this string. An
  // Xlib display is not shared across threads here; each side has one.
  std::function<void(const std::string& display)> on_display_ready;

  // `kretro input` translates a gamepad into XTest events on this display.
  // During an install that is one writer too many: the GUI process is already
  // holding the pad, and the stage is already injecting into that same
  // display, so the two would fight over one pointer. The install path passes
  // false; playing a game leaves it alone.
  bool input_helper = true;

  // The process group the launched program was put in, reported as soon as it
  // exists. An installer waits for a person, so "abandon this install" cannot
  // wait for the installer: it kills this group, the waitpid below returns,
  // and the guards tear the compositor down on the way out.
  //
  // Setting this is also what asks for that group. Unset - which is every
  // caller but the install - the program stays in kretro's own process group,
  // and that is not a detail: a game in a group of its own is not in the
  // terminal's foreground group, so Ctrl-C during `kretro play` would reach
  // kretro and never the game. The two cannot disagree because there is one
  // field, not two.
  std::function<void(pid_t)> on_pgid;
};

struct CompositorResult {
  int status = -1;
  uint64_t seconds = 0;
};

// Runs one Windows program inside our own Weston and Xwayland, and waits for
// it. This is what playing a game and running an installer have in common, and
// it is why an installer run gets screenshots and pause-on-blur for free.
CompositorResult run_in_compositor(const rt::Env& e, const rt::Env& wine_env,
                                   const std::filesystem::path& prog,
                                   const std::vector<std::string>& args,
                                   const std::filesystem::path& cwd,
                                   const CompositorOptions& opt,
                                   const std::function<void(const std::string&)>& say);

Outcome play(const rt::Env& e, const std::string& id, const Options& opt);

// The timeline, newest first.
std::vector<Record> journal(const std::string& id);
void write_record(const std::string& id, const Record& r);

// Copies a tree with hardlinks where it can, so a snapshot of a 600 MB game
// whose save is 2 MB costs 2 MB, and counts the files that arrived.
//
// False when anything did not arrive: a directory that could not be read, a
// file that could neither be linked nor copied, a destination that could not
// be created. It used to return nothing at all and count every file it had
// tried, which made "the copy worked" indistinguishable from "the disk is
// full" to the one caller that then deleted the original.
bool link_tree(const std::filesystem::path& from, const std::filesystem::path& to,
               size_t* files);

// Copies the writable layer into a numbered generation as a hardlink farm, so
// only files that actually changed cost anything. Empty when there was nothing
// to copy; throws when the copy did not complete, having removed what it got -
// half a generation is offered as a whole one and restores a state the game
// was never in.
std::filesystem::path snapshot(const std::string& id, const std::filesystem::path& upper);
std::vector<std::string> generations(const std::string& id);

// Puts a generation back. The live saves become a generation of their own
// first, and are replaced only once the replacement is complete and on disk:
// every failure here leaves the game exactly as it was, and says so.
void restore(const std::string& id, const std::string& generation);

}  // namespace kg::session
