#include "bundles.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "../util/paths.h"
#include "app.h"

// The implementation is compiled into app.cpp; this is only the declarations,
// for reading a picture's size without drawing it.
#include "stb_image.h"

namespace kg::gui {
namespace fs = std::filesystem;
using namespace kg::bundle;

namespace {

// The pictures go into bundle.meta whole, and bundle.meta is read into memory
// whole by every player. Generous for a picture; small beside 64 MiB.
constexpr uint64_t kMaxPicture = 8ull << 20;
// A dgVoodoo DLL is a few hundred kilobytes.
constexpr uint64_t kMaxExtraFile = 16ull << 20;

const char* kBackends[] = {"auto", "dxvk", "wined3d-vk", "wined3d-gl", "cnc-ddraw"};
const char* kBackendWords[] = {"auto", "DXVK", "WineD3D on Vulkan", "WineD3D on OpenGL", "cnc-ddraw"};
const char* kDisplays[] = {"integer", "fit", "native"};
const char* kDisplayWords[] = {"integer scaling", "fit the screen", "native size"};

const char* step_word(int i) {
  static const char* w[] = {"1  Identity", "2  Games", "3  Each game", "4  Check",
                            "5  Size",     "6  Rights", "7  Build",    "8  Preview"};
  return w[i];
}

// There is no imgui_stdlib in this tree. ImGui keeps its own copy of a field
// while it is being edited, so a buffer made fresh each frame from the string
// is enough, and the string is only written when the text changed.
bool input(const char* label, std::string& s, size_t cap, ImGuiInputTextFlags flags = 0) {
  std::vector<char> buf(cap, '\0');
  std::snprintf(buf.data(), cap, "%s", s.c_str());
  if (!ImGui::InputText(label, buf.data(), cap, flags)) return false;
  s = buf.data();
  return true;
}

bool input_multiline(const char* label, std::string& s, size_t cap, ImVec2 size) {
  std::vector<char> buf(cap, '\0');
  std::snprintf(buf.data(), cap, "%s", s.c_str());
  if (!ImGui::InputTextMultiline(label, buf.data(), cap, size)) return false;
  s = buf.data();
  return true;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

bool is_png(const std::string& b) { return b.size() > 8 && b.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0; }

// "1280x400 PNG, 120 KB", or why it is not a picture.
std::string picture_words(const std::string& png) {
  int w = 0, h = 0, n = 0;
  if (!stbi_info_from_memory(reinterpret_cast<const unsigned char*>(png.data()), static_cast<int>(png.size()), &w, &h,
                             &n)) {
    return "not a picture this kretro can read";
  }
  return std::to_string(w) + "x" + std::to_string(h) + " PNG, " + human_size(png.size());
}

void warn_text(const std::string& s) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
  ImGui::TextWrapped("%s", s.c_str());
  ImGui::PopStyleColor();
}

void good_text(const std::string& s) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
  ImGui::TextWrapped("%s", s.c_str());
  ImGui::PopStyleColor();
}

fs::path home_of_the_person() {
  if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
  return fs::path("/");
}

std::string env_or(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

int index_of(const char* const* set, size_t n, const std::string& v) {
  for (size_t i = 0; i < n; ++i) {
    if (v == set[i]) return static_cast<int>(i);
  }
  return 0;
}

}  // namespace

Bundles::Bundles(const rt::Env& e) : env_(e) {}

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
    BaseSource base = find_player_base(env_or("KRETRO_SELF"));
    base_bytes_ = player_base_bytes(base);
    // Only a list already remembered: reading it takes a copy of the runtime,
    // which is the build's to make, not the page's on the way in.
    base_licenses_ = base_licenses(base, env_or("KRETRO_DWARFS"), cache_dir(), false);
  } catch (const std::exception& ex) {
    base_trouble_ = ex.what();
  }
  if (browse_dir_.empty()) browse_dir_ = home_of_the_person();
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
  d.out_dir = home_of_the_person().string();
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
  fs::path tool = env_or("KRETRO_DWARFS");
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
  begin_page("Bundles", "Esc back", big_);
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
    ImGui::End();
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
  ImGui::End();
}

void Bundles::bundle_page() {
  std::string title = "Bundle: " + (draft_.title.empty() ? draft_.id : draft_.title);
  begin_page(title.c_str(), "Esc back to the bundles", big_);
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
  ImGui::End();
}

