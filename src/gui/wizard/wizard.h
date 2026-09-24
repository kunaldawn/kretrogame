// The wizard: a disc you own becoming a game on the shelf, one question at a
// time.
//
// This is the other half of dropping the automation. A games/*.toml manifest
// existed because nobody was watching the install - source.verify named files
// in a directory that did not exist yet, source.setup said which of the disc's
// executables was the real one, run.windows_version was knowledge you got by
// reading an installer's error box. Every one of those is a fact a person has
// at a particular moment. The wizard asks at that moment instead.
//
// It has a directory of its own, a file per step or pair of steps beside the
// wiring in wizard.cpp, because it is eight screens, a worker thread and a
// nested X display.
#pragma once

#include <SDL.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../install/build.h"
#include "../../install/preset.h"
#include "../../install/setup_ref.h"
#include "../../pack/kgpack.h"
#include "../../rt/env.h"
#include "../job.h"
#include "../stage/stage.h"
#include "../window.h"
#include "imgui.h"

namespace kg::gui {

// What a frame of the wizard came to, for the shelf to act on.
struct WizardOutcome {
  enum Kind {
    Still,         // still up, and nothing asked of the shelf
    Closed,        // done with the screen; `status` is the sentence for the shelf
    Play,          // done, and `game_id` is to be played
    WantsLibrary,  // still up, and asking for the Library screen, once
  } kind = Still;
  std::string game_id;
  std::string status;
};

class Wizard {
 public:
  Wizard(const rt::Env& e, SDL_Renderer* ren, Fonts fonts);
  ~Wizard();
  Wizard(const Wizard&) = delete;
  Wizard& operator=(const Wizard&) = delete;

  // Step 1, with nothing chosen.
  void begin();
  // Step 1, prefilled from the manifest that named this game.
  void begin_from(const install::Prefill& p);
  // A recipe: the discs are resolved, the identity and the run block are
  // known, so there is nothing left to ask before the install itself.
  void begin_from_recipe(const install::Prefill& p, const Hash& expect_root);

  // Draws the current step. Closed or Play means the wizard is done with the
  // screen and the caller should go back to the shelf.
  WizardOutcome draw();

  // Escape. True when the wizard consumed it - which on the install step means
  // raising the abandon prompt rather than stepping anywhere.
  bool back();

  bool busy() const { return job_.running(); }
  // The window was closed while an install was running.
  void ask_abandon();
  // Something was dropped on the window while step 1 was up.
  void add_source(const std::filesystem::path& p);

 private:
  enum class Step { Sources, Identity, What, Installing, Where, Runs, Presentation, Build, Done };

  void sources_page();
  void preset_offer();
  void identity_page();
  void what_page();
  void installing_page();
  void where_page();
  void runs_page();
  void presentation_page();
  void build_page();
  void done_page();

  void browser();
  // What went wrong, drawn on the page it went wrong on. A wizard step is a
  // whole screen and the log modal is gone by the time anybody could read it,
  // so a failure with nowhere to be said is a failure the user never sees.
  void trouble_banner();
  // Which entry of step 3's ranked list the draft already names. A manifest
  // that says INSTALL.EXE has to survive the list being ranked around it, or
  // accepting a preset would quietly choose a different installer.
  void preselect_setup();
  void modal();
  void abandon_popup();
  void start_the_install();
  void leave_installing();
  // What draw() returns on the frame the wizard lets go of the screen: Play
  // when the done page's "Play it" left "play:<id>" in status_, Closed with
  // status_ otherwise.
  WizardOutcome closed() const;

  // A Job, as the shelf runs one, for the same reason: the window has to keep
  // drawing through the minutes a probe, an install or a build takes. It
  // carries the step to go to afterwards, because with eight steps "what
  // happens when this finishes" would otherwise be scattered over all of them.
  // What a job failed with is job_.error(), a copy: the install step draws
  // while its worker is still running - it is the one step with no modal over
  // it - and reading a std::string another thread is assigning to is a torn
  // pointer and a length that do not belong together.
  void run_job(const std::string& title, std::function<void()> job, Step next,
               bool show_modal = true);
  void pump_job();
  // The sources dropped on the window while a job was running, added the
  // moment it is not. Running add_source when a second disc is dropped while
  // "Reading your discs" is up would reset build_ - the very object the
  // worker is inside.
  void take_pending_sources();
  // The one place a finished job is taken apart: the worker joined, the step
  // moved on or held, and whatever it failed with put where a person can read
  // it. The log modal's Close button and pump_job both end here, so there is
  // one answer to what a failure means, not two copies that disagree.
  void finish_job();

