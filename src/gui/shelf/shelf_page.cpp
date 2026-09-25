// The shelf: every game as a tile, the games played lately in a row over
// them, the filter you type, and a sidebar of the other screens, whose
// letters the host reads.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "../../install/collection.h"
#include "../format.h"
#include "../palette.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {

namespace {

bool contains_ci(const std::string& hay, const std::string& needle) {
  auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
  return it != hay.end();
}

bool less_ci(const std::string& a, const std::string& b) {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
  });
}

using View = ShelfPage::View;
using Sort = ShelfPage::Sort;

bool in_view(const Entry& e, View v) {
  switch (v) {
    case View::Installed: return e.installed;
    case View::NotInstalled: return !e.installed && !e.blocked;
    case View::Blocked: return e.blocked;
    default: return true;
  }
}

// The views and the sorts by name, in the order they are offered.
const char* const kViewNames[] = {"all", "installed", "not installed", "blocked"};
const char* const kSortNames[] = {"default", "recent", "name", "play time"};

// A combo for one of the shelf's presentation choices: `caption` dim before
// it, and the choice's name in it. The combo takes the focus as any control
// does, but never text, so typing still reaches the host's filter.
template <typename E, size_t N>
void choice(const char* id, const char* caption, const char* const (&names)[N], E& value, float w) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", caption);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(w);
  if (ImGui::BeginCombo(id, names[static_cast<size_t>(value)])) {
    for (size_t i = 0; i < N; ++i) {
      const bool on = static_cast<size_t>(value) == i;
      if (ImGui::Selectable(names[i], on)) value = static_cast<E>(i);
      if (on) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
}

// How wide a choice's combo is: its longest name and the arrow.
template <size_t N>
float choice_w(const char* const (&names)[N]) {
  float widest = 0;
  for (const char* n : names) widest = std::max(widest, ImGui::CalcTextSize(n).x);
  return widest + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2;
}

}  // namespace

void ShelfPage::draw() {
  PageWindow page("kretro",
                  "arrows move   Enter play   A add a game   I import   L library   S settings   "
                  "D doctor   B bundles   Esc quit",
                  ctx_.fonts.big());

  // At compact width the sidebar is a rail with no room for the views, so
  // they are a combo in the header instead; a view chosen there stays chosen.
  const bool compact = breakpoint() == Breakpoint::Compact;
  std::vector<const Entry*> shown;
  for (const Entry& e : ctx_.entries) {
    if (!in_view(e, view_)) continue;
    if (ctx_.filter.empty() || contains_ci(e.name, ctx_.filter) || contains_ci(e.id, ctx_.filter)) {
      shown.push_back(&e);
    }
  }
  switch (sort_) {
    case Sort::Recent:
      std::stable_sort(shown.begin(), shown.end(), [](const Entry* a, const Entry* b) {
        if (a->last_played != b->last_played) return a->last_played > b->last_played;
        return less_ci(a->name, b->name);
      });
      break;
    case Sort::Name:
      std::stable_sort(shown.begin(), shown.end(), [](const Entry* a, const Entry* b) { return less_ci(a->name, b->name); });
      break;
    case Sort::PlayTime:
      std::stable_sort(shown.begin(), shown.end(), [](const Entry* a, const Entry* b) {
        if (a->total_seconds != b->total_seconds) return a->total_seconds > b->total_seconds;
        return less_ci(a->name, b->name);
      });
      break;
    default: break;
  }
  // Where you left off: the games played most lately, while nothing is typed
  // (a filter is a search, and the row would only repeat its hits).
  std::vector<const Entry*> recent;
  if (ctx_.filter.empty()) {
    for (const Entry& e : ctx_.entries)
      if (e.sessions > 0 && in_view(e, view_)) recent.push_back(&e);
    std::stable_sort(recent.begin(), recent.end(),
                     [](const Entry* a, const Entry* b) { return a->last_played > b->last_played; });
    if (recent.size() > 8) recent.resize(8);
  }

  // The shelf opens on the game last opened: its tile in the recent row if
  // it is there, in the grid if not, and the first tile when it is neither.
  const Entry* selected =
      ctx_.entries.empty() ? nullptr : &ctx_.entries[std::min(ctx_.selected, ctx_.entries.size() - 1)];
  // Coming back, the focus is where it was, unless another game was opened
  // while the shelf was away (the wizard's Play it, a game named on the
  // command line): then it is on that one.
  const bool moved = selected && selected->id != drawn_selected_;
  drawn_selected_ = selected ? selected->id : std::string();
  const Entry* target = nullptr;
  bool target_recent = false;
  if (selected && std::find(recent.begin(), recent.end(), selected) != recent.end()) {
    target = selected;
    target_recent = true;
  } else if (selected && std::find(shown.begin(), shown.end(), selected) != shown.end()) {
    target = selected;
  } else if (!recent.empty()) {
    target = recent.front();
    target_recent = true;
  } else if (!shown.empty()) {
    target = shown.front();
  }
  // Where the focus is to open, and, while the focus is not shown on it, the
  // tile marked a step quieter than the focus and not in its amber: it is
  // where the focus comes back to.
  auto mark = [&](const Entry* e, bool in_recent) {
    tiles_item();
    if (e != target || in_recent != target_recent) return;
    default_focus(moved);
    if (!nav_focused()) {
      outline(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), u32(kAccentDim),
              std::max(2.0f, std::round(px(2))));
    }
  };

  const float height = ImGui::GetContentRegionAvail().y;
  nav_section_begin("sidebar");
  sidebar(height);
  nav_section_end();
  ImGui::SameLine(0, std::round(px(compact ? 12 : 20)));
  ImGui::BeginGroup();
  header(shown.size());
  vgap(6);

  // One region scrolls the main column, the recent row with the grid, and
  // at a very wide window the column stops growing and is centred.
  begin_scroll("main", ImVec2(0, 0));
  tiles_begin();
  centre_column(content_max_w(Content::Grid));
  if (shown.empty()) {
    const std::string none = "No games yet, and no manifests found.\n\nPut a disc image in " +
                             install::iso_dir().string() + " and press A.";
    const char* message = ctx_.entries.empty()  ? none.c_str()
                          : ctx_.filter.empty() ? "Nothing on the shelf in this view."
                                                : "Nothing matches that filter.";
    empty_state(message);
  } else {
    if (!recent.empty()) {
      nav_section_begin("recent");
      section("recent");
      const float design_h = compact ? 150.0f : (breakpoint() == Breakpoint::Wide ? 240.0f : 200.0f);
      const float rh = std::round(px(design_h));
      const float rw = std::round(rh * 16.0f / 9.0f);
      if (carousel_begin("recent", rh)) {
        for (const Entry* e : recent) {
          if (recent_tile(*e, rw, rh)) {
            ctx_.selected = ctx_.index_of(e->id);
            ctx_.go(Screen::Game);
          }
          mark(e, true);
        }
      }
      carousel_end();
      nav_section_end();
    }

    nav_section_begin("grid");
    const std::string heading = std::string(view_ == View::All ? "all games" : kViewNames[static_cast<size_t>(view_)]) +
                                " (" + std::to_string(shown.size()) + ")";
    section(heading.c_str());
    // The tiles share the row evenly, so the grid meets both margins; the
    // region keeps its scrollbar's width back itself once it scrolls.
    const Grid g = tile_grid(ImGui::GetContentRegionAvail().x, px(220), 0.75f);
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, g.gap));
    grid_begin();
    for (size_t i = 0; i < shown.size(); ++i) {
      if (i % static_cast<size_t>(g.cols)) ImGui::SameLine(0, g.gap);
      tile(*shown[i], g.w, g.h);
      mark(shown[i], false);
    }
    grid_end();
    ImGui::PopStyleVar();
    nav_section_end();
  }
  end_centre_column();
  tiles_end();
  end_scroll();
  ImGui::EndGroup();
}

