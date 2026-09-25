// The window kretro's shelf and a player's launcher both draw in: SDL, a
// renderer, and Dear ImGui with kretro's look and the runtime's fonts.
#pragma once

#include <SDL.h>

#include <cstdint>
#include <optional>
#include <string>

#include "../rt/env.h"
#include "imgui.h"
#include "scale.h"

namespace kg::gui {

// The window both programs draw in. Its fonts are not kept here: they are
// rebuilt whenever the scale changes, so a page asks for them through the
// handle fonts() returns (scale.h) at the moment it draws.
struct Window {
  SDL_Window* win = nullptr;
  SDL_Renderer* ren = nullptr;

  // The fonts open_window loaded, for a page that takes them together.
  Fonts fonts() const { return Fonts{}; }
};

struct WindowSpec {
  const char* title = "kretro";
  int x = -1, y = -1;  // -1 is centred
  uint32_t width = 1280, height = 800;
  bool borderless = false;
  bool fullscreen = false;
};

// SDL, the window, a renderer (accelerated if the driver allows, software if
// not - a window must come up on a machine whose graphics are broken, because
// that is when somebody needs to read what is wrong), ImGui with kretro's look
// and the runtime's fonts, and every gamepad already plugged in. Empty, with
// `why` set, when there is no window to be had.
std::optional<Window> open_window(const rt::Env& e, const WindowSpec& spec, std::string* why);
// Works out the scale for the window as it is now and, when it has changed,
// rebuilds the fonts and the style for it before the frame begins.
void begin_frame(const Window& w);
void end_frame(const Window& w);
// ImGui first, then SDL: the reverse of how they were opened.
void close_window(Window& w);

// Whether this process has a Wayland or X11 session to draw on at all.
bool have_display();

}  // namespace kg::gui
