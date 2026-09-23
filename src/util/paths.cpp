#include "paths.h"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace kg {
namespace fs = std::filesystem;

namespace {
const char* env(const char* k) {
  const char* v = std::getenv(k);
  return (v && *v) ? v : nullptr;
}

// Empty for kretro's layout; the bundle id for a player's.
std::string& bundle() {
  static std::string id;
  return id;
}
}  // namespace

fs::path state_dir() {
  // Resolved once. Launching a game changes HOME for the child, and anything
  // that recomputed this afterwards would start answering with a different
  // directory - which is how saves end up somewhere nobody looks.
  static const fs::path resolved = [] {
    if (const char* s = env("KRETRO_STATE")) return fs::path(s);
    if (const char* x = env("XDG_DATA_HOME")) return fs::path(x) / "kretro";
    if (const char* h = env("HOME")) return fs::path(h) / ".local" / "share" / "kretro";
    return fs::path("/tmp") / "kretro";
  }();
  return resolved;
}

fs::path runtime_dir() {
  if (const char* r = env("KRETRO_RUNTIME")) return r;
  return {};
}

fs::path games_dir() { return state_dir() / "games"; }
fs::path runtimes_dir() { return state_dir() / "runtimes"; }
fs::path saves_dir() { return state_dir() / "saves"; }
fs::path prefixes_dir() { return state_dir() / "prefixes"; }
fs::path home_dir() { return state_dir() / "home"; }
fs::path gl_dir() { return state_dir() / "gl"; }
fs::path cache_dir() { return state_dir() / "cache"; }

void use_bundle_layout(const std::string& bundle_id) { bundle() = bundle_id; }
bool bundle_layout() { return !bundle().empty(); }

fs::path runtime_base_dir() {
  if (const char* x = env("XDG_RUNTIME_DIR")) return x;
  // The same directory the bootstrap made and checked is ours; asking it
  // again here costs nothing and keeps the two from disagreeing.
  return fs::path("/tmp") / (".kretro-" + std::to_string(getuid()));
}

fs::path user_cache_dir() {
  if (const char* c = env("XDG_CACHE_HOME")) return c;
  if (const char* h = env("HOME")) return fs::path(h) / ".cache";
  return runtime_base_dir();
}

fs::path game_saves_dir(const std::string& id) {
  return bundle_layout() ? state_dir() / id / "saves" : saves_dir() / id;
}
fs::path game_prefix_dir(const std::string& id) {
  return bundle_layout() ? state_dir() / id / "prefix" : prefixes_dir() / id;
}
fs::path game_home_dir(const std::string& id) {
  return bundle_layout() ? state_dir() / id / "home" : home_dir() / id;
}
std::string state_key(const fs::path& state) {
  // FNV-1a: the name has to come out the same from every build of the player,
  // or a mount left by a killed session is never found to be swept.
  std::string s = state.lexically_normal().string();
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char b[17];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(h));
  return std::string(b, 8);
}

fs::path game_mount_dir(const std::string& id) {
  if (!bundle_layout()) return saves_dir() / id;
  // Keyed by the state as well as the bundle. The lock that keeps two
  // sessions of one game apart is in the state, and a session starts by
  // unmounting whatever it finds at its mount points: two copies of one
  // bundle with two states - one on a stick with its -data beside it, one in
  // Downloads, or kretro previewing a rebuild under a throwaway HOME - would
  // otherwise each take the other's running game for a stale mount.
  return runtime_base_dir() / "kretro" / (bundle() + "-" + state_key(state_dir())) / id;
}
fs::path game_extract_dir(const std::string& id) {
  if (!bundle_layout()) return state_dir() / "extracted" / id;
  // Keyed the same way. On the unpacked path the game writes into this tree
  // and what it wrote is found at exit: two states sharing one copy would
  // each file the other's saves as their own.
  return user_cache_dir() / "kretro" / (bundle() + "-" + state_key(state_dir())) / id;
}

void ensure_state_dirs() {
  std::error_code ec;
  if (bundle_layout()) {
    // A player's state holds one directory per game and its own few files;
    // the shelf's directories would only be empty folders a person has to
    // wonder about.
    fs::create_directories(state_dir(), ec);
    fs::create_directories(cache_dir(), ec);
    return;
  }
  for (const fs::path& p : {games_dir(), runtimes_dir(), saves_dir(), home_dir(),
                            prefixes_dir(), gl_dir(), cache_dir()}) {
    fs::create_directories(p, ec);
  }
}

}  // namespace kg