// The other screens, each a route to what its letter does, and below them
// the views, with how many games each holds.
void ShelfPage::sidebar(float height) {
  const bool rail = breakpoint() == Breakpoint::Compact;
  size_t installed = 0, not_installed = 0, blocked = 0;
  for (const Entry& e : ctx_.entries) {
    installed += in_view(e, View::Installed);
    not_installed += in_view(e, View::NotInstalled);
    blocked += in_view(e, View::Blocked);
  }
  const int counts[] = {static_cast<int>(ctx_.entries.size()), static_cast<int>(installed),
                        static_cast<int>(not_installed), static_cast<int>(blocked)};
  // The places: the shelf itself, then the host's letters in the order of
  // the hint's screens. With nothing on the shelf, adding a game is the
  // thing to do, so the page opens there.
  std::vector<NavEntry> rows = {
      {"shelf", nullptr, "\xe2\x96\xa6", true},
      {"library", "L", "\xe2\x97\x89"},
      {"import", "I", "\xe2\x87\xa9"},
      {"bundles", "B", "\xe2\x96\xa3"},
      {"add a game", "A", "+", false, -1, nullptr, ctx_.entries.empty()},
      {"settings", "S", "\xe2\x97\x88"},
      {"doctor", "D", "\xe2\x88\x9a"},
  };
  const char keys[] = {0, 'l', 'i', 'b', 'a', 's', 'd'};
  const size_t places = rows.size();
  if (!rail) {
    const char* glyphs[] = {"\xe2\x97\x8b", "\xe2\x97\x8f", "\xe2\x97\x8c", "\xc3\x97"};
    for (size_t i = 0; i < 4; ++i) {
      NavEntry v{kViewNames[i], nullptr, glyphs[i], view_ == static_cast<View>(i), counts[i]};
      if (i == 0) v.heading = "collections";
      rows.push_back(v);
    }
  }
  const int hit = nav_sidebar("nav", rows.data(), static_cast<int>(rows.size()), 0, height);
  if (hit < 0) return;
  const size_t i = static_cast<size_t>(hit);
  if (i < places) {
    if (keys[i] && ctx_.nav) ctx_.nav(keys[i]);
  } else {
    view_ = static_cast<View>(i - places);
  }
}

