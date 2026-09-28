// The Bundles page's wiring: coming and going, the facts the steps are
// computed from, the list of bundles and the page one bundle is drawn on. Each
// step is a file of its own beside this one.
#include "bundles_page.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <exception>
#include <string>
#include <vector>

#include "../../util/env.h"
#include "../../util/paths.h"
#include "../../util/safe_names.h"
#include "../format.h"
#include "../widgets.h"
#include "choices.h"

namespace kg::gui {
namespace fs = std::filesystem;
using namespace kg::bundle;
using bundles_detail::step_word;

Bundles::Bundles(const rt::Env& e, Fonts fonts, Textures* textures) : env_(e), fonts_(fonts), textures_(textures) {}

Bundles::~Bundles() {
  // kretro is closing. A build still running is cancelled - build_bundle
  // removes its .partial on the way out - and waited for, because it is
  // writing into members of this object.
  cancel_ = true;
  if (worker_.joinable()) worker_.join();
  if (prober_.joinable()) prober_.join();
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
      facts_[e.id] = read_pack_facts(e.pack, e.id);
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
  for (std::string& l : preview_.poll()) preview_log_.push_back(std::move(l));
  if (editing_) bundle_page();
  else list_page();
}

void Bundles::trouble_line() {
  if (trouble_.empty()) return;
  badge_line(BadgeKind::Fail, trouble_);
  ImGui::Spacing();
}

bool Bundles::tick_box(const char* label, bool* v, bool compact) {
  // The theme's frame outline is meant to sit quietly around a field; around
  // an empty box it all but disappears, so a box to tick gets the accent's
  // dim shade. Compact, it is a line of text tall and keeps the text's
  // baseline, so the sentence beside it lines up with it.
  ImGui::PushStyleColor(ImGuiCol_Border, kAccentDim);
  if (compact) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x, std::round(px(2))));
  }
  const bool changed = ImGui::Checkbox(label, v);
  if (compact) ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  return changed;
}

namespace {

// The strip under a bundle's card that holds its Open and Build again: a gap
// and a ghost button's height.
float card_strip_h() { return std::round(px(10)) + std::round(px(36)); }

}  // namespace

void Bundles::list_page() {
  set_chrome_path("~/bundles");
  PageWindow page("Bundles", "Esc back", fonts_.big());
  ImGui::TextWrapped(
      "A bundle is one file that carries a runtime and the games you choose from your shelf. Whoever "
      "downloads it runs it and plays; they need nothing else. Each bundle you build is remembered here, "
      "so the next version is one click.");
  ImGui::Spacing();
  if (!base_trouble_.empty()) {
    badge_line(BadgeKind::Warn, base_trouble_);
    ImGui::Spacing();
  }
  for (const std::string& u : unreadable_) badge_line(BadgeKind::Fail, "Cannot read a remembered bundle: " + u);
  vgap(4);
  section("bundles");

  // The cards scroll in a region of their own under the page's words, the
  // keyboard and the pad moving through it as though it were the page.
  begin_scroll("bundles_list", ImVec2(0, 0));
  // Room for a card grown by the focus at the top of the region.
  vgap(6);
  Grid g = tile_grid(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize, px(280), 0.5625f);
  // Never wider than a card reads well at: two cards to a narrow window are
  // otherwise half of it each, and New bundle a slab.
  if (g.w > px(340)) {
    g.w = std::floor(px(340));
    g.h = std::floor(g.w * 0.5625f);
  }
  const float card_h = g.h + card_strip_h();
  const ImGuiStyle& st = ImGui::GetStyle();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, g.gap));
  grid_begin();
  // New bundle first, then the bundles as they are remembered. The list
  // opens on the first bundle, New bundle being there for when there is
  // none.
  if (new_card(g.w, card_h)) start_new();
  if (remembered_.empty()) default_focus();
  std::string open_id, again_id;
  for (size_t i = 0; i < remembered_.size(); ++i) {
    const Draft& d = remembered_[i];
    if ((i + 1) % static_cast<size_t>(g.cols)) ImGui::SameLine(0, g.gap);
    bool open_it = false, again = false;
    bundle_card(d, g.w, g.h, i == 0, &open_it, &again);
    if (open_it) open_id = d.id;
    if (again) again_id = d.id;
  }
  grid_end();
  ImGui::PopStyleVar();
  if (remembered_.empty()) {
    vgap(12);
    empty_state("No bundles yet.", "\xe2\x96\xa1", std::round(px(120)));  // U+25A1, an empty box
  }
  end_scroll();
  // Acted on once the list is drawn, which they change.
  if (!again_id.empty()) {
    // The one click the design promises: straight to Build with every field
    // as it was left. The Build step still refuses what it would refuse.
    open_bundle(again_id, Step::Build);
    start_build();
  } else if (!open_id.empty()) {
    open_bundle(open_id, Step::Identity);
  }
}