  const rt::Env& env_;
  SDL_Renderer* ren_;
  ImFont* big_ = nullptr;

  Step step_ = Step::Sources;
  install::Draft draft_;
  // Step 2's offer. Computed once, when step 2 is first drawn: it reads
  // a handful of small toml files and compares strings, which is nothing beside
  // the probing that got us here.
  std::vector<install::Preset> presets_;
  size_t preset_index_ = 0;
  bool presets_computed_ = false;
  bool preset_settled_ = false;   // taken or declined; either way, asked once
  std::filesystem::path work_;
  std::unique_ptr<install::Build> build_;
  std::vector<install::Source> classified_;
  std::vector<std::string> known_as_;   // one per disc, from db/discs.txt
  // Dropped while the probe was running. Held rather than refused: a person
  // handing the window their second disc while the first is being read has
  // done nothing wrong, and saying nothing at all would be the program
  // ignoring what it was handed.
  std::vector<std::filesystem::path> pending_;
  std::vector<std::string> missing_;    // discs a recipe wants and we lack
  // Set by the worker as the last thing open_sources' job does, read by the UI
  // thread on every frame it draws. Atomic because it is also the gate: the
  // disc rows below are read out of Build's own vector, and until this is true
  // that vector is being cleared and rebuilt by somebody else.
  std::atomic<bool> probed_{false};
  // "This zip holds no disc" is a fact about the collection, so the page
  // offers the Library screen. draw() reports it once, then forgets.
  bool want_library_ = false;
  std::string status_;
  // Said on the current page until the next thing happens. status_ is the
  // sentence the shelf's WizardPage shows when the wizard closes, which is no
  // use to somebody still standing in it.
  std::string trouble_;

  // Text fields. There is no imgui_stdlib in this tree, so every editable
  // string is a fixed buffer synced to the Draft on change.
  char id_buf_[64] = {};
  char name_buf_[128] = {};
  char serial_buf_[64] = {};
  char args_buf_[128] = {};
  char subdir_buf_[128] = {};
  char member_buf_[128] = {};
  int year_ = 0;

  size_t setup_pick_ = 0, candidate_pick_ = 0, exe_pick_ = 0;
  std::vector<install::SetupChoice> setups_;   // (disc, path on it)
  // Listing a disc spawns 7z once per disc. An empty list is a real answer - a
  // disc with no executable at its root - so "have we looked" is a flag of its
  // own rather than setups_.empty(), which would have step 3 list every disc
  // again on every frame it drew.
  bool setups_scanned_ = false;

  // Step 4.
  std::unique_ptr<Stage> stage_;
  std::mutex display_mutex_;
  std::string display_;
  std::string stage_trouble_;
  bool stage_dead_ = false;
  size_t files_ = 0;
  // Which disc each drive letter is currently pointed at, as the combo shows
  // it. Build::drives() records what was mounted and is what goes into the
  // pack, so a swap must not edit it; this is the UI's own memory of a change
  // the user made.
  std::vector<size_t> drive_pick_;
  // What swap_disc said when it refused. Caught at the call site, because it
  // is called from inside an open BeginCombo.
  std::string drive_trouble_;
  bool confirm_abandon_ = false;
  bool abandoning_ = false;
  // The done page's buttons all mean "leave", and draw() reports that to the
  // shelf's WizardPage as a Closed or Play WizardOutcome. Set there and
  // nowhere else.
  bool finished_ = false;

  // Step 8.
  bool expect_root_set_ = false;
  Hash expect_root_{};
  install::Result result_;
  bool root_matched_ = false;

  // The staging tree a killed run left behind.
  std::filesystem::path stale_;
  uint64_t stale_bytes_ = 0;

  // The file browser: a plain list over std::filesystem, because there is no
  // file dialog in this binary and there should not be one.
  bool browsing_ = false;
  std::filesystem::path browse_dir_;

  // The worker.
  Job job_;
  bool modal_ = true;
  Step next_ = Step::Sources;
};

}  // namespace kg::gui
