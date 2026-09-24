#include <algorithm>
#include <exception>
#include <string>
#include <vector>

#include "../format.h"
#include "../widgets.h"
#include "bundles_page.h"

namespace kg::gui {
using namespace kg::bundle;

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

}  // namespace kg::gui
