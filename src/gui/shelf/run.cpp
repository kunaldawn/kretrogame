// gui::run: the shelf's window, from opening it to putting it down.
#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include "../../config/config.h"
#include "../event_loop.h"
#include "../stage/x_errors.h"
#include "../window.h"
#include "host.h"
#include "shelf.h"

namespace kg::gui {

int run(const rt::Env& e, const Startup& entry) {
  if (!have_display()) {
    if (entry.create) {
      // Installing means watching an installer click by click; there is no
      // headless form of it left to fall back to, and saying so is better than
      // offering `kretro list` to somebody who asked to install something.
      std::fprintf(stderr,
                   "kretro: no display to draw on.\n"
                   "  Installing a game means watching its installer, and this machine has no\n"
                   "  Wayland or X11 session to watch it in. Run this from a desktop.\n");
    } else {
      std::fprintf(stderr,
                   "kretro: no display to draw on.\n"
                   "  This machine has no Wayland or X11 session, so there is no shelf to show.\n"
                   "  Everything still works from the terminal - try: kretro list\n");
    }
    return 1;
  }

  // Before SDL opens its own X connection, because SDL's X11 backend installs
  // an error handler during SDL_Init and chains to whatever it finds - and
  // because XInitThreads must come before every other Xlib call in the
  // process, this one included.
  install_x_error_handlers();

  // The window opens where it was left, in the mode it was left in.
  config::Config cfg = config::load(config::config_file());
  WindowSpec spec;
  spec.title = "kretro";
  spec.x = cfg.window.x;
  spec.y = cfg.window.y;
  spec.width = cfg.window.width;
  spec.height = cfg.window.height;
  spec.borderless = cfg.window.mode == config::WindowMode::Borderless;
  spec.fullscreen = cfg.window.mode == config::WindowMode::Fullscreen;
  std::string why;
  std::optional<Window> opened = open_window(e, spec, &why);
  if (!opened) {
    std::fprintf(stderr, "kretro: %s\n", why.c_str());
    return 1;
  }
  Window w = *opened;
  SDL_Window* win = w.win;

  // A scope of its own, closing before the shutdown sequence below.
  //
  // The host is a Wizard, a Stage and a cache of SDL textures. ~Wizard touches
  // ImGui::GetIO(), ~Stage destroys an SDL texture and ~Textures destroys the
  // rest of them - all of which need ImGui's context and SDL's video subsystem
  // to still exist. As a plain local of this function it was destroyed at the
  // end of the function, which is after ImGui::DestroyContext() and after
  // SDL_Quit(): every ordinary exit of the shelf ran three destructors over
  // libraries that had already been torn down.
  {
    shelf::ShelfHost host(e, w);
    host.start(entry);
    run_event_loop(w, host);
  }  // the host goes here, while ImGui and SDL are both still up

  // Written once, on the way out, rather than on every drag: a settings file
  // rewritten sixty times a second while somebody resizes is a settings file
  // waiting to be truncated by a crash.
  if (cfg.window.remember_geometry) {
    Uint32 f = SDL_GetWindowFlags(win);
    if (f & SDL_WINDOW_FULLSCREEN_DESKTOP) {
      cfg.window.mode = config::WindowMode::Fullscreen;
    } else {
      cfg.window.mode = (f & SDL_WINDOW_BORDERLESS) ? config::WindowMode::Borderless
                                                    : config::WindowMode::Windowed;
      int ww = 0, wh = 0, wx = 0, wy = 0;
      SDL_GetWindowSize(win, &ww, &wh);
      SDL_GetWindowPosition(win, &wx, &wy);
      if (ww > 0 && wh > 0) {
        cfg.window.width = static_cast<uint32_t>(ww);
        cfg.window.height = static_cast<uint32_t>(wh);
        cfg.window.x = wx;
        cfg.window.y = wy;
      }
    }
    config::save(config::config_file(), cfg);
  }

  close_window(w);
  return 0;
}

}  // namespace kg::gui
