// One kretro per game: the advisory lock a session holds on saves/<id>/lock.
#pragma once

#include <filesystem>
#include <string>

namespace kg::session {

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
  friend GameLock lock_path(const std::filesystem::path& file);
  int fd_ = -1;
  bool busy_ = false;
};

// Where the lock for a game lives. Inside the game's saves directory, beside
// the layers it guards, so removing a game removes its lock with it.
std::filesystem::path lock_file(const std::string& id);

// Takes the lock, or comes back saying who could not have it.
GameLock lock_game(const std::string& id);
// The same lock on any file: what a whole shelf's writers share.
GameLock lock_path(const std::filesystem::path& file);

}  // namespace kg::session
