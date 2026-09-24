// The Bundles page: games on the shelf becoming one file somebody else runs.
//
// Eight steps, as the design lays them out - identity, games, each game, the
// check list, size, rights, build, preview - drawn as one page with the steps
// down its left side rather than as a wizard, because a bundle is revisited:
// the second version of it is the first with one number changed, and a person
// should be able to go straight to Build. Everything the page decides is in
// src/bundle/builder.h, where it is tested; this directory draws it and holds
// the two things a page has to - a worker thread for the build, and the
// preview's running process. bundles_page.cpp is the wiring, and each step, or
// pair of steps, is a file of its own beside it.
#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../../bundle/builder.h"
#include "../../bundle/builder/preview.h"
#include "../../rt/env.h"
#include "../shelf/scan.h"
#include "../window.h"
#include "imgui.h"

namespace kg::gui {

class Bundles {
 public:
  Bundles(const rt::Env& e, Fonts fonts);
  ~Bundles();
  Bundles(const Bundles&) = delete;
  Bundles& operator=(const Bundles&) = delete;

  // Coming onto the page: what is remembered, what is on the shelf, and
  // whether this kretro can build players at all.
  void open();
  void draw();
  // Escape. True when the page used it - closing a file list, or going from a
  // bundle back to the list of bundles - and false when the page is done and
  // the shelf should come back.
  bool back();
  enum class Step { Identity, Games, PerGame, Check, Size, Rights, Build, Preview };
  // Straight into one remembered bundle, at one step: what "Build again" does
  // on the list, and a deep link for anything else that wants one. False when
  // no bundle is remembered under that id.
  bool open_bundle(const std::string& id, Step at = Step::Identity);

 private:
  enum class Browse { None, Banner, Icon, Cover, Dll, Folder };

  // The pages.
  void list_page();
  void bundle_page();
  void identity_step();
  void games_step();
  void per_game_step();
  void check_step();
  void size_step();
  void rights_step();
  void build_step();
  void preview_step();

  void browser();
  void take_file(const std::filesystem::path& p);
  void trouble_line();

  // Opening, remembering and leaving a bundle.
  void start_new();
  void edit(const bundle::Draft& d);
  void remember();
  void close_bundle();

  // What the steps are computed from, refreshed as the draft changes.
  void refresh_facts();
  std::vector<bundle::GameFacts> game_facts();
  const bundle::PackFacts* facts_for(const std::string& id) const;
  void probe_imports();
  std::optional<pe::Imports> imports_for(const std::string& id);

  void start_build();
  void pump_build();
  void start_preview();

  const rt::Env& env_;
  ImFont* big_ = nullptr;

  std::vector<bundle::Draft> remembered_;
  std::vector<std::string> unreadable_;
  std::vector<Entry> shelf_;
  std::map<std::string, bundle::PackFacts> facts_;
  std::vector<install::StoredKey> keys_;

  // The player base, looked for once on the way in.
  uint64_t base_bytes_ = 0;
  std::string base_trouble_;
  // Its runtime's licence list, as the last build remembered it: what the
  // size step puts in bundle.meta when it counts it.
  std::vector<std::string> base_licenses_;

  bool editing_ = false;
  bundle::Draft draft_;
  std::string was_id_;     // the id the draft is remembered under
  bool dirty_ = false;
  Step step_ = Step::Identity;
  size_t game_pick_ = 0;
  bool confirm_key_ = false;
  std::string trouble_;

  Browse browse_ = Browse::None;
  std::filesystem::path browse_dir_;

  // Reading each game's executable out of its pack: a dwarfsextract per game,
  // on a thread of its own so the page does not stall while it runs.
  std::thread prober_;
  std::mutex probe_mutex_;
  std::map<std::string, pe::Imports> imports_;
  std::atomic<bool> probing_{false};

  // The build.
  std::thread worker_;
  std::atomic<bool> build_done_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<uint64_t> progress_done_{0}, progress_total_{0};
  std::mutex build_mutex_;
  std::string progress_stage_;
  std::string build_error_;
  std::optional<bundle::Built> built_;
  double build_seconds_ = 0;
  std::string build_for_;  // the id of the bundle being built, whichever is open by the end
  std::string build_note_;

  // "Repack for faster loading", one pack at a time, on a thread of its own:
  // it unpacks and packs a whole game. The page reads the pack's facts again
  // when it is done.
  void start_repack(const std::string& id);
  void pump_repack();
  std::thread repacker_;
  std::atomic<bool> repack_done_{false};
  std::string repacking_;        // the game being repacked, while it is
  std::string repack_error_;     // under build_mutex_, as the build's are
  std::string repack_note_;

  // The preview.
  bundle::Preview preview_;
  std::vector<std::string> preview_log_;
};

}  // namespace kg::gui
