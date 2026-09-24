// The shelf: every game as a tile, the filter you type, and the letters that
// open the other screens (the host reads those).
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "../format.h"
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

}  // namespace

void ShelfPage::draw() {
  PageWindow page("kretro",
                  "arrows move   Enter play   A add a game   I import   L library   S settings   "
                  "D doctor   B bundles   Esc quit",
                  ctx_.fonts.big);

  if (!ctx_.filter.empty()) {
    ImGui::TextDisabled("filter: %s", ctx_.filter.c_str());
    ImGui::Spacing();
  }

  std::vector<const Entry*> shown;
  for (const Entry& e : ctx_.entries) {
    if (ctx_.filter.empty() || contains_ci(e.name, ctx_.filter) || contains_ci(e.id, ctx_.filter)) {
      shown.push_back(&e);
    }
  }

  if (shown.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped(
        ctx_.entries.empty()
            ? "No games yet, and no manifests found.\n\nPut a disc image in the iso directory and press A."
            : "Nothing matches that filter.");
    return;
  }

  const float avail = ImGui::GetContentRegionAvail().x;
  const float tile_w = 300.0f, tile_h = 225.0f, pad = 18.0f;
  int cols = std::max(1, static_cast<int>((avail + pad) / (tile_w + pad)));

  ImGui::BeginChild("grid", ImVec2(0, 0), false);
  for (size_t i = 0; i < shown.size(); ++i) {
    if (i % cols) ImGui::SameLine(0, pad);
    tile(*shown[i], tile_w, tile_h);
  }
  ImGui::EndChild();
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
  bool activated = gui::tile(e.id, e.name, sub, art, ctx_.fonts.big, w, h, dim);
  if (activated) {
    ctx_.selected = ctx_.index_of(e.id);
    ctx_.go(Screen::Game);
  }
}

}  // namespace kg::gui::shelf
