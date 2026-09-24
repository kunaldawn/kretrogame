// The Bundles page's wiring: coming and going, the facts the steps are
// computed from, the list of bundles and the page one bundle is drawn on. Each
// step is a file of its own beside this one.
#include "bundles_page.h"

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

#include "../../util/env.h"
#include "../../util/paths.h"
#include "../format.h"
#include "../widgets.h"
#include "choices.h"

namespace kg::gui {
namespace fs = std::filesystem;
using namespace kg::bundle;
using bundles_detail::step_word;

Bundles::Bundles(const rt::Env& e, Fonts fonts) : env_(e), big_(fonts.big) {}

Bundles::~Bundles() {
  // kretro is closing. A build still running is cancelled - build_bundle
  // removes its .partial on the way out - and waited for, because it is
  // writing into members of this object.
  cancel_ = true;
  if (worker_.joinable()) worker_.join();
  if (prober_.joinable()) prober_.join();
  // A repack stops at its next step and leaves the pack as it was.
  if (repacker_.joinable()) repacker_.join();
  if (editing_ && dirty_) {
    try {
      remember();
    } catch (const std::exception&) {
    }
  }
}

// ---- coming and going -----------------------------------------------------------

void Bundles::open() {
  unreadable_.clear();
  remembered_ = load_drafts(bundles_dir(), &unreadable_);
  shelf_.clear();
  for (Entry& e : scan(env_)) {
    if (e.installed) shelf_.push_back(std::move(e));
  }
  facts_.clear();
  for (const Entry& e : shelf_) {
    try {
      facts_[e.id] = read_pack_facts(e.pack);
    } catch (const std::exception&) {
      // The shelf already read this pack once; one it cannot read twice is
      // one it does not list, and the draft says the game is gone.
    }
  }
  keys_ = install::load_keys(install::keys_file());
  base_bytes_ = 0;
  base_trouble_.clear();
  base_licenses_.clear();
  try {
    BaseSource base = find_player_base(env_or_empty("KRETRO_SELF"));
    base_bytes_ = player_base_bytes(base);
    // Only a list already remembered: reading it takes a copy of the runtime,
    // which is the build's to make, not the page's on the way in.
    base_licenses_ = base_licenses(base, env_or_empty("KRETRO_DWARFS"), cache_dir(), false);
  } catch (const std::exception& ex) {
    base_trouble_ = ex.what();
  }
  if (browse_dir_.empty()) browse_dir_ = home_dir();
}

bool Bundles::open_bundle(const std::string& id, Step at) {
  for (const Draft& d : remembered_) {
    if (d.id != id) continue;
    edit(d);
    step_ = at;
    return true;
  }
  return false;
}

bool Bundles::back() {
  if (browse_ != Browse::None) {
    browse_ = Browse::None;
    return true;
  }
  if (editing_) {
    close_bundle();
    return true;
  }
  return false;
}

void Bundles::start_new() {
  Draft d;
  d.title = "My bundle";
  d.id = unused_id(bundles_dir(), id_from_title(d.title));
  d.out_dir = home_dir().string();
  edit(d);
  was_id_.clear();
  dirty_ = true;
}

void Bundles::edit(const Draft& d) {
  draft_ = d;
  was_id_ = d.id;
  editing_ = true;
  dirty_ = false;
  step_ = Step::Identity;
  game_pick_ = 0;
  trouble_.clear();
  build_note_.clear();
  preview_log_.clear();
  {
    // A build of another bundle may still be running and about to write these.
    std::lock_guard<std::mutex> lk(build_mutex_);
    build_error_.clear();
  }
  probe_imports();
}

void Bundles::remember() {
  save_draft(bundles_dir(), draft_, was_id_);
  was_id_ = draft_.id;
  dirty_ = false;
}

void Bundles::close_bundle() {
  if (dirty_) {
    try {
      remember();
    } catch (const std::exception& ex) {
      // An id nobody can name a file after cannot be remembered; say so on
      // the page rather than leaving with the work unsaved.
      trouble_ = ex.what();
      return;
    }
  }
  editing_ = false;
  unreadable_.clear();
  remembered_ = load_drafts(bundles_dir(), &unreadable_);
}

// ---- facts ------------------------------------------------------------------------

const PackFacts* Bundles::facts_for(const std::string& id) const {
  auto it = facts_.find(id);
  return it == facts_.end() ? nullptr : &it->second;
}

std::optional<pe::Imports> Bundles::imports_for(const std::string& id) {
  std::lock_guard<std::mutex> lk(probe_mutex_);
  auto it = imports_.find(id);
  if (it == imports_.end()) return std::nullopt;
  return it->second;
}

std::vector<GameFacts> Bundles::game_facts() {
  std::vector<GameFacts> out;
  out.reserve(draft_.games.size());
  for (const DraftGame& g : draft_.games) {
    out.push_back({facts_for(g.id), imports_for(g.id), install::key_for(keys_, g.id)});
  }
  return out;
}

void Bundles::probe_imports() {
  if (probing_) return;
  if (prober_.joinable()) prober_.join();
  std::vector<PackFacts> todo;
  {
    std::lock_guard<std::mutex> lk(probe_mutex_);
    for (const DraftGame& g : draft_.games) {
      if (imports_.count(g.id)) continue;
      if (const PackFacts* f = facts_for(g.id)) todo.push_back(*f);
    }
  }
  if (todo.empty()) return;
  probing_ = true;
  fs::path tool = env_or_empty("KRETRO_DWARFS");
  fs::path scratch = cache_dir() / "bundle-exe";
  prober_ = std::thread([this, todo = std::move(todo), tool, scratch] {
    for (const PackFacts& f : todo) {
      pe::Imports im = read_exe_imports(f, tool, scratch);
      std::lock_guard<std::mutex> lk(probe_mutex_);
      imports_[f.meta.id] = std::move(im);
    }
    probing_ = false;
  });
}

void Bundles::refresh_facts() {
  keys_ = install::load_keys(install::keys_file());
  probe_imports();
}

// ---- drawing ----------------------------------------------------------------------

void Bundles::draw() {
  pump_build();
  pump_repack();
  for (std::string& l : preview_.poll()) preview_log_.push_back(std::move(l));
  if (editing_) bundle_page();
  else list_page();
}

void Bundles::trouble_line() {
  if (trouble_.empty()) return;
  warn_text(trouble_);
  ImGui::Spacing();
}

void Bundles::list_page() {
  PageWindow page("Bundles", "Esc back", big_);
  ImGui::TextWrapped(
      "A bundle is one file that carries a runtime and the games you choose from your shelf. Whoever "
      "downloads it runs it and plays; they need nothing else. Each bundle you build is remembered here, "
      "so the next version is one click.");
  ImGui::Spacing();
  if (!base_trouble_.empty()) {
    warn_text(base_trouble_);
    ImGui::Spacing();
  }
  for (const std::string& u : unreadable_) warn_text("Cannot read a remembered bundle: " + u);

  if (ImGui::Button("New bundle", ImVec2(260, 48))) start_new();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  if (remembered_.empty()) {
    ImGui::TextDisabled("No bundles yet.");
    return;
  }
  for (const Draft& d : remembered_) {
    ImGui::PushID(d.id.c_str());
    ImGui::BeginGroup();
    ImGui::PushFont(big_);
    ImGui::TextUnformatted(d.title.empty() ? d.id.c_str() : d.title.c_str());
    ImGui::PopFont();
    std::string games;
    for (const DraftGame& g : d.games) games += (games.empty() ? "" : ", ") + g.name;
    ImGui::TextDisabled("%s %s  -  %s", d.id.c_str(), d.version.c_str(), games.empty() ? "no games" : games.c_str());
    if (!d.last_built.empty()) {
      ImGui::TextDisabled("last built %s: %s, %s", d.last_built_at.c_str(), d.last_built.c_str(),
                          human_size(d.last_size).c_str());
    }
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetWindowWidth() - 360);
    if (ImGui::Button("Open", ImVec2(140, 40))) edit(d);
    ImGui::SameLine();
    // The one click the design promises: straight to Build with every field
    // as it was left. The Build step still refuses what it would refuse.
    if (ImGui::Button("Build again", ImVec2(160, 40))) {
      open_bundle(d.id, Step::Build);
      start_build();
      ImGui::PopID();
      break;
    }
    ImGui::Separator();
    ImGui::PopID();
  }
}

