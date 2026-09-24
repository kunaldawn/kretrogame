#include "host.h"

#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include "../event_loop.h"
#include "../window.h"
#include "launcher.h"

namespace kg::gui {
namespace launcher {

LauncherHost::LauncherHost(const player::Player& p, const Window& w)
    : ctx_(p, w),
      checking_(ctx_),
      blocked_(ctx_),
      grid_(ctx_),
      game_(ctx_),
      saves_(ctx_),
      display_(ctx_),
      controls_(ctx_),
      bundle_(ctx_),
      licenses_(ctx_),
      about_(ctx_) {
  ctx_.go = [this](Screen s) { pages_.go(s); };
  pages_.set(Screen::Checking, checking_);
  pages_.set(Screen::Blocked, blocked_);
  pages_.set(Screen::Grid, grid_);
  pages_.set(Screen::Game, game_);
  pages_.set(Screen::Saves, saves_);
  pages_.set(Screen::Display, display_);
  pages_.set(Screen::Controls, controls_);
  pages_.set(Screen::Bundle, bundle_);
  pages_.set(Screen::Licenses, licenses_);
  pages_.set(Screen::About, about_);
  ctx_.begin_check();
}

// The jobs capture the context, so the worker is joined before it goes.
LauncherHost::~LauncherHost() {
  ctx_.job.join();
}

void LauncherHost::request_quit() {
  if (ctx_.job.running()) return;  // a check or an unpack is writing; it finishes first
  ctx_.quit = true;
}

void LauncherHost::back() {
  if (ctx_.job.running()) return;
  switch (pages_.current()) {
    case Screen::Saves: case Screen::Display: case Screen::Controls: pages_.go(Screen::Game); break;
    case Screen::Licenses: case Screen::About: pages_.go(Screen::Bundle); break;
    case Screen::Game: case Screen::Bundle:
      if (ctx_.ids.size() > 1) pages_.go(Screen::Grid); else ctx_.quit = true;
      break;
    default: ctx_.quit = true;
  }
}

// Escape and pad B both go back. Dropped files are not handled.
void LauncherHost::on_event(const SDL_Event& ev) {
  if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) back();
  if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_B) back();
}

// A finished job's continuation runs first, on this thread, and may start the
// next job or change the screen. Then the page, then at most one modal over
// it, in this order.
void LauncherHost::frame() {
  ctx_.finish_job();
  pages_.page().draw();
  if (ctx_.job.running()) working_modal(ctx_);
  else if (!ctx_.notes.empty()) notes_modal(ctx_);
  else if (ctx_.offer_desktop) desktop_modal(ctx_);
  else if (ctx_.consent) consent_modal(ctx_);
}

void LauncherHost::remember_window(SDL_Window* win) {
  Uint32 f = SDL_GetWindowFlags(win);
  ctx_.state.window_fullscreen = (f & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
  if (!ctx_.state.window_fullscreen) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    if (ww > 0 && wh > 0) {
      ctx_.state.window_w = static_cast<uint32_t>(ww);
      ctx_.state.window_h = static_cast<uint32_t>(wh);
    }
  }
  ctx_.save_state();
}

}  // namespace launcher

int run_launcher(const player::Player& p) {
  if (!have_display()) {
    std::fprintf(stderr,
                 "%s: no display to draw on.\n"
                 "  This machine has no Wayland or X11 session, so there is no launcher to show.\n"
                 "  From a terminal: --doctor, play <game>, --licenses. --help lists the rest.\n",
                 p.bundle().self.filename().c_str());
    return 1;
  }
  player::LauncherState ls = player::load_launcher(p.launcher_file());
  WindowSpec spec;
  spec.title = p.bundle().meta.title.c_str();
  spec.width = ls.window_w;
  spec.height = ls.window_h;
  spec.fullscreen = ls.window_fullscreen;
  std::string why;
  std::optional<Window> opened = open_window(p.env(), spec, &why);
  if (!opened) {
    std::fprintf(stderr, "%s: %s\n", p.bundle().self.filename().c_str(), why.c_str());
    return 1;
  }
  Window w = *opened;

  {
    launcher::LauncherHost app(p, w);
    run_event_loop(w, app);
    app.remember_window(w.win);
  }
  close_window(w);
  return 0;
}

}  // namespace kg::gui