const Texture* Bundles::card_art(const Draft& d) {
  if (!textures_) return nullptr;
  // Named by where the picture came from and its size, so a picture changed
  // in the editor is read again rather than shown as it was.
  if (!d.banner.empty()) {
    return textures_->png("bundle-card:" + d.id + ":" + d.banner_from + ":" + std::to_string(d.banner.size()), d.banner);
  }
  for (const DraftGame& g : d.games) {
    if (g.cover.empty()) continue;
    return textures_->png("bundle-card:" + d.id + ":" + g.id + ":" + g.cover_from + ":" + std::to_string(g.cover.size()),
                          g.cover);
  }
  return nullptr;
}

bool Bundles::new_card(float w, float h) {
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("New bundle", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 b(a.x + w, a.y + h);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hovered) dl->AddRectFilled(a, b, u32(kBg1));
  // A dashed frame: a place for a card rather than a card.
  const float t = std::max(1.0f, std::round(px(1)));
  const float dash = std::round(px(8)), gap = std::round(px(6));
  const ImU32 c = u32(hovered ? kAccentDim : kLine);
  const float step = dash + gap;
  const int across = static_cast<int>(std::ceil(w / step)), down = static_cast<int>(std::ceil(h / step));
  for (int i = 0; i < across; ++i) {
    const float x = a.x + step * static_cast<float>(i), x1 = std::min(b.x, x + dash);
    dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x1, a.y + t), c);
    dl->AddRectFilled(ImVec2(x, b.y - t), ImVec2(x1, b.y), c);
  }
  for (int i = 0; i < down; ++i) {
    const float y = a.y + step * static_cast<float>(i), y1 = std::min(b.y, y + dash);
    dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + t, y1), c);
    dl->AddRectFilled(ImVec2(b.x - t, y), ImVec2(b.x, y1), c);
  }
  ImFont* big = fonts_.big();
  const float bs = font_px(big);
  const char* plus = "+";
  const char* words = "new bundle";
  const ImVec2 pw = big->CalcTextSizeA(bs, FLT_MAX, 0, plus);
  const ImVec2 ww = ImGui::CalcTextSize(words);
  const float total = pw.y + std::round(px(4)) + ww.y;
  const float y0 = std::round(a.y + (h - total) * 0.5f);
  const ImU32 ink = u32(hovered ? kAccent : kDim);
  dl->AddText(big, bs, ImVec2(std::round(a.x + (w - pw.x) * 0.5f), y0), ink, plus);
  dl->AddText(ImVec2(std::round(a.x + (w - ww.x) * 0.5f), y0 + pw.y + std::round(px(4))), ink, words);
  return pressed;
}

