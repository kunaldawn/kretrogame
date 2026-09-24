#include "x_errors.h"

#include <X11/Xlib.h>

#include <csetjmp>

namespace kg::gui {

thread_local std::jmp_buf g_x_jump;
thread_local bool g_x_armed = false;

namespace {

// A protocol error is not fatal to us. The one we expect is a request against
// a window that the compositor destroyed between our asking about it and the
// server reading the request, and the answer to that is to carry on with the
// last frame we have.
int on_x_error(Display*, XErrorEvent*) { return 0; }

int on_x_io_error(Display* dpy) {
  // Which connection, not "a connection". This is called at the end of every
  // install, because the compositor the installer ran in is torn down and the
  // display goes with it; a process-wide bool set here condemned every display
  // opened afterwards and left the second install of a session staring at a
  // black panel.
  dead_displays().mark(dpy);
  if (g_x_armed) {
    g_x_armed = false;
    std::longjmp(g_x_jump, 1);
  }
  return 0;
}

}  // namespace

DeadDisplays& dead_displays() {
  static DeadDisplays d;
  return d;
}

void install_x_error_handlers() {
  // Before XSetErrorHandler and before SDL: XInitThreads has to be the first
  // Xlib call in the process, and after it the stage's capture thread and the
  // UI thread may share one connection legally.
  XInitThreads();
  XSetErrorHandler(on_x_error);
  XSetIOErrorHandler(on_x_io_error);
}

}  // namespace kg::gui
