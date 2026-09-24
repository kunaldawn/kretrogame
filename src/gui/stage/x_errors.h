// Xlib's process-wide error handlers, and the record of which connection died.
//
// Only kretro's shelf installs these: the player never opens a stage, and it
// must not call XInitThreads.
#pragma once

#include <atomic>
#include <csetjmp>

namespace kg::gui {

// Installs Xlib's process-wide error handlers, and XInitThreads with them.
//
// Xlib's defaults call exit(). We open a connection to a display that we
// ourselves tear down, so a dead connection here is not an error, it is the
// normal end of an install - and the default handler would take the whole GUI
// down with it. The protocol error handler returns; the I/O error handler
// notes which connection died and jumps back to the call that provoked it.
//
// Called once, from gui::run, before SDL_Init: SDL's X11 backend installs its
// own error handler when the video subsystem comes up and chains to whatever
// it finds, so ours has to already be there. XInitThreads must precede every
// other Xlib call in the process, SDL's included.
void install_x_error_handlers();

// Which X connection has died, by address.
//
// Xlib allows exactly one I/O error handler per process and it cannot return
// normally, so the handler itself has to be process-wide. A dead connection is
// not. The compositor an install runs in is torn down when the install ends,
// which kills the display the stage was reading - an entirely normal event -
// and a single sticky bool then said "X is gone" for the rest of the session.
// Every install after the first drew a permanently black panel: exactly one
// install per launch worked.
//
// So what is remembered is the connection rather than a bool, and opened()
// clears it. The address matters both ways: a Display the handler condemned
// has since been freed, and the next XOpenDisplay may be handed that same
// address back, at which point it is a live connection wearing a dead one's
// name.
class DeadDisplays {
 public:
  // From the I/O error handler, on whichever thread was talking to the server.
  void mark(const void* c) { dead_.store(c, std::memory_order_relaxed); }
  // A new connection. Clears the condemnation if it landed on the same
  // address the old one had.
  void opened(const void* c) {
    if (c && dead_.load(std::memory_order_relaxed) == c) {
      dead_.store(nullptr, std::memory_order_relaxed);
    }
  }
  bool dead(const void* c) const {
    return c != nullptr && dead_.load(std::memory_order_relaxed) == c;
  }

 private:
  std::atomic<const void*> dead_{nullptr};
};

// The one the error handler writes and the stage reads.
DeadDisplays& dead_displays();

// Armed by the stage around every call it makes on its own connection.
//
// The I/O error handler cannot return in the classic Xlib contract - Xlib
// calls exit(1) if it does - so a flag alone would not save us. The only way
// out of a server that has gone away is to jump back to just before the call,
// which is why the guarded regions hold nothing but pointers and integers: a
// longjmp past a destructor would trade one crash for a worse one.
//
// One definition, in x_errors.cpp beside the handler that jumps to it. The
// armed regions are in stage.cpp, and a second copy there would be a buffer
// the handler never jumps to.
extern thread_local std::jmp_buf g_x_jump;
extern thread_local bool g_x_armed;

}  // namespace kg::gui
