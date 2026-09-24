#include "stage_probe.h"

#include "../../session/compositor.h"
#include "../../session/prefix.h"
#include "../../util/paths.h"
#include "stage.h"
#include "x_errors.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace kg::gui {

int stage_probe(const rt::Env& e, int seconds) {
  namespace fs = std::filesystem;
  if (!e.valid() || rt::which(e, "weston").empty() || rt::which(e, "Xwayland").empty() ||
      rt::find_wine(e.root).empty()) {
    std::fprintf(stderr, "skip: this runtime has no weston, Xwayland or wine to try\n");
    return 77;
  }

  install_x_error_handlers();

  std::error_code ec;
  fs::path work = cache_dir() / "stage-probe";
  fs::path prefix = work / "prefix";
  fs::path home = work / "home";
  fs::create_directories(prefix, ec);
  fs::create_directories(home / ".config", ec);

  Meta m;
  m.id = "stage-probe";
  m.run.windows_version = "winxp";
  m.run.width = 800;
  m.run.height = 600;
  auto say = [](const std::string& s) { std::fprintf(stderr, "  %s\n", s.c_str()); };
  // The registry flag comes before `say`, and is false for the same reason as
  // the proving screen's: a scratch prefix for winecfg has no pack behind it
  // and no registry fragment to apply.
  session::prepare_prefix(e, prefix, home, m, /*apply_registry=*/false, say);

  rt::Env we = rt::wine_env(e, prefix, home);

  std::mutex mu;
  std::string display;
  session::CompositorOptions co;
  co.width = 800;
  co.height = 600;
  co.socket_suffix = "stage-probe";
  co.home = home;
  co.capture = false;
  co.pause_on_blur = false;
  co.headless = true;
  co.input_helper = false;
  // Not wait_for_processes: winecfg is not a stub, and this run is ended by the
  // clock rather than by a person closing it.
  co.stop_after = seconds + 15;
  co.on_display_ready = [&](const std::string& d) {
    std::lock_guard<std::mutex> lk(mu);
    display = d;
  };

  int verdict = 1;
  std::string note = "the display never came up";
  // A looker beside the compositor rather than inside on_display_ready: that
  // callback fires before the program is launched, and blocking in it would
  // hold up the very thing we are waiting to photograph.
  std::thread looker([&] {
    std::string d;
    for (int i = 0; i < 240 && d.empty(); ++i) {
      usleep(250000);
      std::lock_guard<std::mutex> lk(mu);
      d = display;
    }
    if (d.empty()) return;
    sleep(static_cast<unsigned>(seconds));

    Display* dpy = XOpenDisplay(d.c_str());
    if (!dpy) { note = "the display " + d + " could not be opened"; return; }
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    XWindowAttributes at{};
    if (!XGetWindowAttributes(dpy, root, &at) || at.width <= 0) {
      note = "the root window of " + d + " has no size";
      XCloseDisplay(dpy);
      return;
    }
    XImage* img = XGetImage(dpy, root, 0, 0, static_cast<unsigned>(at.width),
                            static_cast<unsigned>(at.height), AllPlanes, ZPixmap);
    if (!img) {
      note = "the root window of " + d + " could not be read";
      XCloseDisplay(dpy);
      return;
    }
    const PixelShape shape = pixel_shape(img->depth, img->bits_per_pixel,
                                         static_cast<unsigned long>(img->red_mask),
                                         static_cast<unsigned long>(img->blue_mask));
    // Every seventh pixel: enough to tell a painted screen from a black one,
    // and a fraction of the work.
    long lit = 0, looked_at = 0;
    for (int py = 0; py < at.height; py += 7) {
      for (int px = 0; px < at.width; px += 7) {
        ++looked_at;
        if ((XGetPixel(img, px, py) & 0x00fffffful) != 0) ++lit;
      }
    }
    XDestroyImage(img);
    XCloseDisplay(dpy);

    const double pct = looked_at ? 100.0 * static_cast<double>(lit) / static_cast<double>(looked_at) : 0.0;
    std::printf("stage: %dx%d %s, %.1f%% of pixels not black\n", at.width, at.height,
                shape == PixelShape::Argb8888   ? "argb8888"
                : shape == PixelShape::Abgr8888 ? "abgr8888"
                                                : "UNRECOGNISED LAYOUT",
                pct);
    if (shape == PixelShape::Unsupported) {
      note = "the root window is in a pixel layout the stage cannot upload";
    } else if (pct < 1.0) {
      note = "the root window is uniformly black - Xwayland drew nothing we can see";
    } else {
      verdict = 0;
    }
  });

  session::run_in_compositor(e, we, rt::find_wine(e.root), {"winecfg"}, fs::path{}, co, say);
  looker.join();
  if (verdict != 0) std::fprintf(stderr, "FAIL: %s\n", note.c_str());
  return verdict;
}

}  // namespace kg::gui
