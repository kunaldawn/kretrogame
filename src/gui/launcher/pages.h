// The launcher's pages, one to a screen, and the modals the host puts over
// them. A page draws itself from the LauncherContext and moves to another
// screen through it.
#pragma once

#include <string>
#include <vector>

#include "../../session/journal.h"
#include "../focus.h"
#include "../page.h"
#include "context.h"

namespace kg::gui::launcher {

// A page's name as a part of the path the status bar shows, the way the page
// chrome writes one it derives from a title: lower case, with a dash for
// each run of anything else. The pages under a game or under the bundle's
// settings set their path from it, "~/example-game/saves", so it reads as the
// hierarchy it is.
std::string path_part(const std::string& name);

// The header of a page under a game or under the bundle's settings, which
// says where it is rather than naming itself in large type (the page does
// that at the head of its column): the bundle, the game when the player has
// more than one, and `leaf`, "Example Collection › Example Game › saves".
// Set before the page's PageWindow. Defined in settings_pages.cpp.
void set_trail(const LauncherContext& ctx, const std::string& game, const char* leaf);

// A paragraph in `colour`, wrapped to the page but no wider than `max_w`, set
// a line at a time with a little room between the lines: ImGui's own wrapping
// sets them solid, and in a paragraph of any length they nearly touch.
// Defined in settings_pages.cpp.
void spaced_text(const std::string& text, const ImVec4& colour, float max_w);

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

// The focused game as a hero band under the bundle's banner, with Play, and
// the bundle's games in a row under it (a grid when there are many).
class GridPage : public Page {
 public:
  explicit GridPage(LauncherContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  void count_line();
  LauncherContext& ctx_;
  // The game the band is about, the one it shows now, and the one it is
  // cross-fading from.
  std::string hero_id_, shown_id_, was_id_;
  // That game's journal as last read, and when to read it again.
  Refresh disk_;
  std::vector<session::Record> journal_;
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
  // The game's journal as last read, and when to read it again.
  Refresh disk_;
  std::vector<session::Record> journal_;
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
  // The game's snapshots and journal as last read, and when to read them
  // again.
  Refresh disk_;
  std::vector<std::string> gens_;
  std::vector<session::Record> recs_;
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
  // Whether the applications-menu entry is there, as last looked, and when
  // to look again.
  Refresh disk_;
  bool installed_ = false;
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
  // The report as it stood when the page was first drawn, kept so that it is
  // not read while a job could be writing it.
  player::doctor::Report report_;
  bool have_report_ = false;
};

// ---- working (working_modal.cpp) --------------------------------------------

// The running job's title and how far it has got.
void working_modal(LauncherContext& ctx);

}  // namespace kg::gui::launcher
