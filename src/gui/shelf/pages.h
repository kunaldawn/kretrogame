// The shelf's pages, one to a screen, each in a file of its own. A page draws
// itself from the ShelfContext and moves to another screen through it; the
// few that reach into another page - to open the wizard, or to have the
// Library scan again - are handed that page when they are made.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL.h>

#include "../../gpu/probe.h"
#include "../../install/draft.h"
#include "../../pack/kgpack.h"
#include "../../util/hash.h"
#include "../bundles/bundles_page.h"
#include "../page.h"
#include "../widgets.h"
#include "../wizard/wizard.h"
#include "context.h"

namespace kg::gui::shelf {

class LibraryPage;

// The Create screen: the wizard, and what the shelf does with what it says.
class WizardPage : public Page {
 public:
  WizardPage(ShelfContext& ctx, LibraryPage& library, SDL_Renderer* ren);
  void draw() override;
  // The wizard is eight screens inside one Screen, so Escape asks it first.
  bool back() override { return wizard_.back(); }
  // While the wizard is up, anything dropped is a source for it.
  bool dropped(const std::filesystem::path& p) override {
    wizard_.add_source(p);
    return true;
  }
  bool busy() const override { return wizard_.busy(); }

  // The wizard with nothing chosen.
  void create();
  // The wizard, starting from what the manifest for `id` knows.
  void open_for(const std::string& id);
  // The wizard, rebuilding a recipe's game from the discs it resolved to.
  void begin_from_recipe(const install::Prefill& p, const Hash& root);
  void ask_abandon() { wizard_.ask_abandon(); }

 private:
  // One test for both, because the shelf's jobs and the wizard's share the
  // "busy" popup.
  bool shelf_busy() const { return ctx_.job.running() || wizard_.busy(); }

  ShelfContext& ctx_;
  LibraryPage& library_;
  Wizard wizard_;
};

// The shelf itself: every game as a tile.
class ShelfPage : public Page {
 public:
  explicit ShelfPage(ShelfContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  void tile(const Entry& e, float w, float h);
  ShelfContext& ctx_;
};

// One game.
class GamePage : public Page {
 public:
  GamePage(ShelfContext& ctx, WizardPage& wizard) : ctx_(ctx), wizard_(wizard) {}
  void draw() override;

 private:
  void uninstall_modal(const Entry& e);

  ShelfContext& ctx_;
  WizardPage& wizard_;
  bool confirm_uninstall_ = false;
  bool also_saves_ = false;
};

// A .kgpack, what it is, and taking it in.
class ImportPage : public Page {
 public:
  ImportPage(ShelfContext& ctx, WizardPage& wizard) : ctx_(ctx), wizard_(wizard) {}
  void draw() override;
  // The page, about `p`.
  void open_file(const std::filesystem::path& p);

 private:
  // Everything the Import page knows about one file, worked out once.
  //
  // Opening the pack, reading its meta and - for a recipe - resolving every
  // disc it names against the collection cannot happen on every frame.
  // Resolving is find_iso_by_fingerprint, which hashes the head of every image
  // in the collection: at sixty frames a second, on a collection of .iso files,
  // that is a disk that never stops and a page that never becomes responsive.
  // None of it changes while the same file is on screen.
  struct Inspected {
    std::filesystem::path of;    // the file this describes; empty means nothing
    std::string trouble;         // why it could not be read, when it could not
    Meta meta;
    bool has_body = false;
    Hash root{};
    uint64_t bytes = 0;
    install::Prefill prefill;    // the recipe's discs, resolved; empty for a capsule
  };

  void inspect_for_import(const std::filesystem::path& f);
  void import_browser();

  ShelfContext& ctx_;
  WizardPage& wizard_;
  Inspected import_;
  std::filesystem::path drop_path_;
  bool import_replace_ = false;
  // The Import page's own file list, and where it is looking. Started at the
  // user's home rather than at games_dir(): a pack somebody sent you is in
  // Downloads, and a pack already in games_dir() is already installed.
  bool import_browsing_ = false;
  std::filesystem::path import_dir_ = home_dir();
};

// What this machine can draw with, and what is wrong with it.
class DoctorPage : public Page {
 public:
  explicit DoctorPage(ShelfContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  ShelfContext& ctx_;
  gpu::Report report_;
  bool probed_ = false;
};

// Every disc in the library folders, and whether it can be installed.
class LibraryPage : public Page {
 public:
  LibraryPage(ShelfContext& ctx, WizardPage& wizard) : ctx_(ctx), wizard_(wizard) {}
  void draw() override;
  // A folder dropped on the window: one more library folder, and this page.
  void add_folder(const std::filesystem::path& p);
  // A disc image or an archive dropped on the window: this page, scanning
  // only that file.
  void scan_only(const std::filesystem::path& p);
  // The sentence beside the page's buttons.
  void note(const std::string& s) { drop_note_ = s; }
  // Back to "nothing scanned yet", so the next visit asks for a scan.
  void rescan_next() { scanned_ = false; }

 private:
  struct DiscRow {
    std::string archive, label, known_as, game_id, state;
    uint64_t bytes = 0;
  };

  void scan_library(const std::vector<std::string>& where);

  ShelfContext& ctx_;
  WizardPage& wizard_;
  // Written by the scan on the worker, under the job's lock, and read only
  // while no job is running.
  std::vector<DiscRow> rows_;
  std::filesystem::path library_only_;  // set when a single file was dropped
  std::string drop_note_;
  bool scanned_ = false;
};

// The window, scaling and the library folders.
class SettingsPage : public Page {
 public:
  explicit SettingsPage(ShelfContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  ShelfContext& ctx_;
};

// One game's snapshots, newest first, and going back to one.
class TimelinePage : public Page {
 public:
  explicit TimelinePage(ShelfContext& ctx) : ctx_(ctx) {}
  void draw() override;

 private:
  ShelfContext& ctx_;
  std::string restore_gen_;
  bool confirm_restore_ = false;
};

// The Bundles screen: the Bundles page, which does its own drawing.
class BundlesPage : public Page {
 public:
  explicit BundlesPage(ShelfContext& ctx) : ctx_(ctx), bundles_(ctx.env, ctx.fonts) {}
  void draw() override { bundles_.draw(); }
  bool back() override { return bundles_.back(); }
  // Coming onto the screen.
  void open();

 private:
  ShelfContext& ctx_;
  Bundles bundles_;
};

}  // namespace kg::gui::shelf
