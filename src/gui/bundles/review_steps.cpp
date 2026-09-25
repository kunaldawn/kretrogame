#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "../format.h"
#include "../widgets.h"
#include "bundles_page.h"
#include "choices.h"

namespace kg::gui {
using namespace kg::bundle;

// Across the Bundles page a line's severity is its badge's colour alone
// ([FAIL] red, [WARN] amber, [ OK ] green); the sentence after it stays in
// the text colour, so no two reds or greens meet on one line.

// ---- 4. check ---------------------------------------------------------------------------------

void Bundles::check_step() {
  section("check");
  std::vector<GameFacts> facts = game_facts();
  std::vector<Check> checks = run_checks(draft_, facts);
  std::vector<std::string> blocking = blockers(draft_, facts);
  if (probing_) {
    spinner();
    ImGui::SameLine();
    ImGui::TextDisabled("Still reading the games' executables; more may appear.");
  }
  if (!blocking.empty()) {
    ImGui::TextUnformatted("Before anything can be built:");
    for (const std::string& b : blocking) badge_line(BadgeKind::Fail, b);
    ImGui::Spacing();
  }
  if (checks.empty()) {
    badge_line(BadgeKind::Ok, "Nothing to fix or acknowledge.");
    return;
  }
  ImGui::TextWrapped("Fix each of these, or acknowledge it: the bundle is built only when every one is either.");
  ImGui::Spacing();
  for (const Check& c : checks) {
    ImGui::PushID(c.id.c_str());
    bool acked = draft_.acked(c.id);
    // A box a line of text tall, on the line of the sentence it answers, so
    // every block is as tall as its words and they all space out alike.
    if (tick_box("##ack", &acked, true)) {
      if (acked) draft_.acknowledged.push_back(c.id);
      else draft_.acknowledged.erase(std::remove(draft_.acknowledged.begin(), draft_.acknowledged.end(), c.id),
                                     draft_.acknowledged.end());
      dirty_ = true;
    }
    ImGui::SameLine();
    begin_badge_block(acked ? BadgeKind::Ok : BadgeKind::Warn, acked ? "ACK" : nullptr);
    ImGui::TextWrapped("%s", c.text.c_str());
    if (!c.fix.empty()) colored_text(kDim, "To fix: " + c.fix);
    end_badge_block();
    vgap(4);
    ImGui::PopID();
  }
}

// ---- 5. size ----------------------------------------------------------------------------------------

void Bundles::size_step() {
  section("size");
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
  if (!base_trouble_.empty()) badge_line(BadgeKind::Warn, "The runtime's size is unknown: " + base_trouble_);

  // Each part's share of the whole as a bar, and the sizes to the right so
  // their digits line up. The size columns fit the widest figure they hold.
  const float cell = ImGui::CalcTextSize("0").x;
  const float num_w = std::max(ImGui::CalcTextSize("without its discs").x, cell * 15);
  // The last column says what a part would weigh without its discs. When no
  // part carries any, every cell of it would be empty: it is left out.
  bool discs = r.total_without_discs != r.total;
  for (const SizePart& p : r.games) discs = discs || p.without_discs != p.bytes;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(std::round(px(8)), std::round(px(5))));
  const bool table =
      ImGui::BeginTable("size", discs ? 4 : 3,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  if (table) {
    ImGui::TableSetupColumn("part", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("share", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("as it is", ImGuiTableColumnFlags_WidthFixed, cell * 10);
    if (discs) ImGui::TableSetupColumn("without its discs", ImGuiTableColumnFlags_WidthFixed, num_w);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("part");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextDisabled("share");
    ImGui::TableSetColumnIndex(2);
    right_aligned("as it is", kDim);
    if (discs) {
      ImGui::TableSetColumnIndex(3);
      right_aligned("without its discs", kDim);
    }
    const uint64_t whole = std::max<uint64_t>(r.total, 1);
    auto row = [&](const std::string& a, uint64_t b, uint64_t c) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      elided_text(a);
      ImGui::TableSetColumnIndex(1);
      // A share under one per cent reads "0%", and so the bar shows no
      // fill either, rather than a sliver that looks like a stray glyph.
      const float share = static_cast<float>(static_cast<double>(b) / static_cast<double>(whole));
      block_progress(share < 0.01f ? 0.0f : share);
      ImGui::TableSetColumnIndex(2);
      right_aligned(human_size(b));
      if (discs && b != c) {
        ImGui::TableSetColumnIndex(3);
        right_aligned("about " + human_size(c), kDim);
      }
    };
    row("runtime and player", r.runtime.bytes, r.runtime.bytes);
    row("pictures and extra files", r.meta, r.meta);
    for (const SizePart& p : r.games) row(p.label, p.bytes, p.without_discs);
    ImGui::TableNextRow(0, 0);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, u32(kBg2));
    ImGui::TableSetColumnIndex(0);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("total");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    elided_text("(" + std::to_string(r.total) + " bytes)", kDim);
    ImGui::TableSetColumnIndex(2);
    right_aligned(human_size(r.total), kAccent);
    if (discs && r.total_without_discs != r.total) {
      ImGui::TableSetColumnIndex(3);
      right_aligned("about " + human_size(r.total_without_discs), kDim);
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  if (r.warnings.empty()) badge_line(BadgeKind::Ok, "Under 2 GiB: any host takes it, and it fits on a FAT32 drive.");
  for (const std::string& w : r.warnings) badge_line(BadgeKind::Warn, w);
  ImGui::Spacing();
  colored_text(kDim,
               "The discs cannot be left out of a pack here: a game's pack goes into the bundle byte for "
               "byte. Leaving them out means installing the game again without them.");
}

// ---- 6. rights ------------------------------------------------------------------------------------

void Bundles::rights_step() {
  section("rights");
  ImGui::TextWrapped(
      "The bundle carries these games whole. kretro does not check whether you may give them to anybody; "
      "it records that you said you may, in the file itself.");
  ImGui::Spacing();
  if (tick_box("I have the right to distribute these games", &draft_.rights)) dirty_ = true;
  if (!draft_.rights) {
    ImGui::Spacing();
    badge_line(BadgeKind::Warn, "Build stays off until this is ticked.");
  }
}

}  // namespace kg::gui
