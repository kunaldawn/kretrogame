// The grid: the bundle's banner, then one tile per game with its cover, name
// and year.
#include <algorithm>
#include <string>

#include "../texture.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {

void GridPage::banner() {
  const bundle::BundleMeta& m = ctx_.p.bundle().meta;
  if (const Texture* b = ctx_.textures.png("banner", m.banner)) {
    float w = ImGui::GetContentRegionAvail().x;
    float h = std::min(220.0f, w * static_cast<float>(b->h) / static_cast<float>(b->w));
    ImGui::Image(reinterpret_cast<ImTextureID>(b->tex), ImVec2(h * static_cast<float>(b->w) / static_cast<float>(b->h), h));
    ImGui::Spacing();
  }
}

void GridPage::draw() {
  const bundle::BundleMeta& m = ctx_.p.bundle().meta;
  PageWindow page(m.title.c_str(), "arrows move   Enter play   Esc quit", ctx_.w.big);
  banner();
  const float avail = ImGui::GetContentRegionAvail().x;
  const float tw = 300.0f, th = 225.0f, pad = 18.0f;
  const int cols = std::max(1, static_cast<int>((avail + pad) / (tw + pad)));
  ImGui::BeginChild("grid", ImVec2(0, -40), false);
  for (size_t i = 0; i < ctx_.ids.size(); ++i) {
    if (i % cols) ImGui::SameLine(0, pad);
    const bundle::GameMeta& g = ctx_.p.game(ctx_.ids[i]);
    std::string sub = g.year ? std::to_string(g.year) : "";
    if (tile(g.id, g.name, sub, ctx_.cover(g.id), ctx_.w.big, tw, th)) {
      ctx_.selected = g.id;
      ctx_.status.clear();
      ctx_.go(Screen::Game);
    }
  }
  ImGui::EndChild();
  if (ImGui::Button("Bundle settings")) ctx_.go(Screen::Bundle);
  status_line();
}

void GridPage::status_line() {
  if (ctx_.status.empty()) return;
  ImGui::SameLine();
  ImGui::TextWrapped("%s", ctx_.status.c_str());
}

}  // namespace kg::gui::launcher
