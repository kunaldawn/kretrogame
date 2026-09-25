#include <algorithm>
#include <chrono>
#include <cmath>
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
#include "choices.h"

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
  section("build");
  if (form_begin("where", {"Write it to"})) {
    form_row("Write it to");
    // The button first, so a long folder is what gets cut short.
    // A button as tall as the page's fields, so the form keeps one rhythm.
    if (ImGui::Button("Choose folder...")) {
      if (!draft_.out_dir.empty()) browse_dir_ = draft_.out_dir;
      browse_ = Browse::Folder;
    }
    ImGui::SameLine(0, std::round(px(12)));
    if (draft_.out_dir.empty()) ImGui::TextDisabled("(no folder chosen)");
    else elided_text(draft_.out_dir, kCyan);
    if (version_is_safe(draft_.version) && kg::id_is_safe(draft_.id)) {
      ImGui::TextDisabled("as");
      ImGui::SameLine();
      elided_text(output_name(draft_), kCyan);
    }
    ImGui::EndTable();
  }
  if (!base_trouble_.empty()) badge_line(BadgeKind::Fail, base_trouble_);
  ImGui::Dummy(ImVec2(0, std::round(px(6))));

  if (worker_.joinable()) {
    uint64_t done = progress_done_, total = progress_total_;
    std::string stage;
    {
      std::lock_guard<std::mutex> lk(build_mutex_);
      stage = progress_stage_;
    }
    // Unknown until the build has counted what it will write: the bar then
    // slides rather than sitting at nought.
    float frac = total ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : -1.0f;
    spinner();
    ImGui::SameLine();
    elided_text(stage.empty() ? std::string("building") : stage);
    std::string words = human_size(done) + " of " + human_size(total);
    block_progress(frac, 0, total ? words.c_str() : nullptr);
    ImGui::Dummy(ImVec2(0, std::round(px(6))));
    ImGui::BeginDisabled(cancel_.load());
    // An ordinary button: the big one is Build's, and this is not an
    // action to draw the eye to.
    if (ImGui::Button(cancel_ ? "Cancelling..." : "Cancel")) cancel_ = true;
    ImGui::EndDisabled();
    ImGui::Spacing();
    colored_text(kDim, "It is written as .partial, checked, and only then named " + output_name(draft_) +
                           ". Cancelled, nothing is left.");
    return;
  }

  std::vector<GameFacts> facts = game_facts();
  std::vector<Check> checks = run_checks(draft_, facts);
  std::vector<std::string> blocking = blockers(draft_, facts);
  bool ready = ready_to_build(draft_, checks, blocking);
  // Build itself is the footer's action (bundle_page); what stands in its
  // way is said here, in full.
  if (!ready) {
    for (const std::string& b : blocking) badge_line(BadgeKind::Fail, b);
    size_t open = std::count_if(checks.begin(), checks.end(), [&](const Check& c) { return !draft_.acked(c.id); });
    if (open) {
      badge_line(BadgeKind::Warn, std::to_string(open) + " check(s) neither fixed nor acknowledged: see Check.");
    }
    if (!draft_.rights) badge_line(BadgeKind::Warn, "Rights: not yet ticked.");
  }
  std::string err, note = build_note_;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    err = build_error_;
  }
  if (!err.empty()) badge_line(BadgeKind::Fail, err);
  if (!note.empty()) badge_line(BadgeKind::Ok, note);
  if (!draft_.last_built.empty() && note.empty()) {
    colored_text(kDim, "Last built " + draft_.last_built_at + ": " + draft_.last_built + ", " +
                           human_size(draft_.last_size));
  }
  if (!draft_.last_built.empty() && !draft_.published) {
    section("publish");
    if (ImGui::Button("Mark as published")) {
      draft_.published = true;
      dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    elided_text("once it has gone out: the id is then fixed", kDim);
  }
}

}  // namespace kg::gui
