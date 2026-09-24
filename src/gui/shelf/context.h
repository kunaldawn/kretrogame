// What every page of the shelf shares: the collection as last scanned, the
// one worker a long job runs on, the status line, the filter typed on the
// shelf, and the way from one screen to another.
#pragma once

#include <SDL.h>

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
  bool quit = false, quit_after_wizard = false;

  // The collection again, from disk, keeping the selection in range.
  void reload();
  // Where `id` is in entries, or 0 when it is not there.
  size_t index_of(const std::string& id) const;
  // Plays `id` there and then, in the middle of the frame, with the window
  // hidden until the game is over.
  void play(const std::string& id);
  // The Game screen, for `id`.
  void open_game(const std::string& id);
  // A job on the worker, its output in the log modal, so the window keeps
  // drawing through the minutes it may take. Nothing when one is running.
  void run_job(const std::string& title, std::function<void()> fn);

  const Texture* texture(const std::filesystem::path& p) { return textures.file(p); }
};

}  // namespace kg::gui::shelf