// ---- 1. identity ------------------------------------------------------------------------

void Bundles::identity_step() {
  ImGui::TextUnformatted("Title");
  ImGui::SetNextItemWidth(-1);
  if (input("##title", draft_.title, 128)) {
    dirty_ = true;
    // The id follows the title until the author types one, and never once
    // the bundle is out: that is when changing it would lose people's saves.
    if (!draft_.id_typed && !draft_.published) draft_.id = id_from_title(draft_.title);
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Bundle id");
  ImGui::SetNextItemWidth(420);
  ImGui::BeginDisabled(draft_.published);
  if (input("##id", draft_.id, 101)) {
    draft_.id_typed = true;
    dirty_ = true;
  }
  ImGui::EndDisabled();
  if (draft_.published) {
    ImGui::TextWrapped(
        "Fixed: this bundle has been published, and every player keeps its saves under this id. A new id "
        "would make a new bundle, whose players could not find the old one's saves.");
  } else {
    ImGui::TextDisabled(draft_.id_typed ? "Yours. It becomes fixed once you mark the bundle published."
                                        : "Made from the title. Type your own if you want another.");
    if (draft_.id_typed && ImGui::SmallButton("Make it from the title again")) {
      draft_.id_typed = false;
      draft_.id = id_from_title(draft_.title);
      dirty_ = true;
    }
  }
  if (!kg::id_is_safe(draft_.id)) warn_text("An id is letters, digits, '-', '_' and '.', starting with a letter or digit.");

  ImGui::Spacing();
  ImGui::TextUnformatted("Version");
  ImGui::SetNextItemWidth(200);
  if (input("##version", draft_.version, 33)) dirty_ = true;
  if (!version_is_safe(draft_.version)) warn_text("A version is letters, digits, '-', '_' and '.': it goes into the file name.");
  else ImGui::TextDisabled("The file will be %s", output_name(draft_).c_str());

  ImGui::Spacing();
  auto picture = [&](const char* what, std::string& bytes, std::string& from, Browse mode) {
    ImGui::PushID(what);
    ImGui::TextUnformatted(what);
    ImGui::SameLine(160);
    if (bytes.empty()) ImGui::TextDisabled("none");
    else ImGui::Text("%s  (%s)", picture_words(bytes).c_str(), fs::path(from).filename().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Choose PNG...")) browse_ = mode;
    if (!bytes.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Remove")) {
        bytes.clear();
        from.clear();
        dirty_ = true;
      }
    }
    ImGui::PopID();
  };
  picture("Banner", draft_.banner, draft_.banner_from, Browse::Banner);
  picture("Icon", draft_.icon, draft_.icon_from, Browse::Icon);

  ImGui::Spacing();
  if (!draft_.published) {
    ImGui::TextWrapped(
        "Once a build of this bundle has gone out to anybody, mark it published: its id is then fixed.");
    ImGui::BeginDisabled(draft_.last_built.empty());
    if (ImGui::Button("Mark as published")) {
      draft_.published = true;
      dirty_ = true;
    }
    ImGui::EndDisabled();
  }
}

// ---- 2. games ---------------------------------------------------------------------------

void Bundles::games_step() {
  ImGui::TextWrapped("The games go into the file in this order, and the launcher shows them in it.");
  ImGui::Spacing();
  int move = -1, dir = 0, drop = -1;
  for (size_t i = 0; i < draft_.games.size(); ++i) {
    const DraftGame& g = draft_.games[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::Text("%zu.  %s", i + 1, g.name.c_str());
    const PackFacts* f = facts_for(g.id);
    ImGui::SameLine(420);
    if (!f) {
      warn_text("no longer on the shelf");
    } else {
      std::string discs = f->discs_carried ? "carries " + std::to_string(f->discs_carried) + " disc" +
                                                 (f->discs_carried == 1 ? "" : "s")
                                           : "carries no discs";
      if (f->discs_named) discs += ", needs " + std::to_string(f->discs_named) + " it does not carry";
      ImGui::TextDisabled("%s, %s", human_size(f->bytes).c_str(), discs.c_str());
      // Installed before packs stored what is already compressed raw, in
      // 4 MiB blocks: it plays, from a mount that reads more slowly.
      if (packed_before_faster_loading(*f)) {
        ImGui::SameLine();
        if (repacking_ == g.id) {
          ImGui::TextDisabled("repacking...");
        } else {
          ImGui::BeginDisabled(repacker_.joinable() || worker_.joinable());
          if (ImGui::SmallButton("Repack for faster loading")) start_repack(g.id);
          ImGui::EndDisabled();
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Packed before kretro stored packs for fast reads. Repacking keeps the game "
                              "exactly as it is - the same files, the same Merkle root, the same saves - and "
                              "changes only how its pack stores them.");
          }
        }
      }
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 260);
    ImGui::BeginDisabled(i == 0);
    if (ImGui::SmallButton("up")) { move = static_cast<int>(i); dir = -1; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 == draft_.games.size());
    if (ImGui::SmallButton("down")) { move = static_cast<int>(i); dir = 1; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("remove")) drop = static_cast<int>(i);
    ImGui::PopID();
  }
  if (move >= 0) {
    std::swap(draft_.games[move], draft_.games[move + dir]);
    dirty_ = true;
  }
  if (drop >= 0) {
    draft_.games.erase(draft_.games.begin() + drop);
    game_pick_ = 0;
    dirty_ = true;
  }
  if (draft_.games.empty()) ImGui::TextDisabled("No games yet: add some from the shelf below.");
  {
    std::string err, note = repack_note_;
    {
      std::lock_guard<std::mutex> lk(build_mutex_);
      err = repack_error_;
    }
    if (!err.empty()) warn_text(err);
    if (!note.empty()) good_text(note);
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("On your shelf");
  bool any = false;
  for (const Entry& e : shelf_) {
    bool chosen = std::any_of(draft_.games.begin(), draft_.games.end(), [&](const DraftGame& g) { return g.id == e.id; });
    if (chosen) continue;
    const PackFacts* f = facts_for(e.id);
    if (!f) continue;
    any = true;
    ImGui::PushID(e.id.c_str());
    if (ImGui::SmallButton("add")) {
      // The shelf's own title screen, when it has one, is the first cover.
      draft_.games.push_back(game_from_pack(*f, e.title_png.empty() ? e.last_png : e.title_png));
      dirty_ = true;
      probe_imports();
    }
    ImGui::SameLine();
    ImGui::Text("%s", e.name.c_str());
    ImGui::SameLine(420);
    ImGui::TextDisabled("%s%s", human_size(f->bytes).c_str(),
                        f->discs_carried ? ", with its discs" : ", no discs");
    ImGui::PopID();
  }
  if (!any) ImGui::TextDisabled(shelf_.empty() ? "Nothing is installed yet." : "Every installed game is in this bundle.");
}

void Bundles::start_repack(const std::string& id) {
  if (repacker_.joinable() || worker_.joinable()) return;
  const PackFacts* f = facts_for(id);
  if (!f) return;
  repacking_ = id;
  repack_note_.clear();
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    repack_error_.clear();
  }
  repack_done_ = false;
  cancel_ = false;
  fs::path pack = f->path, tool = env_or("KRETRO_DWARFS"), scratch = cache_dir() / "repack";
  repacker_ = std::thread([this, pack, tool, scratch] {
    std::string err;
    try {
      Callbacks cb;
      cb.cancelled = [this] { return cancel_.load(); };
      repack_for_faster_loading(pack, tool, scratch, cb);
    } catch (const Cancelled&) {
      err = "Repack cancelled; the pack is as it was.";
    } catch (const std::exception& ex) {
      err = ex.what();
    }
    std::lock_guard<std::mutex> lk(build_mutex_);
    repack_error_ = err;
    repack_done_ = true;
  });
}

