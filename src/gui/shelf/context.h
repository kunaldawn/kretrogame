// What every page of the shelf shares: the collection as last scanned, the
// one worker a long job runs on, the status line, the filter typed on the
// shelf, and the way from one screen to another.
#pragma once

#include <SDL.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "../../rt/env.h"
#include "../job.h"
#include "../texture.h"
#include "../window.h"
#include "scan.h"

namespace kg::gui::shelf {

// The shelf's screens. The wizard is eight screens inside Create, and the
// Bundles page its own several inside Bundles.
enum class Screen { Shelf,
                    Game,
                    Create,
                    Import,
                    Doctor,
                    Library,
                    Settings,
                    Timeline,
                    Bundles };

struct ShelfContext {
  ShelfContext(const rt::Env& e, SDL_Window* w, SDL_Renderer* r, Fonts f);
  ShelfContext(const ShelfContext&) = delete;
  ShelfContext& operator=(const ShelfContext&) = delete;

  const rt::Env& env;
  // The window the shelf draws in, and hides while a game plays.
  SDL_Window* win;
  Fonts fonts;
  Textures textures;

  std::vector<Entry> entries;
  // The game the Game, Settings and Timeline screens are about.
  size_t selected = 0;
  // `filter` is what has been typed on the shelf. It stays set on every
  // other screen, and Escape clears it wherever it is pressed.
  std::string status, filter;

  // The shelf's worker. Its body captures a page or this context, so the
  // host joins it before either goes.
  Job job;
  // Puts a screen up. The host's router; a page calls it rather than knowing
  // which pages there are.
  std::function<void(Screen)> go;
  // What the host's letter `c` opens from the shelf ('a' the wizard, 'i'
  // Import, 'l' Library, 's' Settings, 'd' Doctor, 'b' Bundles): the same
  // actions typing the letter takes, for the shelf's sidebar to offer the
  // mouse and the pad. Nothing while a job or an install is running.
  std::function<void(char)> nav;
  bool quit = false, quit_after_wizard = false;

  // The collection again, from disk, keeping the selection in range.
  void reload();
  // Counts the reloads: a session played, a game built, imported or
  // uninstalled all end in one, so a page keeping a copy of something else
  // it read from disk reads it again when this changes.
  unsigned generation = 0;
  // Where `id` is in entries, or 0 when it is not there.
  size_t index_of(const std::string& id) const;
  // Plays `id` on the worker, with what the session says in the job's
  // modal: getting a fresh prefix ready takes a minute, and a window that
  // vanished at the click said nothing about it. The window is hidden once
  // the game's own screen is up, and comes back when the game is over.
  // Nothing when a job is running. By value: the entry it came from goes
  // when the collection is read again.
  void play(std::string id);
  // Called every frame by the host, on this thread: hides the window when
  // the session says the game's screen is up.
  void hide_for_game();
  // Called by the modal on a finished job's frame: when that job was a game,
  // the window comes back, the status says how it went, and a session that
  // ended well closes the modal by itself.
  void after_play();
  // Whether the worker is playing a game.
  bool playing() const { return !playing_.empty(); }
  // The Game screen, for `id`.
  void open_game(const std::string& id);
  // A job on the worker, its output in the log modal, so the window keeps
  // drawing through the minutes it may take. Nothing when one is running.
  void run_job(const std::string& title, std::function<void()> fn);

  const Texture* texture(const std::filesystem::path& p) { return textures.file(p); }

 private:
  // The game the worker is playing, or empty. The UI thread's own.
  std::string playing_;
  // Set by the worker when the game's screen is up; the UI thread hides the
  // window, since SDL's window calls are its to make.
  std::atomic<bool> screen_up_{false};
  bool hidden_ = false;
  // What the session came to, written by the worker under the job's lock.
  std::string played_;
};

}  // namespace kg::gui::shelf
