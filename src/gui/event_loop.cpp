#include "event_loop.h"

#include <SDL.h>

#include <cstdlib>
#include <exception>

#include "focus.h"
#include "imgui_impl_sdl2.h"
#include "scale.h"
#include "window.h"

namespace kg::gui {

namespace {

// Ctrl and + (on either keyboard's plus, or = which is + unshifted), Ctrl and
// -, Ctrl and 0: the zoom every browser has. The key still goes on to ImGui
// and the host, neither of which gives a Ctrl chord a meaning. The window
// picks the new scale up at the next frame.
//
// On X11, SDL2 also sends the chord's character as text: "-", "=" or "0",
// which the shelf would add to its filter and the installer's stage would
// type into the installer. The caller drops that text (see run_event_loop).
bool zoom_key(const SDL_Event& ev) {
  if (ev.type != SDL_KEYDOWN || !(ev.key.keysym.mod & KMOD_CTRL)) return false;
  switch (ev.key.keysym.sym) {
    case SDLK_EQUALS:
    case SDLK_PLUS:
    case SDLK_KP_PLUS: zoom_in(); return true;
    case SDLK_MINUS:
    case SDLK_KP_MINUS: zoom_out(); return true;
    case SDLK_0:
    case SDLK_KP_0: zoom_reset(); return true;
    default: return false;
  }
}

// Ctrl and the mouse wheel, which would otherwise also scroll the page under
// the pointer: it zooms instead, and ImGui never sees it.
bool zoom_wheel(const SDL_Event& ev) {
  if (ev.type != SDL_MOUSEWHEEL || !(SDL_GetModState() & KMOD_CTRL)) return false;
  int y = ev.wheel.y;
  if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
  if (y > 0) zoom_in();
  else if (y < 0) zoom_out();
  return true;
}

// How far the mouse has moved since a key or a button was last pressed. A
// hand resting on a mouse nudges it; that alone does not say the mouse is in
// use again.
float g_mouse_travel = 0;

bool modifier(SDL_Keycode k) {
  switch (k) {
    case SDLK_LCTRL:
    case SDLK_RCTRL:
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
    case SDLK_LALT:
    case SDLK_RALT:
    case SDLK_LGUI:
    case SDLK_RGUI: return true;
    default: return false;
  }
}

}  // namespace

void note_input(const SDL_Event& ev) {
  switch (ev.type) {
    case SDL_KEYDOWN:
      if (modifier(ev.key.keysym.sym)) return;
      g_mouse_travel = 0;
      set_input_mode(InputMode::Keyboard);
      return;
    case SDL_CONTROLLERBUTTONDOWN:
      g_mouse_travel = 0;
      set_input_mode(InputMode::Pad);
      return;
    case SDL_CONTROLLERAXISMOTION:
      // Past a resting stick's drift.
      if (ev.caxis.value > 12000 || ev.caxis.value < -12000) {
        g_mouse_travel = 0;
        set_input_mode(InputMode::Pad);
      }
      return;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEWHEEL: set_input_mode(InputMode::Mouse); return;
    case SDL_MOUSEMOTION:
      g_mouse_travel += static_cast<float>(std::abs(ev.motion.xrel) + std::abs(ev.motion.yrel));
      if (g_mouse_travel > 4.0f) set_input_mode(InputMode::Mouse);
      return;
    default: return;
  }
}

void run_event_loop(const Window& w, Host& h) {
  // Set by a zoom chord, until the text it brings with it (if any) has been
  // dropped. Any other key clears it, so text typed after a chord is kept.
  bool drop_text = false;
  while (!h.quit()) {
    // Hidden while a game plays on the worker. Events are still taken, so a
    // quit is still heard, but none reaches the page: a pad button pressed
    // in the game would otherwise press whatever the page has focused.
    const bool hidden = (SDL_GetWindowFlags(w.win) & SDL_WINDOW_HIDDEN) != 0;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (hidden) {
        if (ev.type == SDL_QUIT) h.request_quit();
        if (ev.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(ev.cdevice.which);
        if (ev.type == SDL_DROPFILE && ev.drop.file) SDL_free(ev.drop.file);
        continue;
      }
      if (zoom_wheel(ev)) continue;
      if (ev.type == SDL_TEXTINPUT && drop_text) {
        drop_text = false;
        continue;
      }
      if (ev.type == SDL_KEYDOWN) drop_text = false;
      note_input(ev);
      ImGui_ImplSDL2_ProcessEvent(&ev);
      if (zoom_key(ev)) drop_text = true;
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
    // Nothing is shown, and a hidden window's present may not wait for a
    // vertical blank: without this the loop would spin a core for the length
    // of the game.
    if (hidden) SDL_Delay(50);
  }
}

}  // namespace kg::gui
