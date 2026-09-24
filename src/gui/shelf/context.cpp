#include "context.h"

#include <exception>
#include <utility>

#include "../../session/play.h"
#include "../format.h"

namespace kg::gui::shelf {

ShelfContext::ShelfContext(const rt::Env& e, SDL_Window* w, SDL_Renderer* r, Fonts f)
    : env(e), win(w), fonts(f), textures(r) {}

void ShelfContext::reload() {
  entries = gui::scan(env);
  if (selected >= entries.size()) selected = entries.empty() ? 0 : entries.size() - 1;
}

size_t ShelfContext::index_of(const std::string& id) const {
  for (size_t i = 0; i < entries.size(); ++i)
    if (entries[i].id == id) return i;
  return 0;
}

void ShelfContext::play(const std::string& id) {
  // The game gets its own screen; ours would only be in the way.
  SDL_HideWindow(win);
  session::PlayRequest req;
  req.id = id;
  try {
    session::Outcome out = session::play(env, req);
    status = "played for " + human_time(out.seconds);
    if (!out.generation.empty()) status += ", snapshot " + out.generation.filename().string();
  } catch (const std::exception& ex) {
    status = ex.what();
  }
  SDL_ShowWindow(win);
  SDL_RaiseWindow(win);
  textures.forget_matching("/" + id + "/");
  reload();
}

void ShelfContext::open_game(const std::string& id) {
  selected = index_of(id);
  go(Screen::Game);
}

// A stop asked of the last job is not one asked of this.
void ShelfContext::run_job(const std::string& title, std::function<void()> fn) {
  if (job.running()) return;
  job.clear_cancel();
  job.start(title, std::move(fn));
}

}  // namespace kg::gui::shelf
