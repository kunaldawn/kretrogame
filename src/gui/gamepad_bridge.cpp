#include "gamepad_bridge.h"

#include <SDL.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <map>

namespace kg::gui {
namespace {

// What a gamepad means to a game that never heard of one. These are the
// bindings that make sense for a keyboard-and-mouse game of the period; a
// manifest can override them per game.
struct Binding { SDL_GameControllerButton button; KeySym key; };

// The names a manifest may use for a button.
const std::map<std::string, int> kButtonNames = {
    {"up", SDL_CONTROLLER_BUTTON_DPAD_UP},       {"down", SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {"left", SDL_CONTROLLER_BUTTON_DPAD_LEFT},   {"right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    {"a", SDL_CONTROLLER_BUTTON_A},              {"b", SDL_CONTROLLER_BUTTON_B},
    {"x", SDL_CONTROLLER_BUTTON_X},              {"y", SDL_CONTROLLER_BUTTON_Y},
    {"start", SDL_CONTROLLER_BUTTON_START},      {"back", SDL_CONTROLLER_BUTTON_BACK},
    {"l", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},   {"r", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
};

const Binding kDefaults[] = {
    {SDL_CONTROLLER_BUTTON_DPAD_UP, XK_Up},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, XK_Down},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, XK_Left},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XK_Right},
    {SDL_CONTROLLER_BUTTON_A, XK_Return},
    {SDL_CONTROLLER_BUTTON_B, XK_Escape},
    {SDL_CONTROLLER_BUTTON_X, XK_space},
    {SDL_CONTROLLER_BUTTON_Y, XK_Tab},
    {SDL_CONTROLLER_BUTTON_START, XK_Escape},
    {SDL_CONTROLLER_BUTTON_BACK, XK_Tab},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XK_Prior},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XK_Next},
};

constexpr int kStickDeadzone = 9000;
constexpr double kPointerSpeed = 14.0;  // pixels per poll at full deflection

bool alive(pid_t pid) { return pid > 0 && kill(pid, 0) == 0; }

}  // namespace

int run_input(const std::string& display, pid_t game_pid, bool pause_on_blur,
              const std::map<std::string, std::string>& overrides) {
  Display* dpy = XOpenDisplay(display.c_str());
  if (!dpy) return 1;
  int ev = 0, err = 0, major = 0, minor = 0;
  if (!XTestQueryExtension(dpy, &ev, &err, &major, &minor)) {
    XCloseDisplay(dpy);
    return 1;
  }

  if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
    // No gamepad support is not fatal: pausing on focus loss is worth having
    // on its own.
    if (!pause_on_blur) { XCloseDisplay(dpy); return 1; }
  }
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
  }

  std::map<int, KeyCode> keys;
  for (const Binding& b : kDefaults) {
    keys[b.button] = XKeysymToKeycode(dpy, b.key);
  }
  // The manifest's own bindings, over the defaults. An unknown button name or
  // an unknown keysym is skipped rather than fatal: a typo in a manifest
  // should cost you that one button, not the gamepad.
  for (const auto& kv : overrides) {
    auto bit = kButtonNames.find(kv.first);
    if (bit == kButtonNames.end()) continue;
    KeySym ks = XStringToKeysym(kv.second.c_str());
    if (ks == NoSymbol) continue;
    keys[bit->second] = XKeysymToKeycode(dpy, ks);
  }

  bool stopped = false;
  double px = 0, py = 0;

  while (alive(game_pid)) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(e.cdevice.which);
      if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
        auto it = keys.find(e.cbutton.button);
        if (it != keys.end() && it->second) {
          XTestFakeKeyEvent(dpy, it->second, e.type == SDL_CONTROLLERBUTTONDOWN, CurrentTime);
          XFlush(dpy);
        }
      }
    }

    // The left stick drives the pointer, because most of these games are
    // pointer-driven and a d-pad alone will not do.
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
      SDL_GameController* c = SDL_GameControllerFromInstanceID(i);
      if (!c) continue;
      int ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
      int ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
      if (abs(ax) > kStickDeadzone) px += ax / 32768.0 * kPointerSpeed;
      if (abs(ay) > kStickDeadzone) py += ay / 32768.0 * kPointerSpeed;
      if (static_cast<int>(px) || static_cast<int>(py)) {
        XTestFakeRelativeMotionEvent(dpy, static_cast<int>(px), static_cast<int>(py), CurrentTime);
        XFlush(dpy);
        px -= static_cast<int>(px);
        py -= static_cast<int>(py);
      }
      if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XFlush(dpy);
      }
    }

    if (pause_on_blur) {
      // With no window manager on this display, keyboard focus follows what
      // the compositor gives Xwayland. When the window is not focused the
      // server reports None, and that is our cue.
      Window focus = None;
      int revert = 0;
      XGetInputFocus(dpy, &focus, &revert);
      bool blurred = (focus == None);
      if (blurred && !stopped) {
        kill(game_pid, SIGSTOP);
        stopped = true;
      } else if (!blurred && stopped) {
        kill(game_pid, SIGCONT);
        stopped = false;
      }
    }

    usleep(16000);  // about 60 times a second
  }

  // Never leave a game stopped: if it was paused when it died, or if we are
  // torn down first, it has to be running for anything else to clean up.
  if (stopped) kill(game_pid, SIGCONT);
  SDL_Quit();
  XCloseDisplay(dpy);
  return 0;
}

}  // namespace kg::gui
