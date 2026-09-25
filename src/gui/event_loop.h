// The frame loop the shelf and a player's launcher both run: events in, one
// frame drawn, until the host says it is done.
#pragma once

#include <SDL.h>

#include <string>

#include "window.h"

namespace kg::gui {

// What the loop asks of the program around it.
class Host {
 public:
  virtual ~Host() = default;
  virtual bool quit() const = 0;
  // The window was closed. The host may refuse, or ask first.
  virtual void request_quit() = 0;
  // Every event, after the loop has done its own part with it: ImGui has
  // seen it, a closed window has asked to quit, a new gamepad is open and F11
  // has toggled fullscreen.
  virtual void on_event(const SDL_Event&) {}
  // Draws one frame, between begin_frame and end_frame.
  virtual void frame() = 0;
  // What frame() threw.
  virtual void page_failed(const std::string& what) = 0;
};

// Runs until h.quit(). Installs nothing process-wide: the shelf installs its X
// error handlers before it opens the window, and the player never does.
void run_event_loop(const Window& w, Host& h);

// Notes which device `ev` came from, for input_mode() (focus.h): a key or a
// pad button shows the focus, a click, the wheel or a mouse moved more than a
// few pixels stops insisting on it. run_event_loop calls it for every event.
void note_input(const SDL_Event& ev);

}  // namespace kg::gui