void Bundles::bundle_page() {
  std::string title = "Bundle: " + (draft_.title.empty() ? draft_.id : draft_.title);
  PageWindow page(title.c_str(), "Esc back to the bundles", big_);
  trouble_line();

  std::vector<GameFacts> facts = game_facts();
  std::vector<Check> checks = run_checks(draft_, facts);

  ImGui::BeginChild("steps", ImVec2(240, 0), true);
  for (int i = 0; i <= static_cast<int>(Step::Preview); ++i) {
    std::string label = step_word(i);
    // What is still open on a step, so the list says where to go next.
    if (static_cast<Step>(i) == Step::Check) {
      size_t open = std::count_if(checks.begin(), checks.end(), [&](const Check& c) { return !draft_.acked(c.id); });
      if (open) label += "  (" + std::to_string(open) + ")";
    }
    if (static_cast<Step>(i) == Step::Rights && !draft_.rights) label += "  (!)";
    if (ImGui::Selectable(label.c_str(), step_ == static_cast<Step>(i))) {
      if (dirty_) {
        try {
          remember();
        } catch (const std::exception& ex) {
          trouble_ = ex.what();
        }
      }
      step_ = static_cast<Step>(i);
      browse_ = Browse::None;
      if (step_ == Step::PerGame || step_ == Step::Check) refresh_facts();
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("step", ImVec2(0, 0), false);
  switch (step_) {
    case Step::Identity: identity_step(); break;
    case Step::Games: games_step(); break;
    case Step::PerGame: per_game_step(); break;
    case Step::Check: check_step(); break;
    case Step::Size: size_step(); break;
    case Step::Rights: rights_step(); break;
    case Step::Build: build_step(); break;
    case Step::Preview: preview_step(); break;
  }
  if (browse_ != Browse::None) browser();
  ImGui::EndChild();
}

}  // namespace kg::gui