void Bundles::bundle_card(const Draft& d, float w, float h, bool first, bool* open, bool* again) {
  ImGui::PushID(d.id.c_str());
  ImGui::BeginGroup();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float strip = card_strip_h();
  // Open and Build again show while the card has the focus or the pointer,
  // and the card says when it was last built the rest of the time. They are
  // there all the same, so the keys reach them from the card and their IDs
  // never change; whether the focus was on either is from the frame before.
  ImGuiStorage* store = ImGui::GetStateStorage();
  const ImGuiID was_on = ImGui::GetID("##buttons-focused");
  const bool over = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(at, ImVec2(at.x + w, at.y + h + strip));

  TileSpec t;
  t.id = d.id;
  t.name = d.title.empty() ? d.id : d.title;
  t.sub = "v" + d.version + "  \xc2\xb7  " + std::to_string(d.games.size()) + (d.games.size() == 1 ? " game" : " games");
  t.art = card_art(d);
  t.big = fonts_.big();
  t.w = w;
  t.h = h;
  bool tile_focused = false;
  t.focused = &tile_focused;
  // A card is opened as its Open button opens it.
  if (tile_ex(t)) *open = true;
  if (first) default_focus();
  if (ImGui::IsItemHovered()) {
    std::string games;
    for (const DraftGame& g : d.games) games += (games.empty() ? "" : ", ") + g.name;
    ImGui::SetTooltip("%s %s\n%s\n%s", d.id.c_str(), d.version.c_str(), games.empty() ? "no games" : games.c_str(),
                      d.last_built.empty() ? "not built yet" : d.last_built.c_str());
  }
  const bool shown = tile_focused || store->GetBool(was_on) || over;
  const float a = anim01(ImGui::GetID("##reveal"), shown, 0.12f);

  const float gap = std::round(px(10));
  const float bh = std::round(px(36));
  const float y = at.y + h + gap;
  // When it was last built, under the card while its buttons are hidden:
  // the one fades out before the other fades in, so the two never show at
  // once.
  const float words_a = std::max(0.0f, 1.0f - a * 2.0f), buttons_a = std::max(0.0f, a * 2.0f - 1.0f);
  if (words_a > 0.0f) {
    ImFont* small = fonts_.small();
    const std::string when =
        d.last_built.empty() ? std::string("not built yet")
                             : "last built " + d.last_built_at + (d.last_size ? ", " + human_size(d.last_size) : "");
    const std::string line = elide(small, when, w);
    ImGui::GetWindowDrawList()->AddText(small, font_px(small), ImVec2(at.x, std::round(y + (bh - font_px(small)) * 0.5f)),
                                        u32(kDim, words_a), line.c_str());
  }
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * buttons_a);
  const float half = std::floor((w - gap) * 0.5f);
  ImGui::SetCursorScreenPos(ImVec2(at.x, y));
  if (ghost_button("Open", ImVec2(half, bh))) *open = true;
  bool buttons_focused = ImGui::IsItemFocused();
  ImGui::SameLine(0, gap);
  if (ghost_button("Build again", ImVec2(w - half - gap, bh))) *again = true;
  buttons_focused = buttons_focused || ImGui::IsItemFocused();
  ImGui::PopStyleVar();
  store->SetBool(was_on, buttons_focused);
  ImGui::EndGroup();
  ImGui::PopID();
}

namespace {

// The words the stepper names the steps by, in Step's order.
const char* const kStepWords[] = {"identity", "games", "each game", "check", "size", "rights", "build", "preview"};

}  // namespace

void Bundles::go_to(Step s) {
  if (dirty_) {
    try {
      remember();
    } catch (const std::exception& ex) {
      trouble_ = ex.what();
    }
  }
  step_ = s;
  browse_ = Browse::None;
  if (step_ == Step::PerGame || step_ == Step::Check) refresh_facts();
}

