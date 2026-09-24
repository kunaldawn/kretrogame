// The launcher's pages, one to a screen, and the modals the host puts over
// them. A page draws itself from the LauncherContext and moves to another
// screen through it.
#pragma once

#include <string>

#include "../page.h"
#include "context.h"

namespace kg::gui::launcher {

// ---- first run (first_run.cpp) ----------------------------------------------

// The machine being checked, before anything else is up.
class CheckingPage : public Page {
 public:
  explicit CheckingPage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  LauncherContext& ctx_;
};

// What stops every game on this machine.
class BlockedPage : public Page {
 public:
  explicit BlockedPage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  LauncherContext& ctx_;
};

// The first run's warnings, said once.
void notes_modal(LauncherContext& ctx);
// "Add to applications menu", offered once.
void desktop_modal(LauncherContext& ctx);

// ---- the grid (grid_page.cpp) -----------------------------------------------

// The bundle's banner, then one tile per game.
class GridPage : public Page {
 public:
  explicit GridPage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  void banner();
  void status_line();
  LauncherContext& ctx_;
};

// ---- one game (game_page.cpp) -----------------------------------------------

// Play, the way to the game's saves, display and controls, and why it last
// would not play.
class GamePage : public Page {
 public:
  explicit GamePage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  LauncherContext& ctx_;
};

// Asking before unpacking a game on a machine with no FUSE.
void consent_modal(LauncherContext& ctx);

// ---- saves (saves_page.cpp) -------------------------------------------------

// Export, import, and the snapshots to go back to.
class SavesPage : public Page {
 public:
  explicit SavesPage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  LauncherContext& ctx_;
  std::string export_path_, import_path_, restore_gen_;
};

// ---- display and controls (settings_pages.cpp) ------------------------------

class DisplayPage : public Page {
 public:
  explicit DisplayPage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  LauncherContext& ctx_;
};

class ControlsPage : public Page {
 public:
  explicit ControlsPage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  LauncherContext& ctx_;
};

// ---- the bundle (bundle_pages.cpp) ------------------------------------------

// The applications-menu entry, the data folder, and the way to the licences
// and About.
class BundlePage : public Page {
 public:
  explicit BundlePage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  LauncherContext& ctx_;
};

class LicensesPage : public Page {
 public:
  explicit LicensesPage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  LauncherContext& ctx_;
  std::string licenses_text_;
};

// The doctor's report, with a Save button.
class AboutPage : public Page {
 public:
  explicit AboutPage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  LauncherContext& ctx_;
  std::string report_text_;
};

// ---- working (working_modal.cpp) --------------------------------------------

// The running job's title and how far it has got.
void working_modal(LauncherContext& ctx);

}  // namespace kg::gui::launcher
