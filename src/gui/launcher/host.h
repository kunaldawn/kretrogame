// The launcher's side of the event loop: which screen is up, what Escape, pad
// B and a closed window mean on it, and the modals over all of them. The
// screens themselves are pages.h.
#pragma once

#include <SDL.h>

#include <string>

#include "../../player/player.h"
#include "../event_loop.h"
#include "../page.h"
#include "../window.h"
#include "context.h"
#include "pages.h"

namespace kg::gui::launcher {

class LauncherHost : public Host {
 public:
  // `w` is the Window local to run_launcher, which this must not outlive.
  LauncherHost(const player::Player& p, const Window& w);
  ~LauncherHost() override;
  LauncherHost(const LauncherHost&) = delete;
  LauncherHost& operator=(const LauncherHost&) = delete;

  bool quit() const override { return ctx_.quit; }
  void request_quit() override;
  void page_failed(const std::string& what) override { ctx_.status = what; }
  void on_event(const SDL_Event& ev) override;
  void frame() override;

  // What the window was, for launcher.toml on the way out.
  void remember_window(SDL_Window* win);

 private:
  void back();

  // First, so it is the last thing to go: every page holds a reference to it.
  LauncherContext ctx_;
  CheckingPage checking_;
  BlockedPage blocked_;
  GridPage grid_;
  GamePage game_;
  SavesPage saves_;
  DisplayPage display_;
  ControlsPage controls_;
  BundlePage bundle_;
  LicensesPage licenses_;
  AboutPage about_;
  Pages<Screen, 10> pages_;
};

}  // namespace kg::gui::launcher
