// A nested display, drawn inside our own window.
//
// The installer of a 1998 game has to be clicked through by a person. It ran
// in its own window before, and the choice was between a second window the
// user has to find and a fullscreen takeover that hides everything we know
// about what is happening. Neither is what an application does.
//
// So Weston runs headless and nobody sees it; a thread of ours reads the root
// window of the Xwayland inside it, and we draw the result as a panel. What
// the user does over that panel is fed back in through XTest - the same calls
// the gamepad translation uses, for the same reason: there is a person
// driving.
//
// Proved on this runtime before anything was built on it: a headless Weston
// produces an Xwayland root window that XGetImage can read, in 32-bit ARGB,
// with winecfg's window in it. `tests/integration/stage.sh` is that proof and can be run
// again on any machine that doubts it.
//
// XShmGetImage is the obvious next step and would turn the per-frame round trip
// into a shared-memory copy. It costs -lXext in LIBS (mk/config.mk). This
// version does not do it; measure before you add it.
//
// The Xlib error handlers the stage relies on are in x_errors.h, and the
// no-GUI proof `kretro stage-probe` runs is in stage_probe.h.
#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
// DeadDisplays is inline there, and tests/unit/test_stage.cpp reaches it
// through this header.
#include "x_errors.h"

// Deliberately not <X11/Xlib.h> and not <SDL.h>. Xlib defines None, Status,
// True and False as macros, and this header is included by wizard.h, and
// through it by the shelf, all of it full of ImGui. Everything X lives behind
// the Impl below.
struct SDL_Renderer;

namespace kg::gui {

// ---- the parts that are decisions rather than calls -----------------------
//
// Inline, in the header, so that tests/unit/test_stage.cpp can reach them: the test
// binaries link LIB_OBJ and B3_OBJ only and never SDL, X11 or ImGui, which is
// the same rule that keeps the wizard's ranking out of gui/.

enum class PixelShape { Unsupported, Argb8888, Abgr8888 };

// The SDL pixel format an XImage of this shape is already in, or Unsupported
// when uploading it would mean converting every pixel. Little-endian host
// assumed, which every machine this runs on is.
inline PixelShape pixel_shape(int depth, int bits_per_pixel, unsigned long red_mask,
                              unsigned long blue_mask) {
  if ((depth != 24 && depth != 32) || bits_per_pixel != 32) return PixelShape::Unsupported;
  if (red_mask == 0x00ff0000ul && blue_mask == 0x000000fful) return PixelShape::Argb8888;
  if (red_mask == 0x000000fful && blue_mask == 0x00ff0000ul) return PixelShape::Abgr8888;
  return PixelShape::Unsupported;
}

// Where a src_w x src_h picture lands inside a max_w x max_h box, centred and
// letterboxed. x and y are relative to the box's own top-left corner.
struct Fit {
  float x = 0, y = 0, w = 0, h = 0;
};

inline Fit fit_inside(int src_w, int src_h, float max_w, float max_h) {
  Fit f;
  if (src_w <= 0 || src_h <= 0 || max_w <= 0 || max_h <= 0) return f;
  const float s = std::min(max_w / static_cast<float>(src_w), max_h / static_cast<float>(src_h));
  f.w = static_cast<float>(src_w) * s;
  f.h = static_cast<float>(src_h) * s;
  f.x = (max_w - f.w) * 0.5f;
  f.y = (max_h - f.h) * 0.5f;
  return f;
}

// The pixel of the nested display under (px, py), which is in the same box
// coordinates as Fit. False when the point is in the letterbox or outside.
inline bool unmap_point(float px, float py, const Fit& f, int src_w, int src_h, int* x, int* y) {
  if (f.w <= 0 || f.h <= 0) return false;
  const float u = (px - f.x) / f.w;
  const float v = (py - f.y) / f.h;
  if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) return false;
  *x = static_cast<int>(u * static_cast<float>(src_w));
  *y = static_cast<int>(v * static_cast<float>(src_h));
  if (*x >= src_w) *x = src_w - 1;
  if (*y >= src_h) *y = src_h - 1;
  return true;
}

// What went down through the stage and is owed an up.
//
// A press is only a press when the pointer is over the picture: pressing in
// the letterbox around it, or with the panel not focused at all, is not
// pressing anything in the installer. A release is a different question
// entirely. The button is already held inside the nested server, nothing but
// our own up will lift it, and where the pointer happens to be by then does
// not change that - which is what a person doing the most ordinary thing there
// is discovers: dragging an InstallShield scrollbar off the edge of the panel,
// or clicking near the border and letting go a pixel outside it, left the
// installer with a mouse button held down for the rest of its life.
//
// So the presses this stage sent are remembered, and a release is forwarded
// when - and only when - it answers one of them. Buttons and keysyms get a
// ledger each; they are different numbers in different X requests.
class HeldInput {
 public:
  // A press, forwarded only when it is over the picture. True means send it.
  bool press(int code, bool over) {
    if (!over) return false;
    if (!held(code)) down_.push_back(code);
    return true;
  }
  // A release, forwarded wherever the pointer is - but only for something this
  // stage actually put down. A button released over the panel that went down
  // somewhere else belongs to whatever it went down in.
  bool release(int code) {
    for (size_t i = 0; i < down_.size(); ++i) {
      if (down_[i] != code) continue;
      down_.erase(down_.begin() + static_cast<long>(i));
      return true;
    }
    return false;
  }
  bool held(int code) const {
    return std::find(down_.begin(), down_.end(), code) != down_.end();
  }
  // Something is still down, so the stage keeps being asked for input even
  // after the pointer has left it: the up has to reach the installer, and the
  // frame it arrives on is a frame the panel no longer has the mouse over.
  bool any() const { return !down_.empty(); }

 private:
  std::vector<int> down_;
};

// The X keysym for a character the user typed, or 0 for something that is not
// a character at all. Latin-1 keysyms are their own code points - that is the
// whole of the X convention for them - and everything above is the Unicode
// keysym, 0x01000000 | code point.
inline unsigned long keysym_for_char(unsigned int cp) {
  if (cp >= 0x20u && cp < 0x7fu) return cp;
  if (cp >= 0xa0u && cp <= 0xffu) return cp;
  if (cp > 0xffu) return 0x01000000ul | cp;
  return 0;
}

class Stage {
 public:
  Stage(SDL_Renderer* r, const std::string& display);
  ~Stage();                       // stops the capture thread, closes the display

  Stage(const Stage&) = delete;
  Stage& operator=(const Stage&) = delete;

  // Uploads the newest captured frame if there is one. False means the
  // connection is gone; the caller keeps drawing the last frame.
  bool refresh();

  // Draws at up to `max`, preserving aspect, and forwards input.
  void draw(ImVec2 max);

  // True while the panel is what the keyboard is talking to. The caller uses
  // this to take ImGui's keyboard navigation out of the way.
  bool has_focus() const;

  // Empty when all is well; otherwise what went wrong, in a sentence meant for
  // the person in front of the screen. By value, and not by reference: it is
  // written by the capture thread and read by the UI thread, and handing out a
  // reference to a std::string two threads share is a torn read rather than a
  // stale message.
  std::string trouble() const;

 private:
  struct Impl;

  void capture_loop();
  bool grab_frame();
  // `live` is "the panel has the input": presses start only then, releases
  // are owed whatever has happened since.
  void forward_input(ImVec2 origin, const Fit& f, bool live);
  bool drain_input(int budget);   // on the capture thread; false when the display died
  void type_char(unsigned long keysym);
  unsigned char find_spare_keycode();

  std::unique_ptr<Impl> p_;
};

}  // namespace kg::gui
