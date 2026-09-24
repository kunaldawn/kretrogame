#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../util/env.h"
#include "../../util/format.h"
#include "../../util/paths.h"
#include "../../util/safe_names.h"
#include "../format.h"
#include "../widgets.h"
#include "bundles_page.h"

namespace kg::gui {
using namespace kg::bundle;

// ---- 7. build -------------------------------------------------------------------------------------

void Bundles::start_build() {
  if (worker_.joinable()) return;
  std::vector<GameFacts> facts = game_facts();
  std::vector<Check> checks = run_checks(draft_, facts);
  std::vector<std::string> blocking = blockers(draft_, facts);
  if (!ready_to_build(draft_, checks, blocking)) {
    trouble_ = !blocking.empty() ? blocking.front()
               : !draft_.rights  ? "Tick \"I have the right to distribute these games\" under Rights first."
                                 : "Some checks are neither fixed nor acknowledged: see Check.";
    return;
  }
  try {
    remember();
  } catch (const std::exception& ex) {
    trouble_ = ex.what();
    return;
  }
  trouble_.clear();
  build_error_.clear();
  build_note_.clear();
  built_.reset();
  cancel_ = false;
  build_done_ = false;
  progress_done_ = 0;
  progress_total_ = 0;
  BuildInputs in = shelf_build_inputs();
  Draft d = draft_;
  build_for_ = d.id;
  worker_ = std::thread([this, d, in] {
    auto t0 = std::chrono::steady_clock::now();
    Callbacks cb;
    cb.progress = [this](const Progress& p) {
      progress_done_ = p.done;
      progress_total_ = p.total;
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (progress_stage_ != p.stage) progress_stage_ = std::string(p.stage);
    };
    cb.cancelled = [this] { return cancel_.load(); };
    std::optional<Built> got;
    std::string err;
    try {
      got = build_from_draft(d, in, cb);
    } catch (const Cancelled&) {
      err = "Cancelled. Nothing was left behind.";
    } catch (const std::exception& ex) {
      err = ex.what();
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard<std::mutex> lk(build_mutex_);
    built_ = std::move(got);
    build_error_ = err;
    build_seconds_ = secs;
    build_done_ = true;
  });
}

void Bundles::pump_build() {
  if (!worker_.joinable() || !build_done_) return;
  worker_.join();
  build_done_ = false;
  std::lock_guard<std::mutex> lk(build_mutex_);
  if (!built_) return;
  // Remembered against the bundle it was built for, which is the one open
  // unless the author has since left it for another.
  const std::string when = fmt::local_minute(std::time(nullptr));
  char secs[32];
  std::snprintf(secs, sizeof(secs), "%.0f", build_seconds_);
  build_note_ = "Built " + built_->path.string() + ", " + human_size(built_->size) + ", in " + secs +
                " seconds, and checked end to end.";
  // The build read the base's licence list and remembered it; the size step
  // counts it from now on.
  try {
    base_licenses_ = base_licenses(find_player_base(env_or_empty("KRETRO_SELF")), env_or_empty("KRETRO_DWARFS"), cache_dir(), false);
  } catch (const std::exception&) {
  }
  auto stamp = [&](Draft& d) {
    d.last_built = built_->path.string();
    d.last_size = built_->size;
    d.last_built_at = when;
  };
  const std::string& built_id = build_for_;
  if (editing_ && built_id == draft_.id) {
    stamp(draft_);
    dirty_ = true;
    try { remember(); } catch (const std::exception&) {}
  } else {
    // From the file, not from the list's copy: the list was read before the
    // author went into a bundle, and may be holding one they have since
    // renamed, which writing back would bring back as a second bundle.
    try {
      stamp_built(bundles_dir(), built_id, built_->path.string(), built_->size, when);
    } catch (const std::exception&) {
    }
    if (!editing_) {
      unreadable_.clear();
      remembered_ = load_drafts(bundles_dir(), &unreadable_);
    }
  }
}

void Bundles::build_step() {
  ImGui::TextUnformatted("Write it to");
  ImGui::SameLine(200);
  ImGui::TextUnformatted(draft_.out_dir.empty() ? "(no folder chosen)" : draft_.out_dir.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Choose folder...")) {
    if (!draft_.out_dir.empty()) browse_dir_ = draft_.out_dir;
    browse_ = Browse::Folder;
  }
  if (version_is_safe(draft_.version) && kg::id_is_safe(draft_.id)) {
    ImGui::TextDisabled("as %s", output_name(draft_).c_str());
  }
  if (!base_trouble_.empty()) warn_text(base_trouble_);
  ImGui::Spacing();

  if (worker_.joinable()) {
    uint64_t done = progress_done_, total = progress_total_;
    std::string stage;
    {
      std::lock_guard<std::mutex> lk(build_mutex_);
      stage = progress_stage_;
    }
    float frac = total ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0f;
    std::string words = stage + "  " + human_size(done) + " of " + human_size(total);
    ImGui::ProgressBar(frac, ImVec2(-1, 36), words.c_str());
    ImGui::BeginDisabled(cancel_.load());
    if (ImGui::Button(cancel_ ? "Cancelling..." : "Cancel", ImVec2(200, 44))) cancel_ = true;
    ImGui::EndDisabled();
    ImGui::TextDisabled("It is written as .partial, checked, and only then named %s. Cancelled, nothing is left.",
                        output_name(draft_).c_str());
    return;
  }

  std::vector<GameFacts> facts = game_facts();
  std::vector<Check> checks = run_checks(draft_, facts);
  std::vector<std::string> blocking = blockers(draft_, facts);
  bool ready = ready_to_build(draft_, checks, blocking);
  ImGui::BeginDisabled(!ready || !base_trouble_.empty());
  if (ImGui::Button("Build", ImVec2(260, 56))) start_build();
  ImGui::EndDisabled();
  if (!ready) {
    for (const std::string& b : blocking) ImGui::TextDisabled("%s", b.c_str());
    size_t open = std::count_if(checks.begin(), checks.end(), [&](const Check& c) { return !draft_.acked(c.id); });
    if (open) ImGui::TextDisabled("%zu check(s) neither fixed nor acknowledged: see Check.", open);
    if (!draft_.rights) ImGui::TextDisabled("Rights: not yet ticked.");
  }
  std::string err, note = build_note_;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    err = build_error_;
  }
  if (!err.empty()) warn_text(err);
  if (!note.empty()) good_text(note);
  if (!draft_.last_built.empty() && note.empty()) {
    ImGui::TextDisabled("Last built %s: %s, %s", draft_.last_built_at.c_str(), draft_.last_built.c_str(),
                        human_size(draft_.last_size).c_str());
  }
  if (!draft_.last_built.empty() && !draft_.published) {
    ImGui::Spacing();
    if (ImGui::Button("Mark as published")) {
      draft_.published = true;
      dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("once it has gone out: the id is then fixed");
  }
}

}  // namespace kg::gui
