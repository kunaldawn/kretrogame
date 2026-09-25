// The grid: the game the focus is on as a hero band across the top of the
// window, with the bundle's banner over it, Play and how much the game has
// been played; under it a row of the bundle's games, or a grid of them when
// there are many; and the way to the bundle's settings.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../../session/journal.h"
#include "../format.h"
#include "../texture.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {

namespace {

constexpr float kTileAspect = 0.75f;
// Past this many games a row is a long way to walk; the games are a grid.
constexpr size_t kRowMax = 12;
// How much a card in the row grows with the focus, as a console's does.
constexpr float kCardGrow = 0.08f;

// The banner over the rectangle at `at`, `w` by `h`. Its height was capped,
// so the strip is wider than the picture: the picture is cropped about its
// middle to fill it, but never to less than 60% of its rows, since a banner
// often carries lettering. Past that it is shown whole, centred on a panel
// that still spans the strip.
void banner(const Texture& b, ImVec2 at, float w, float h) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float thin = std::max(1.0f, std::round(px(1)));
  const float img = static_cast<float>(b.w) / static_cast<float>(std::max(b.h, 1));
  float keep = std::clamp(img / (w / h), 0.0f, 1.0f);
  float dw = w;
  if (keep < 0.6f) {
    keep = 0.6f;
    dw = std::floor(h * img / keep);
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), u32(kBg1));
  }
  const float x = at.x + std::floor((w - dw) * 0.5f);
  const float v0 = (1.0f - keep) * 0.5f;
  draw_image(dl, b, ImVec2(x, at.y), ImVec2(x + dw, at.y + h), ImVec2(0, v0), ImVec2(1, 1.0f - v0));
  dl->AddRect(ImVec2(at.x - thin, at.y - thin), ImVec2(at.x + w + thin, at.y + h + thin), u32(kLine), 0.0f, 0, thin);
}

// The height a row of cards `card_h` tall takes, the room carousel_begin
// keeps round a card that grows kCardGrow included.
float row_height(float card_h) {
  const float more = std::max(0.0f, std::round(card_h * (kCardGrow - 0.035f) * 0.5f));
  const float pad = std::max(std::round(card_h * 0.03f), std::round(px(10))) + more;
  return std::round(card_h + pad * 2);
}

}  // namespace

