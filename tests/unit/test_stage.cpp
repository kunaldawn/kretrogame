// Tier 1 unit tests for the stage: no display, no X server, no compositor.
//
// The stage itself cannot be tested here - it needs an X connection and an SDL
// renderer - so what lives in this file is everything about it that is a
// decision rather than a call: the pixel layout it will accept, where a
// picture lands inside a box, and which keysym a character is. Those are the
// parts that fail silently when they are wrong.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

#include "gui/stage/stage.h"
#include "session/compositor.h"
#include "support/check.h"

namespace fs = std::filesystem;
using namespace kg;

// The three fields the stage needs out of the compositor, and their defaults.
// The defaults matter as much as the fields: every existing caller - play, and
// the manifest install path - must keep the behaviour it has today without
// being edited, which means headless is off and the gamepad helper is on
// unless somebody says otherwise.
static void test_compositor_options(const fs::path&) {
  session::CompositorOptions co;
  CHECK(co.headless == false);
  CHECK(co.input_helper == true);
  CHECK(!co.on_display_ready);
  // And the fourth: unset by default, which is the whole of "this program does
  // not go into a process group of its own". Only the install sets it, because
  // only the install has to kill a group; a game left in kretro's group is a
  // game the terminal's Ctrl-C still reaches.
  CHECK(!co.on_pgid);

  std::string seen;
  co.on_display_ready = [&seen](const std::string& d) { seen = d; };
  CHECK(static_cast<bool>(co.on_display_ready));
  co.on_display_ready(":9");
  CHECK_EQ(seen, std::string(":9"));
}

static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

// The one thing that turns a working capture into a screen of garbage. An
// XImage off a typical Xwayland visual is 32 bits per pixel with red in the
// second byte, which on a little-endian host is exactly SDL's ARGB8888 - and
// exactly not the ABGR8888 the texture helper uses for stb-decoded PNGs. Get
// this wrong and the installer is blue where it should be red, which looks
// enough like a theme that somebody will believe it.
static void test_pixel_shape(const fs::path&) {
  CHECK(gui::pixel_shape(24, 32, 0x00ff0000ul, 0x000000fful) == gui::PixelShape::Argb8888);
  CHECK(gui::pixel_shape(32, 32, 0x00ff0000ul, 0x000000fful) == gui::PixelShape::Argb8888);
  CHECK(gui::pixel_shape(24, 32, 0x000000fful, 0x00ff0000ul) == gui::PixelShape::Abgr8888);
  // 16-bit 565, and 24 bits packed into 24: both real, neither uploadable
  // without converting every pixel, so both are refused rather than drawn.
  CHECK(gui::pixel_shape(16, 16, 0x0000f800ul, 0x0000001ful) == gui::PixelShape::Unsupported);
  CHECK(gui::pixel_shape(24, 24, 0x00ff0000ul, 0x000000fful) == gui::PixelShape::Unsupported);
  CHECK(gui::pixel_shape(24, 32, 0x0000ff00ul, 0x000000fful) == gui::PixelShape::Unsupported);
}

// A 4:3 installer inside whatever rectangle the wizard has left over, without
// stretching it: a stretched 640x480 setup dialog is the single clearest sign
// that a thing is emulated rather than run.
static void test_fit_inside(const fs::path&) {
  gui::Fit a = gui::fit_inside(800, 600, 1000, 600);
  CHECK(near(a.w, 800));
  CHECK(near(a.h, 600));
  CHECK(near(a.x, 100));   // letterboxed sideways
  CHECK(near(a.y, 0));

  gui::Fit b = gui::fit_inside(800, 600, 400, 600);
  CHECK(near(b.w, 400));
  CHECK(near(b.h, 300));
  CHECK(near(b.x, 0));
  CHECK(near(b.y, 150));

  gui::Fit c = gui::fit_inside(0, 0, 100, 100);
  CHECK(near(c.w, 0));     // nothing captured yet: draw nothing
}

// The pointer, back into the installer's own coordinates.
static void test_unmap_point(const fs::path&) {
  gui::Fit f = gui::fit_inside(800, 600, 1000, 600);
  int x = -1, y = -1;
  CHECK(gui::unmap_point(500, 300, f, 800, 600, &x, &y));
  CHECK_EQ(x, 400);
  CHECK_EQ(y, 300);

  CHECK(gui::unmap_point(100, 0, f, 800, 600, &x, &y));
  CHECK_EQ(x, 0);
  CHECK_EQ(y, 0);

  // The far corner must land on the last pixel, not one past it: an off-by-one
  // here sends the pointer outside the nested screen, where the click is
  // silently discarded.
  CHECK(gui::unmap_point(899.9f, 599.9f, f, 800, 600, &x, &y));
  CHECK_EQ(x, 799);
  CHECK_EQ(y, 599);

  CHECK(!gui::unmap_point(50, 300, f, 800, 600, &x, &y));   // in the letterbox
  CHECK(!gui::unmap_point(500, -1, f, 800, 600, &x, &y));   // above the panel
}