void Bundles::bundle_page() {
  std::string title = "Bundle: " + (draft_.title.empty() ? draft_.id : draft_.title);
  {
    std::string word = step_word(static_cast<int>(step_));
    // Checked, so that a step word made only of digits one day names the
    // path as it is rather than throwing on every frame.
    const size_t k = word.find_first_not_of("0123456789 ");
    if (k != std::string::npos) word = word.substr(k);
    for (char& c : word) c = c == ' ' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    set_chrome_path("~/bundles/" + draft_.id + "/" + word);
  }
  // Each step is a view of its own for the focus: it opens on the step's
  // first control, and going back to a step goes back to where it was.
  set_page_focus_key("step " + std::to_string(static_cast<int>(step_)));
  // The header is one line, where the bundle is, as a flow's header is: the
  // stepper under it says which step, and the step keeps the room.
  set_page_trail("kretro \xe2\x80\xba bundles \xe2\x80\xba " + (draft_.title.empty() ? draft_.id : draft_.title));
  PageWindow page(title.c_str(), "Esc back to the bundles", fonts_.big());
  trouble_line();

  std::vector<GameFacts> facts = game_facts();
  std::vector<Check> checks = run_checks(draft_, facts);
  const std::vector<std::string> blocking = blockers(draft_, facts);
  const size_t open = std::count_if(checks.begin(), checks.end(), [&](const Check& c) { return !draft_.acked(c.id); });
  const bool ready = ready_to_build(draft_, checks, blocking);

  // Whether each step has what it asks for, for the tick on its node. Only a
  // mark: every step stays open to go to, and Build decides for itself.
  auto done = [&](Step s) {
    switch (s) {
      case Step::Identity:
        return !draft_.title.empty() && kg::id_is_safe(draft_.id) && version_is_safe(draft_.version);
      case Step::Games:
      case Step::PerGame:
      case Step::Size: return !draft_.games.empty();
      case Step::Check: return !draft_.games.empty() && open == 0 && blocking.empty();
      case Step::Rights: return draft_.rights;
      // A build made before, but only while this build could be made again,
      // so the stepper does not call a blocked step fine.
      case Step::Build: return !draft_.last_built.empty() && ready;
      case Step::Preview: return preview_.started();
    }
    return false;
  };
  // What is still open on a step, so the stepper says where to go next: the
  // checks neither fixed nor acknowledged, and the rights not yet claimed.
  auto info = [&](int i) {
    const Step s = static_cast<Step>(i);
    if (s == Step::Check && open) return StepInfo{StepState::Warn, static_cast<int>(open)};
    if (s == Step::Rights && !draft_.rights) return StepInfo{StepState::Fail, 0};
    return StepInfo{done(s) ? StepState::Done : StepState::Todo, 0};
  };
  constexpr int n = static_cast<int>(Step::Preview) + 1;
  nav_section_begin("stepper");
  const int picked = stepper("steps", kStepWords, n, static_cast<int>(step_), info, [](int) { return true; });
  nav_section_end();
  vgap(6);

  // The step, in a pane that scrolls, above the footer.
  nav_section_begin("content");
  const float foot = flow_footer_height() + ImGui::GetStyle().ItemSpacing.y;
  // Flattened, so the arrows and the pad move between the stepper, the step
  // and the footer with no Enter to go into it.
  ImGui::BeginChild("step", ImVec2(0, -foot), ImGuiChildFlags_NavFlattened);
  centre_column(content_max_w(Content::Form));
  default_focus_next();
  // A file list being chosen from is what the step is doing now, so it
  // heads the pane, over the step it fills in. Below the step, on a short
  // window it started out of sight.
  if (browse_ != Browse::None) {
    browser();
    vgap(8);
  }
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
  if (browse_ == Browse::None) browse_shown_ = Browse::None;
  // Room under the last line, so a step scrolled to its end never has its
  // last line on the clip edge.
  vgap(bundles_detail::kStepFoot);
  end_centre_column();
  step_more_below();
  ImGui::EndChild();
  nav_section_end();

  // Back is Escape's way out of the bundle; the step's way on is Build on
  // the Build step and the next step on the others. Whether the bundle is
  // written down yet is said beside it.
  nav_section_begin("footer");
  FlowFooter f;
  f.back = "Back";
  std::string next;
  std::string why;
  if (step_ == Step::Build) {
    f.primary = "Build";
    f.primary_enabled = ready && base_trouble_.empty() && !worker_.joinable();
    why = worker_.joinable()       ? "building"
          : !base_trouble_.empty() ? "no player base to build with"
          : !blocking.empty()      ? blocking.front()
          : !draft_.rights         ? "rights not yet ticked"
                                   : std::to_string(open) + " check(s) open";
    f.reason = why.c_str();
  } else if (step_ != Step::Preview) {
    next = std::string("Next: ") + kStepWords[static_cast<int>(step_) + 1];
    f.primary = next.c_str();
  }
  f.note = dirty_ ? "* unsaved" : "saved";
  const FlowAction act = flow_footer(f);
  nav_section_end();

  if (picked >= 0) go_to(static_cast<Step>(picked));
  if (act == FlowAction::Back) back();
  if (act == FlowAction::Primary) {
    // Every step but Build and Preview has a Next, to the step after it.
    if (step_ == Step::Build) start_build();
    else if (step_ != Step::Preview) go_to(static_cast<Step>(static_cast<int>(step_) + 1));
  }
}

// A step taller than the pane scrolls, and its last visible line is cut
// through the glyphs at the pane's edge, which reads as a drawing fault. A
// fade over the edge and a word at the corner say there is more below.
void Bundles::step_more_below() {
  if (ImGui::GetScrollMaxY() <= 0 || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float left = wp.x, right = wp.x + ImGui::GetWindowContentRegionMax().x;
  const float bottom = wp.y + ImGui::GetWindowHeight();
  const float fade = std::round(ImGui::GetTextLineHeight() * 2.5f);
  const ImU32 clear = u32(kBg0, 0.0f), solid = u32(kBg0);
  dl->AddRectFilledMultiColor(ImVec2(left, bottom - fade), ImVec2(right, bottom), clear, clear, solid, solid);
  ImFont* f = fonts_.small();
  const float size = font_px(f);
  const char* more = "more \xe2\x86\x93";  // U+2193, a downwards arrow
  const ImVec2 w = f->CalcTextSizeA(size, FLT_MAX, 0, more);
  dl->AddText(f, size, ImVec2(std::round(right - w.x), std::round(bottom - w.y - px(2))), u32(kDim), more);
}

}  // namespace kg::gui
