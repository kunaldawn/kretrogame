#include "event_loop.h"

#include <SDL.h>

#include <exception>

#include "imgui_impl_sdl2.h"
#include "window.h"

namespace kg::gui {

void run_event_loop(const Window& w, Host& h) {
  while (!h.quit()) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      ImGui_ImplSDL2_ProcessEvent(&ev);
      if (ev.type == SDL_QUIT) h.request_quit();
      if (ev.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(ev.cdevice.which);
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F11) {
        Uint32 f = SDL_GetWindowFlags(w.win);
        SDL_SetWindowFullscreen(w.win, f & SDL_WINDOW_FULLSCREEN_DESKTOP ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
      }
      h.on_event(ev);
    }

    begin_frame(w);
    // Every page draws from disk, and disk says no: a manifest that moved, a
    // pack whose body cannot be read, a directory that stopped being
    // readable between frames. main() catches what escapes here and exits 1
    // with the message, which is the right end for a program that cannot
    // start and the wrong one for a window somebody is standing in front of.
    try {
      h.frame();
    } catch (const std::exception& e) {
      h.page_failed(e.what());
    }
    end_frame(w);
  }
}

}  // namespace kg::gui