// More than one install per launch has to work.
//
// The I/O error handler fires at the end of every install: the compositor is
// torn down and the display goes with it, which is the normal way an install
// ends rather than a fault. Were the flag it sets a process-wide bool, every
// later install would see "X is dead" before it had opened anything, and draw
// a black panel forever.
static void test_dead_displays(const fs::path&) {
  gui::DeadDisplays d;
  int a = 0, b = 0;
  const void* first = &a;
  const void* second = &b;

  // Nothing is condemned until something dies, and null is nobody.
  CHECK(!d.dead(first));
  CHECK(!d.dead(nullptr));

  d.mark(first);
  CHECK(d.dead(first));
  // The whole of the bug: a second connection is not the first one, and a
  // sticky bool could not tell them apart.
  CHECK(!d.dead(second));

  // The other half. The condemned Display was freed when its server went away,
  // and the next XOpenDisplay may be handed that same address straight back -
  // at which point it is a live connection wearing a dead one's name, and the
  // stage that just opened it says so.
  d.opened(first);
  CHECK(!d.dead(first));

  // Opening an unrelated connection condemns nothing and clears nothing.
  d.mark(second);
  d.opened(first);
  CHECK(d.dead(second));
  CHECK(!d.dead(first));
}

// A click that slips off the edge of the panel must not leave the installer
// with a mouse button held down for the rest of its life.
//
// Forwarding presses and releases only while the pointer is over the picture
// is not enough: the ordinary things a person does to an InstallShield dialog -
// drag its scrollbar, click near the border and let go a pixel outside it -
// send the down over the picture and the up somewhere else. Nothing in the
// nested X server ever lifts a button but our own up.
static void test_held_input(const fs::path&) {
  gui::HeldInput h;

  // Nothing has gone down, so nothing is owed: a release that answers no press
  // of ours belongs to whatever it went down in.
  CHECK(!h.any());
  CHECK(!h.release(1));

  // A press in the letterbox is not a press in the installer, and it is not
  // remembered either - so the release that follows it is not ours to send.
  CHECK(!h.press(1, /*over=*/false));
  CHECK(!h.any());
  CHECK(!h.release(1));

  // The whole of it: down over the picture, up wherever the drag ended.
  CHECK(h.press(1, /*over=*/true));
  CHECK(h.held(1));
  CHECK(h.any());
  CHECK(h.release(1));
  CHECK(!h.held(1));
  CHECK(!h.any());
  // And once only. A second up would be a button lifted twice.
  CHECK(!h.release(1));

  // Buttons are told apart. Right-dragging off the edge while the left is
  // still down must lift the right one and leave the left alone.
  CHECK(h.press(1, true));
  CHECK(h.press(3, true));
  CHECK(h.release(3));
  CHECK(h.held(1));
  CHECK(!h.held(3));
  CHECK(h.any());

  // A press that repeats before its release does not stack up two ups.
  CHECK(h.press(1, true));
  CHECK(h.release(1));
  CHECK(!h.any());

  // The ledger is what keeps the stage listening after the pointer has left
  // it: with something still down, the panel goes on being asked for input.
  gui::HeldInput keys;
  CHECK(!keys.any());
  CHECK(keys.press(0xff09, /*over=*/true));   // XK_Tab, held down
  CHECK(keys.any());
  CHECK(keys.release(0xff09));
  CHECK(!keys.any());
}

// A CD key is where this matters. Latin-1 characters are their own keysyms;
// everything else is the Unicode keysym, and a control character is not
// something a person typed.
static void test_keysym_for_char(const fs::path&) {
  CHECK_EQ(gui::keysym_for_char('A'), 0x41ul);
  CHECK_EQ(gui::keysym_for_char('-'), 0x2dul);
  CHECK_EQ(gui::keysym_for_char(0x00e9u), 0xe9ul);        // e-acute
  CHECK_EQ(gui::keysym_for_char(0x20acu), 0x010020acul);  // euro
  CHECK_EQ(gui::keysym_for_char(0x0au), 0ul);             // newline is a key, not a character
  CHECK_EQ(gui::keysym_for_char(0x7fu), 0ul);             // delete has no Latin-1 keysym
}

int main() {
  fs::path tmp = fs::temp_directory_path() / "kretro-test-stage";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  try {
    test_compositor_options(tmp);
    test_pixel_shape(tmp);
    test_fit_inside(tmp);
    test_unmap_point(tmp);
    test_keysym_for_char(tmp);
    test_dead_displays(tmp);
    test_held_input(tmp);
  } catch (const std::exception& e) {
    kgtest::unexpected(e);
  }

  fs::remove_all(tmp);
  return kgtest::finish();
}