// Over the main column: what has been typed, as a field that shows it (the
// host takes the typing; the field only says where it went), and the sort,
// with the views too when the sidebar is only a rail.
void ShelfPage::header(size_t hits_n) {
  const bool compact = breakpoint() == Breakpoint::Compact;
  const float fh = ImGui::GetFrameHeight();
  const float room = ImGui::GetContentRegionAvail().x;
  const float pad = ImGui::GetStyle().FramePadding.x;
  const float field_w = std::round(std::min(px(320), room * (compact ? 0.4f : 0.45f)));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const ImVec2 b(a.x + field_w, a.y + fh);
  dl->AddRectFilled(a, b, u32(kBg1));
  outline(dl, a, b, u32(ctx_.filter.empty() ? kLine : kFrameLine));
  ImGui::PushClipRect(a, b, true);
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y));
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kAccentDim, "/");
  ImGui::SameLine();
  if (ctx_.filter.empty()) {
    ImGui::TextDisabled("filter");
  } else {
    // What is typed reads as a shell prompt's input, with the cursor after
    // it, so it is plain that the letters went somewhere.
    const float cell = ImGui::CalcTextSize("M").x;
    const float left = b.x - ImGui::GetCursorScreenPos().x - pad - cell * 2;
    ImGui::TextColored(kCyan, "%s", elide(nullptr, ctx_.filter, left).c_str());
    ImGui::SameLine(0, 0);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight();
    const float top = a.y + std::round((fh - line) * 0.5f);
    // The same blink as the title's cursor, about once a second.
    if (reduce_motion() || std::fmod(ImGui::GetTime(), 1.0) < 0.5) {
      dl->AddRectFilled(ImVec2(at.x, top), ImVec2(at.x + std::round(cell), top + std::round(line)), u32(kAccentDim));
    }
  }
  ImGui::PopClipRect();
  ImGui::SetCursorScreenPos(a);
  ImGui::Dummy(ImVec2(field_w, fh));

  // The choices against the right margin, the sort last.
  const float gap = std::round(px(16));
  const float sp = ImGui::GetStyle().ItemSpacing.x;
  const float sort_w = choice_w(kSortNames);
  const float view_w = choice_w(kViewNames);
  float total = ImGui::CalcTextSize("sort").x + sp + sort_w;
  if (compact) total += ImGui::CalcTextSize("view").x + sp + view_w + gap;
  // How many games what is typed leaves, fading in beside the field.
  const float t = anim01(ImGui::GetID("hits"), !ctx_.filter.empty(), 0.12f);
  if (!ctx_.filter.empty()) {
    const std::string hits = std::to_string(hits_n) + " of " + std::to_string(ctx_.entries.size());
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(alpha(kDim, t), "%s", hits.c_str());
  }
  ImGui::SameLine();
  // No room beside the field: the choices go on a line of their own.
  if (ImGui::GetContentRegionAvail().x < total) ImGui::NewLine();
  else align_right(total);
  if (compact) {
    choice("##view", "view", kViewNames, view_, view_w);
    ImGui::SameLine(0, gap);
  }
  choice("##sort", "sort", kSortNames, sort_, sort_w);
}

void ShelfPage::tile(const Entry& e, float w, float h) {
  std::string sub;
  if (!e.installed) {
    sub = e.blocked ? e.blocked_reason : "not installed - press Enter";
  } else if (e.sessions) {
    sub = human_time(e.total_seconds) + "  \xc2\xb7  " + ago(e.last_played);
  } else {
    sub = e.year ? std::to_string(e.year) : "";
    sub += sub.empty() ? "never played" : "  \xc2\xb7  never played";
  }
  const Texture* art = ctx_.texture(e.title_png.empty() ? e.last_png : e.title_png);
  const uint8_t dim = e.installed ? 0 : (e.blocked ? 150 : 90);
  bool activated = gui::tile(e.id, e.name, sub, art, ctx_.fonts.big(), w, h, dim);
  if (activated) {
    ctx_.selected = ctx_.index_of(e.id);
    ctx_.go(Screen::Game);
  }
}

// A game in the recent row: where you left off rather than its title screen,
// and how long it has been played and when, its picture a little dark until
// the focus reaches it.
bool ShelfPage::recent_tile(const Entry& e, float w, float h) {
  TileSpec t;
  t.id = e.id;
  t.name = e.name;
  t.sub = human_time(e.total_seconds) + "  \xc2\xb7  " + ago(e.last_played);
  t.art = ctx_.texture(e.last_png.empty() ? e.title_png : e.last_png);
  t.big = ctx_.fonts.big();
  t.w = w;
  t.h = h;
  t.dim = e.installed ? 0 : (e.blocked ? 150 : 90);
  t.rest_bright = 0.82f;
  return carousel_item(t);
}

}  // namespace kg::gui::shelf