void GridPage::draw() {
  const bundle::BundleMeta& m = ctx_.p.bundle().meta;
  // The hero band runs to the window's top and the banner over it says whose
  // player this is, so the page has no header of its own.
  set_page_bare();
  PageWindow page(m.title.c_str(), "arrows move   Enter play   Esc quit", ctx_.w.fonts().big());
  const ImGuiStyle& st = ImGui::GetStyle();
  const size_t n = ctx_.ids.size();
  const ImVec2 win = ImGui::GetWindowPos();
  const float win_w = ImGui::GetWindowWidth();
  const float page_left = ImGui::GetCursorScreenPos().x;
  const float page_w = ImGui::GetContentRegionAvail().x;
  // On a very wide window what the page lays out keeps to the grid's column,
  // centred, as the shelf's tiles do; the band's picture still spans the
  // window.
  const float avail_w = std::min(page_w, content_max_w(Content::Grid));
  const bool narrowed = avail_w < page_w;
  const float left = page_left + std::floor((page_w - avail_w) * 0.5f);
  // Everything down to the window's bottom margin, the hero band from the
  // very top edge.
  const float page_h = ImGui::GetWindowHeight() - st.WindowPadding.y;
  const bool compact = breakpoint() == Breakpoint::Compact;
  const bool many = n > kRowMax;

  // The game the hero band is about: the one the focus is on, or was last on
  // (Play and the settings leave it where it was); on coming back, the one
  // last opened, which is where the focus opens too.
  auto listed = [&](const std::string& id) { return std::find(ctx_.ids.begin(), ctx_.ids.end(), id) != ctx_.ids.end(); };
  const std::string opens = listed(ctx_.selected) ? ctx_.selected : (n ? ctx_.ids.front() : std::string());
  if (page_appearing() || !listed(hero_id_)) hero_id_ = opens;
  // A change of game cross-fades the band from the one it showed; coming
  // back to the page it simply shows the game.
  bool changed = false;
  if (hero_id_ != shown_id_) {
    was_id_ = page_appearing() ? std::string() : shown_id_;
    shown_id_ = hero_id_;
    changed = !was_id_.empty();
  }
  const float mix = fade_in(ImGui::GetID("##hero-mix"), changed, 0.15f);
  // Read when the page comes up, when the band moves to another game, and
  // after a session; not on every frame.
  if (!hero_id_.empty() && disk_.due(hero_id_ + "\n" + std::to_string(ctx_.generation))) {
    journal_ = session::journal(hero_id_);
  }

  // ---- how the height is shared ----------------------------------------------
  // The band holds, from the top: the banner strip, the year and the title,
  // then Play with the stats. Under it, the row of cards, the focused game's
  // name, and what just happened, if anything.
  ImFont* small = font(FontRole::Small);
  const float top_pad_d = compact ? 14.0f : 20.0f;
  const float strip_d = compact ? 32.0f : 40.0f;
  const float top_d = top_pad_d + strip_d + 12.0f;
  const float foot_pad_d = compact ? 16.0f : 24.0f;
  const float foot_d = foot_pad_d + 56.0f + (compact ? 14.0f : 18.0f);
  const float hero_min = std::round(px(top_d) + font_px(small) + px(8) + font_px(font(FontRole::Hero)) + px(foot_d) + px(8));
  const float badge_w = ImGui::CalcTextSize("[    ]").x;
  float status_h = 0;
  if (!ctx_.status.empty()) {
    const float wrap = avail_w - badge_w - badge_gap();
    status_h = ImGui::CalcTextSize(ctx_.status.c_str(), nullptr, false, wrap).y + st.ItemSpacing.y;
  }
  const float foot_line = ImGui::GetFrameHeight() + st.ItemSpacing.y;
  float hero_h = 0, card_h = 0;
  if (many) {
    hero_h = std::max(std::round(px(compact ? 220 : 260)), hero_min);
  } else {
    // The row takes a good part of the page, the band the rest; on a short
    // window the band keeps what it needs and the cards give way.
    const float below = st.ItemSpacing.y + foot_line + status_h;
    card_h = std::max(std::round(px(compact ? 150 : 180)), std::round(page_h * (compact ? 0.45f : 0.38f)));
    while (page_h - below - row_height(card_h) < hero_min && card_h > px(90)) card_h -= std::round(px(4));
    hero_h = std::max(hero_min, page_h - below - row_height(card_h));
  }

  // ---- the band --------------------------------------------------------------
  nav_section_begin("hero");
  ImGui::SetCursorScreenPos(ImVec2(left, win.y));
  float text_right = win.x + win_w;
  if (!hero_id_.empty()) {
    const bundle::GameMeta& g = ctx_.p.game(hero_id_);
    HeroSpec hs;
    hs.art = ctx_.cover(g.id);
    hs.backdrop = hs.art ? ctx_.cover_backdrop(g.id) : nullptr;
    hs.id = g.id;
    hs.title = g.name;
    hs.kicker = g.year ? std::to_string(g.year) : std::string();
    hs.height = hero_h / ui_scale();
    hs.top = top_d;
    hs.foot = foot_d;
    if (narrowed) hs.content_right = left + avail_w;
    if (!was_id_.empty() && listed(was_id_)) {
      hs.was_id = was_id_;
      hs.was_art = ctx_.cover(was_id_);
      hs.was_backdrop = hs.was_art ? ctx_.cover_backdrop(was_id_) : nullptr;
      hs.mix = mix;
    }
    text_right = hero(hs);
  } else {
    ImGui::Dummy(ImVec2(avail_w, hero_h));
  }
  const float band_bottom = win.y + hero_h;

  // Play, and beside it how much the game has been played, along the band's
  // foot. The same Play the game's own page has; the card itself still opens
  // that page.
  const float play_h = std::round(px(56));
  const float row_y = band_bottom - std::round(px(foot_pad_d)) - play_h;
  if (!hero_id_.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(left, row_y));
    if (play_button("Play", PlayKind::Play)) {
      ctx_.selected = hero_id_;
      ctx_.play(hero_id_);
    }
    float x = ImGui::GetItemRectMax().x;
    auto figure = [&](const char* label, const std::string& value) {
      const ImVec2 sz = stat_size(label, value);
      const float at = x + std::round(px(32));
      // Only what fits before the framed picture; a stat is never cut.
      if (at + sz.x > text_right) {
        x = text_right;
        return;
      }
      ImGui::SetCursorScreenPos(ImVec2(at, row_y + std::round((play_h - sz.y) * 0.5f)));
      stat(label, value);
      x = at + sz.x;
    };
    if (!journal_.empty()) {
      double total = 0;
      for (const session::Record& r : journal_) total += static_cast<double>(r.ended - r.started);
      figure("last played", ago(journal_.front().ended));
      figure("play time", human_time(total));
    } else {
      figure("played", "never");
    }
  }

  // The bundle's own name at the band's top left: its banner, as a strip,
  // or its title in the accent.
  const float strip_y = win.y + std::round(px(top_pad_d));
  const float strip_h = std::round(px(strip_d));
  const Texture* art = ctx_.textures.png("banner", m.banner);
  const float gh = std::round(px(compact ? 36 : 40));
  const char* shown = "\xe2\x89\xa1 settings";
  const float gw = std::round(ImGui::CalcTextSize(shown).x + px(16) * 2);
  const float gx = (narrowed ? left + avail_w : win.x + win_w - std::round(px(32))) - gw;
  const float brand_room = std::max(1.0f, std::min(gx - left - std::round(px(24)), std::round(px(520))));
  if (art && art->w > 0 && art->h > 0) {
    const float ratio = static_cast<float>(art->w) / static_cast<float>(art->h);
    float bw = std::round(strip_h * ratio), bh = strip_h;
    if (bw > brand_room) {
      bw = brand_room;
      bh = std::max(1.0f, std::round(bw / ratio));
    }
    banner(*art, ImVec2(left, strip_y + std::round((strip_h - bh) * 0.5f)), bw, bh);
  } else {
    ImFont* big = ctx_.w.fonts().big();
    const std::string t = elide(big, m.title, brand_room);
    ImGui::GetWindowDrawList()->AddText(big, font_px(big), ImVec2(left, strip_y + std::round((strip_h - font_px(big)) * 0.5f)),
                                        u32(kAccent), t.c_str());
  }
  // The bundle's settings at the top right, as a console puts its menu. The
  // button keeps its old label as its ID; the pad's Start opens it too.
  ImGui::SetCursorScreenPos(ImVec2(gx, strip_y + std::round((strip_h - gh) * 0.5f)));
  if (ghost_button("Bundle settings", ImVec2(gw, gh), shown)) ctx_.go(Screen::Bundle);
  ImGui::SetCursorScreenPos(ImVec2(left, band_bottom));
  ImGui::Dummy(ImVec2(0, 0));
  nav_section_end();

  // ---- the games -------------------------------------------------------------
  std::string focused, hovered;
  auto follow = [&](const std::string& id) {
    // The band follows the focus; with the mouse, what it points at.
    if (ImGui::IsItemFocused() && input_mode() != InputMode::Mouse) focused = id;
    if (ImGui::IsItemHovered() && input_mode() == InputMode::Mouse) hovered = id;
  };
  auto open = [&](const std::string& id) {
    ctx_.selected = id;
    ctx_.status.clear();
    ctx_.go(Screen::Game);
  };
  if (!many) {
    centre_column(avail_w);
    nav_section_begin("carousel");
    const float card_w = std::round(card_h / kTileAspect);
    if (carousel_begin("games", card_h, kCardGrow)) {
      for (const std::string& id : ctx_.ids) {
        const bundle::GameMeta& g = ctx_.p.game(id);
        TileSpec t;
        t.id = g.id;
        t.name = g.name;
        t.sub = g.year ? std::to_string(g.year) : "";
        t.art = ctx_.cover(g.id);
        t.big = ctx_.w.fonts().big();
        t.w = card_w;
        t.h = card_h;
        t.rest_bright = 0.82f;
        t.grow = kCardGrow;
        t.caption = false;
        const bool pressed = carousel_item(t);
        // The row opens on the game last opened; coming back to it, on what
        // had the focus, which is that game's card unless it was Play or the
        // settings.
        if (g.id == opens) default_focus();
        follow(g.id);
        if (pressed) open(g.id);
      }
    }
    carousel_end();
    nav_section_end();
    end_centre_column();
  } else {
    nav_section_begin("grid");
    const float grid_h = std::max(1.0f, ImGui::GetContentRegionAvail().y - foot_line - status_h);
    if (begin_scroll("grid", ImVec2(0, grid_h))) {
      centre_column(content_max_w(Content::Grid));
      const Grid grid = tile_grid(ImGui::GetContentRegionAvail().x, px(200), kTileAspect);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, grid.gap));
      grid_begin();
      for (size_t i = 0; i < n; ++i) {
        if (i % static_cast<size_t>(grid.cols)) ImGui::SameLine(0, grid.gap);
        const bundle::GameMeta& g = ctx_.p.game(ctx_.ids[i]);
        const std::string sub = g.year ? std::to_string(g.year) : "";
        const bool pressed = tile(g.id, g.name, sub, ctx_.cover(g.id), ctx_.w.fonts().big(), grid.w, grid.h);
        if (g.id == opens) default_focus();
        follow(g.id);
        if (pressed) open(g.id);
      }
      grid_end();
      ImGui::PopStyleVar();
      end_centre_column();
    }
    end_scroll();
    nav_section_end();
  }
  if (!focused.empty()) hero_id_ = focused;
  else if (!hovered.empty()) hero_id_ = hovered;

  // ---- the foot --------------------------------------------------------------
  // The game the band is about, named under the row (the cards are only
  // their pictures), and how many there are.
  centre_column(avail_w);
  ImGui::AlignTextToFramePadding();
  if (!many && !shown_id_.empty()) {
    const bundle::GameMeta& g = ctx_.p.game(shown_id_);
    const std::string count = std::to_string(n) + (n == 1 ? " game" : " games");
    const float room = avail_w - ImGui::CalcTextSize(count.c_str()).x - std::round(px(24));
    const std::string year = g.year ? "  \xc2\xb7  " + std::to_string(g.year) : std::string();
    const float year_w = ImGui::CalcTextSize(year.c_str()).x;
    ImGui::TextUnformatted(elide(nullptr, g.name, std::max(1.0f, room - year_w)).c_str());
    if (!year.empty()) {
      ImGui::SameLine(0, 0);
      ImGui::TextDisabled("%s", year.c_str());
    }
  } else {
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
  }
  count_line();
  if (!ctx_.status.empty()) badge_line(BadgeKind::Info, ctx_.status, kDim);
  end_centre_column();
}

void GridPage::count_line() {
  const std::string count = std::to_string(ctx_.ids.size()) + (ctx_.ids.size() == 1 ? " game" : " games");
  const float count_w = ImGui::CalcTextSize(count.c_str()).x;
  const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
  ImGui::SameLine();
  ImGui::SetCursorScreenPos(ImVec2(right - count_w, ImGui::GetCursorScreenPos().y));
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", count.c_str());
}

}  // namespace kg::gui::launcher