void Bundles::pump_repack() {
  if (!repacker_.joinable() || !repack_done_) return;
  repacker_.join();
  repack_done_ = false;
  const std::string id = repacking_;
  repacking_.clear();
  std::string err;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    err = repack_error_;
  }
  // The pack's size and body changed; its tree did not.
  if (auto it = facts_.find(id); it != facts_.end()) {
    try {
      it->second = read_pack_facts(it->second.path);
    } catch (const std::exception&) {
    }
    if (err.empty()) repack_note_ = "Repacked " + it->second.meta.name + " for faster loading: " +
                                    human_size(it->second.bytes) + ", the same game.";
  }
}

// ---- 3. each game ---------------------------------------------------------------------------

void Bundles::per_game_step() {
  if (draft_.games.empty()) {
    ImGui::TextDisabled("Choose games first.");
    return;
  }
  if (game_pick_ >= draft_.games.size()) game_pick_ = 0;
  ImGui::SetNextItemWidth(420);
  if (ImGui::BeginCombo("##game", draft_.games[game_pick_].name.c_str())) {
    for (size_t i = 0; i < draft_.games.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(draft_.games[i].name.c_str(), i == game_pick_)) game_pick_ = i;
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  DraftGame& g = draft_.games[game_pick_];
  const PackFacts* f = facts_for(g.id);
  ImGui::PushID(g.id.c_str());
  ImGui::Separator();

  ImGui::TextUnformatted("Name");
  ImGui::SameLine(200);
  ImGui::SetNextItemWidth(420);
  if (input("##name", g.name, 128)) dirty_ = true;
  ImGui::TextUnformatted("Year");
  ImGui::SameLine(200);
  int year = static_cast<int>(g.year);
  ImGui::SetNextItemWidth(160);
  if (ImGui::InputInt("##year", &year, 0)) {
    g.year = static_cast<uint32_t>(std::clamp(year, 0, 9999));
    dirty_ = true;
  }
  ImGui::TextUnformatted("Cover");
  ImGui::SameLine(200);
  if (g.cover.empty()) ImGui::TextDisabled("none");
  else ImGui::Text("%s  (%s)", picture_words(g.cover).c_str(), fs::path(g.cover_from).filename().c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Choose PNG...")) browse_ = Browse::Cover;
  if (!g.cover.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) {
      g.cover.clear();
      g.cover_from.clear();
      dirty_ = true;
    }
  }

  // Graphics, with what auto would pick said beside it.
  ImGui::Spacing();
  ImGui::TextUnformatted("Graphics");
  ImGui::SameLine(200);
  int b = index_of(kBackends, 5, g.backend);
  ImGui::SetNextItemWidth(300);
  if (ImGui::Combo("##backend", &b, kBackendWords, 5)) {
    g.backend = kBackends[b];
    dirty_ = true;
  }
  std::optional<pe::Imports> im = imports_for(g.id);
  if (!im && !probing_) probe_imports();
  if (!im) {
    ImGui::TextDisabled(probing_ ? "reading the game's executable..." : "the executable has not been read");
  } else if (!im->ok) {
    ImGui::TextDisabled("auto cannot read the executable (%s); the player falls back to WineD3D", im->error.c_str());
  } else {
    AutoBackend a = auto_backend(*im);
    std::string dlls;
    for (const std::string& d : im->dlls) dlls += (dlls.empty() ? "" : " ") + d;
    ImGui::TextWrapped("auto: %s. %s.", a.backend.c_str(), a.reason.c_str());
    ImGui::TextDisabled("imports: %s", dlls.c_str());
  }
  if (ImGui::Checkbox("Needs a GPU (refuse to start with software rendering)", &g.needs_gpu)) dirty_ = true;

  ImGui::TextUnformatted("Display");
  ImGui::SameLine(200);
  int dm = index_of(kDisplays, 3, g.display);
  ImGui::SetNextItemWidth(300);
  if (ImGui::Combo("##display", &dm, kDisplayWords, 3)) {
    g.display = kDisplays[dm];
    dirty_ = true;
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("fullscreen", &g.fullscreen)) dirty_ = true;

  ImGui::TextUnformatted("Gamepad");
  ImGui::SameLine(200);
  ImGui::TextDisabled(g.gamepad.empty() ? "kretro's default map" : "this map, one button=key a line");
  if (f && !f->meta.input.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("the map from its pack")) {
      g.gamepad = format_gamepad(f->meta.input);
      dirty_ = true;
    }
  }
  if (!g.gamepad.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("kretro's default")) {
      g.gamepad.clear();
      dirty_ = true;
    }
    if (input_multiline("##pad", g.gamepad, 2048, ImVec2(-1, 90))) dirty_ = true;
  }

  // dgVoodoo: never ours to ship, sometimes the author's.
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("dgVoodoo and other files for this game");
  ImGui::TextWrapped(
      "kretro never bundles dgVoodoo: its licence forbids shipping it with a launcher for general use. "
      "You may add its files for this one game, as its author allows; they are placed beside the game's "
      "executable. Whether you may distribute them is yours to know.");
  int drop = -1;
  for (size_t i = 0; i < g.extra_dlls.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    ImGui::Text("%s  %s", g.extra_dlls[i].name.c_str(), human_size(g.extra_dlls[i].data.size()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("remove")) drop = static_cast<int>(i);
    ImGui::PopID();
  }
  if (drop >= 0) {
    g.extra_dlls.erase(g.extra_dlls.begin() + drop);
    dirty_ = true;
  }
  if (ImGui::SmallButton("Add files...")) browse_ = Browse::Dll;

  // The CD key.
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("CD key");
  std::string vault = install::key_for(keys_, g.id);
  std::string fragment = f ? f->meta.registry.fragment : std::string();
  if (vault.empty()) {
    ImGui::TextDisabled("The keys vault has no key for %s (kretro key %s <key> stores one).", g.id.c_str(),
                        g.id.c_str());
  }
  bool embed = g.embed_key;
  if (ImGui::Checkbox("Embed my key in this bundle", &embed)) {
    if (embed) {
      confirm_key_ = true;  // only after the warning
    } else {
      g.embed_key = false;
      dirty_ = true;
    }
  }
  if (confirm_key_) {
    ImGui::OpenPopup("embed-key");
    confirm_key_ = false;
  }
  if (ImGui::BeginPopupModal("embed-key", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushTextWrapPos(560);
    warn_text("Your key will be in every copy of this bundle, and anyone who has a copy can read it. If it "
              "is shared further, it is your key that is shared.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Embed it")) {
      g.embed_key = true;
      if (g.key_path.empty()) {
        KeySpot spot = suggest_key_spot(fragment, vault);
        g.key_path = spot.path;
        g.key_value = spot.value;
      }
      dirty_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep it out")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (g.embed_key) {
    warn_text("Embedded: in every copy, readable by anyone.");
    ImGui::TextUnformatted("Registry key");
    ImGui::SameLine(200);
    ImGui::SetNextItemWidth(-1);
    if (input("##keypath", g.key_path, 512)) dirty_ = true;
    ImGui::TextUnformatted("Value name");
    ImGui::SameLine(200);
    ImGui::SetNextItemWidth(300);
    if (input("##keyvalue", g.key_value, 128)) dirty_ = true;
    ImGui::TextDisabled("Where the game's installer keeps its key. This pack does not record the name it "
                        "used; the game's own key under Software is the guess.");
    // Said here because it changes where the key lands: the player writes a
    // 32-bit game's HKLM\Software key under Wow6432Node, where it reads it.
    std::optional<pe::Imports> im = imports_for(g.id);
    if (im && im->ok) {
      ImGui::TextDisabled(im->is64 ? "The game is 64-bit: the key is written at this path as it is."
                                   : "The game is 32-bit: a key under HKLM\\Software is written under "
                                     "Software\\Wow6432Node, which is where a 32-bit program reads it.");
    }
  }
  ImGui::PopID();
}

// ---- 4. check ---------------------------------------------------------------------------------

void Bundles::check_step() {
  std::vector<GameFacts> facts = game_facts();
  std::vector<Check> checks = run_checks(draft_, facts);
  std::vector<std::string> blocking = blockers(draft_, facts);
  if (probing_) ImGui::TextDisabled("Still reading the games' executables; more may appear.");
  if (!blocking.empty()) {
    ImGui::TextUnformatted("Before anything can be built:");
    for (const std::string& b : blocking) warn_text("  " + b);
    ImGui::Spacing();
  }
  if (checks.empty()) {
    good_text("Nothing to fix or acknowledge.");
    return;
  }
  ImGui::TextWrapped("Fix each of these, or acknowledge it: the bundle is built only when every one is either.");
  ImGui::Spacing();
  for (const Check& c : checks) {
    ImGui::PushID(c.id.c_str());
    bool acked = draft_.acked(c.id);
    if (ImGui::Checkbox("##ack", &acked)) {
      if (acked) draft_.acknowledged.push_back(c.id);
      else draft_.acknowledged.erase(std::remove(draft_.acknowledged.begin(), draft_.acknowledged.end(), c.id),
                                     draft_.acknowledged.end());
      dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", c.text.c_str());
    if (!c.fix.empty()) ImGui::TextDisabled("To fix: %s", c.fix.c_str());
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::PopID();
  }
}

// ---- 5. size ----------------------------------------------------------------------------------------

void Bundles::size_step() {
  std::vector<const PackFacts*> packs;
  for (const DraftGame& g : draft_.games) {
    if (const PackFacts* f = facts_for(g.id)) packs.push_back(f);
  }
  uint64_t meta = 0;
  try {
    meta = meta_from_draft(draft_, keys_, "0000-00-00T00:00:00Z", base_licenses_).encode().size();
  } catch (const std::exception&) {
    // A key to embed that is not there: the check step says so; the size is
    // the same to within the key's length.
    Draft d = draft_;
    for (DraftGame& g : d.games) g.embed_key = false;
    meta = meta_from_draft(d, keys_, "", {}).encode().size();
  }
  SizeReport r = size_report(base_bytes_, meta, packs);
  if (!base_trouble_.empty()) warn_text("The runtime's size is unknown: " + base_trouble_);

  if (ImGui::BeginTable("size", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("part");
    ImGui::TableSetupColumn("as it is");
    ImGui::TableSetupColumn("without its discs");
    ImGui::TableHeadersRow();
    auto row = [](const std::string& a, uint64_t b, uint64_t c) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(a.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(human_size(b).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(b == c ? "" : ("about " + human_size(c)).c_str());
    };
    row("runtime and player", r.runtime.bytes, r.runtime.bytes);
    row("pictures and extra files", r.meta, r.meta);
    for (const SizePart& p : r.games) row(p.label, p.bytes, p.without_discs);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("total");
    ImGui::TableNextColumn();
    ImGui::Text("%s  (%llu bytes)", human_size(r.total).c_str(), static_cast<unsigned long long>(r.total));
    ImGui::TableNextColumn();
    if (r.total_without_discs != r.total) ImGui::Text("about %s", human_size(r.total_without_discs).c_str());
    ImGui::EndTable();
  }
  ImGui::Spacing();
  if (r.warnings.empty()) good_text("Under 2 GiB: any host takes it, and it fits on a FAT32 drive.");
  for (const std::string& w : r.warnings) warn_text(w);
  ImGui::TextDisabled("The discs cannot be left out of a pack here: a game's pack goes into the bundle byte for "
                      "byte. Leaving them out means installing the game again without them.");
}

// ---- 6. rights ------------------------------------------------------------------------------------

void Bundles::rights_step() {
  ImGui::TextWrapped(
      "The bundle carries these games whole. kretro does not check whether you may give them to anybody; "
      "it records that you said you may, in the file itself.");
  ImGui::Spacing();
  if (ImGui::Checkbox("I have the right to distribute these games", &draft_.rights)) dirty_ = true;
  if (!draft_.rights) ImGui::TextDisabled("Build stays off until this is ticked.");
}

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
  BuildInputs in{env_or("KRETRO_SELF"), games_dir(), install::keys_file(), env_or("KRETRO_DWARFS"), cache_dir(), {}};
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
  char when[32] = {};
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
  char secs[32];
  std::snprintf(secs, sizeof(secs), "%.0f", build_seconds_);
  build_note_ = "Built " + built_->path.string() + ", " + human_size(built_->size) + ", in " + secs +
                " seconds, and checked end to end.";
  // The build read the base's licence list and remembered it; the size step
  // counts it from now on.
  try {
    base_licenses_ = base_licenses(find_player_base(env_or("KRETRO_SELF")), env_or("KRETRO_DWARFS"), cache_dir(), false);
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

// ---- 8. preview -------------------------------------------------------------------------------

void Bundles::start_preview() {
  preview_log_.clear();
  try {
    fs::path scratch = preview_scratch(cache_dir(), draft_.id);
    preview_.start(draft_.last_built, {}, preview_env(current_env(), env_or("KRETRO_RUNTIME"), scratch), scratch);
    preview_log_.push_back("started " + draft_.last_built + " with an empty HOME at " + (scratch / "home").string());
  } catch (const std::exception& ex) {
    preview_log_.push_back(ex.what());
  }
}

void Bundles::preview_step() {
  ImGui::TextWrapped(
      "Runs the built file as somebody who downloaded it would: an empty home directory, none of kretro's "
      "state and none of its environment. You see the first-run check, the launcher and a fresh prefix; "
      "all of it is thrown away when it ends.");
  ImGui::Spacing();
  if (draft_.last_built.empty()) {
    ImGui::TextDisabled("Build it first.");
    return;
  }
  ImGui::Text("%s", draft_.last_built.c_str());
  if (preview_.running()) {
    if (ImGui::Button("Stop", ImVec2(200, 44))) preview_.stop();
  } else {
    if (ImGui::Button("Run it", ImVec2(200, 44))) start_preview();
    if (preview_.started()) {
      ImGui::SameLine();
      ImGui::TextDisabled("ended with status %d", preview_.status());
    }
  }
  ImGui::BeginChild("preview-log", ImVec2(0, 0), true);
  for (const std::string& l : preview_log_) ImGui::TextUnformatted(l.c_str());
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}

// ---- files -----------------------------------------------------------------------------------

// The same plain list over std::filesystem the wizard and the Import page use,
// and for the same reason: no dialog from another toolkit that a pad cannot
// drive.
void Bundles::browser() {
  std::error_code ec;
  ImGui::Spacing();
  ImGui::Separator();
  const char* what = browse_ == Browse::Folder ? "Choose a folder"
                     : browse_ == Browse::Dll  ? "Choose a file for this game"
                                               : "Choose a PNG";
  ImGui::TextUnformatted(what);
  ImGui::SameLine();
  if (ImGui::SmallButton("close")) browse_ = Browse::None;
  if (browse_ == Browse::Folder) {
    ImGui::SameLine();
    if (ImGui::SmallButton("use this folder")) take_file(browse_dir_);
  }
  ImGui::BeginChild("bundle-browse", ImVec2(0, 280), true);
  ImGui::TextDisabled("%s", browse_dir_.c_str());
  if (ImGui::Selectable("..")) browse_dir_ = browse_dir_.parent_path();
  std::vector<fs::path> dirs, files;
  for (const fs::directory_entry& de : fs::directory_iterator(browse_dir_, ec)) {
    std::string n = de.path().filename().string();
    if (!n.empty() && n[0] == '.') continue;
    if (de.is_directory(ec)) { dirs.push_back(de.path()); continue; }
    if (browse_ == Browse::Folder || !de.is_regular_file(ec)) continue;
    std::string ext = de.path().extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (browse_ != Browse::Dll && ext != ".png") continue;
    files.push_back(de.path());
  }
  std::sort(dirs.begin(), dirs.end());
  std::sort(files.begin(), files.end());
  for (const fs::path& d : dirs) {
    ImGui::PushID(d.c_str());
    if (ImGui::Selectable((d.filename().string() + "/").c_str())) browse_dir_ = d;
    ImGui::PopID();
  }
  for (const fs::path& f : files) {
    ImGui::PushID(f.c_str());
    if (ImGui::Selectable(f.filename().c_str())) take_file(f);
    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    ImGui::TextDisabled("%s", human_size(fs::file_size(f, ec)).c_str());
    ImGui::PopID();
  }
  ImGui::EndChild();
}

void Bundles::take_file(const fs::path& p) {
  std::error_code ec;
  if (browse_ == Browse::Folder) {
    draft_.out_dir = p.string();
    dirty_ = true;
    browse_ = Browse::None;
    return;
  }
  uint64_t size = fs::file_size(p, ec);
  uint64_t cap = browse_ == Browse::Dll ? kMaxExtraFile : kMaxPicture;
  if (ec || size > cap) {
    trouble_ = p.filename().string() + " is " + human_size(size) + ", more than the " + human_size(cap) +
               " a bundle takes for one.";
    return;
  }
  std::string bytes = slurp(p);
  if (browse_ != Browse::Dll && !is_png(bytes)) {
    trouble_ = p.filename().string() + " is not a PNG.";
    return;
  }
  trouble_.clear();
  switch (browse_) {
    case Browse::Banner: draft_.banner = bytes; draft_.banner_from = p.string(); break;
    case Browse::Icon: draft_.icon = bytes; draft_.icon_from = p.string(); break;
    case Browse::Cover:
      if (game_pick_ < draft_.games.size()) {
        draft_.games[game_pick_].cover = bytes;
        draft_.games[game_pick_].cover_from = p.string();
      }
      break;
    case Browse::Dll:
      if (game_pick_ < draft_.games.size()) {
        std::string name = p.filename().string();
        if (!kg::id_is_safe(name)) {
          trouble_ = name + " cannot be placed beside a game under that name: rename it first.";
          return;
        }
        auto& dl = draft_.games[game_pick_].extra_dlls;
        dl.erase(std::remove_if(dl.begin(), dl.end(), [&](const GameMeta::Dll& x) { return x.name == name; }),
                 dl.end());
        dl.push_back({name, bytes});
      }
      break;
    default: break;
  }
  dirty_ = true;
  browse_ = Browse::None;
}

}  // namespace kg::gui
