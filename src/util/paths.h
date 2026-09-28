// Where kretrogame keeps things. One place, so "where did it put my saves"
// has exactly one answer.
//
// Two layouts, one per program. kretro keeps its state by kind of thing -
// every game's saves under saves/, every prefix under prefixes/ - because it is
// a builder with a shelf, and the shelf is the unit a person thinks in. A
// player keeps it by game, because the directory is the bundle's and a person
// who opens it is looking for one game:
//
//   kretro                          a player
//   <state>/saves/<id>/             <state>/<id>/saves/
//   <state>/prefixes/<id>/          <state>/<id>/prefix/
//   <state>/home/<id>/              <state>/<id>/home/
//   <state>/saves/<id>/{image,merged}   $XDG_RUNTIME_DIR/kretro/<bundle>-<key>/<id>/
//   <state>/extracted/<set>/        $XDG_CACHE_HOME/kretro/<bundle>-<key>/<set>/
//
// A player's mount points are never inside its state, because its state may be
// beside the executable on a USB stick, and Ubuntu 25.04 confines fusermount3
// to mount points under $HOME, /tmp and /run/user. Its unpacked games are in
// the cache rather than the state, because they are the file's bytes again and
// nothing a person would miss. <key> is state_key(<state>): the mount points
// and the unpacked copy belong to one state, as the lock that guards them does.
//
// Every caller asks game_*_dir(id) and gets the right one; which layout is in
// force is decided once, at start, by use_bundle_layout.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kg {

// KRETRO_STATE wins (the bootstrap sets it in portable mode, when a
// kretro-data directory sits beside the binary, and a player sets it from its
// own rule before anything asks), then XDG_DATA_HOME, then
// ~/.local/share/kretro.
std::filesystem::path state_dir();

// The mounted or extracted runtime tree, from KRETRO_RUNTIME. Empty when
// kretro was run outside its bootstrap, which is normal during development.
std::filesystem::path runtime_dir();

std::filesystem::path games_dir();     // <state>/games      one index per game
std::filesystem::path packs_dir();     // <state>/packs      one .kgpack per set
std::filesystem::path runtimes_dir();  // <state>/runtimes   pinned capsules
std::filesystem::path saves_dir();     // <state>/saves      overlay uppers
std::filesystem::path prefixes_dir();  // <state>/prefixes   Wine prefixes, disposable
std::filesystem::path home_dir();      // <state>/home       per-game HOME
std::filesystem::path gl_dir();        // <state>/gl         host driver links
std::filesystem::path cache_dir();     // <state>/cache

// A pack holds a media set - every disc once and every game installed from
// them - so a game is found through a one-line index naming its set:
//
//   <state>/games/<id>.set          "s-0123456789abcdef"
//   <state>/packs/<set_id>.kgpack   the set
//
// packs_dir()/<set_id>.kgpack.
std::filesystem::path set_pack(const std::string& set_id);
// games_dir()/<id>.set.
std::filesystem::path game_index(const std::string& id);
// The set the index names, or "" when there is no index or it names something
// that cannot be a set's name: a person can edit it, and it becomes a path.
std::string game_set(const std::string& id);
// The pack an installed game is in: set_pack of its index, or an empty path
// when it has none. Whether that pack still holds the game is for whoever
// opens it to check.
std::filesystem::path game_pack(const std::string& id);
// Writes the index, replacing any that was there.
void write_game_index(const std::string& id, const std::string& set_id);
// Every game with an index, sorted.
std::vector<std::string> indexed_games();

// Switches every game_*_dir below to a player's layout, keyed by `bundle_id`.
// Called once, by the player, before anything else asks where a game lives.
void use_bundle_layout(const std::string& bundle_id);
bool bundle_layout();

// One game's directories, in whichever layout is in force.
std::filesystem::path game_saves_dir(const std::string& id);    // live/, gen/, journal/, lock
std::filesystem::path game_prefix_dir(const std::string& id);
std::filesystem::path game_home_dir(const std::string& id);
std::filesystem::path game_mount_dir(const std::string& id);    // image/ and merged/
// Where a set is unpacked where it cannot be mounted: once per set, and every
// game of it plays from that one copy, each writing under its own
// games/<id>/game - a DVD of three games is unpacked once, not three times.
std::filesystem::path set_extract_dir(const std::string& set_id);

// The session's runtime directory: $XDG_RUNTIME_DIR, or the private
// /tmp/.kretro-<uid> the bootstrap makes when there is none.
std::filesystem::path runtime_base_dir();
// $XDG_CACHE_HOME, or ~/.cache.
std::filesystem::path user_cache_dir();

// Eight hex digits that tell one state directory from another, the same from
// every build: what a player's mount points are keyed by beside its bundle id.
std::string state_key(const std::filesystem::path& state);

// Creates the tree if it is not there yet. Cheap and idempotent.
void ensure_state_dirs();

}  // namespace kg
