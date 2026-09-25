// The shelf's side of the event loop: which screen is up, what a key, a
// dropped file or a closed window means on it, and the job modal over all of
// them. The screens themselves are pages.h.
#pragma once

#include <SDL.h>

#include <string>

#include "../../rt/env.h"
#include "../event_loop.h"
#include "../page.h"
#include "../window.h"
#include "context.h"
#include "pages.h"
#include "shelf.h"

namespace kg::gui::shelf {

class ShelfHost : public Host {
 public:
  ShelfHost(const rt::Env& e, const Window& w);
  ~ShelfHost() override;
  ShelfHost(const ShelfHost&) = delete;
  ShelfHost& operator=(const ShelfHost&) = delete;

  // Where the shelf comes up, from the command line.
  void start(const Startup& entry);

  void frame() override;
  bool quit() const override { return ctx_.quit; }
  void request_quit() override;
  void page_failed(const std::string& what) override;
  void on_event(const SDL_Event& ev) override;

 private:
  void back();
  void dropped(const std::string& path);
  void type(char c);
  // The letter's action, when `c` is one of the shelf's letters (either
  // case), and whether it was.
  bool shortcut(char c);
  void clear_filter() { ctx_.filter.clear(); }
  bool filtering() const { return !ctx_.filter.empty(); }
  // One test for both, because the shelf's jobs and the wizard's share the
  // "busy" popup.
  bool is_busy() const { return ctx_.job.running() || wizard_.busy(); }
  void modal();

  // First, so it is the last thing to go: every page holds a reference to it.
  ShelfContext ctx_;
  // The wizard before the Bundles page, in the order they were always made
  // and, backwards, destroyed.
  WizardPage wizard_;
  ShelfPage shelf_;
  GamePage game_;
  ImportPage import_;
  DoctorPage doctor_;
  LibraryPage library_;
  SettingsPage settings_;
  TimelinePage timeline_;
  BundlesPage bundles_;
  Pages<Screen, 9> pages_;
};

}  // namespace kg::gui::shelf
