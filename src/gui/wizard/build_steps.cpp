#include "wizard.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../../config/config.h"
#include "../format.h"
#include "../palette.h"
#include "../widgets.h"

namespace kg::gui {
namespace fs = std::filesystem;

namespace {

// SafeDisc and SecuROM read raw sectors below the filesystem, and a
// directory-backed CD-ROM drive cannot answer that. Naming it on the build
// step is better than a pack that builds cleanly and a game that will not
// start, and better than refusing to build - the pack is still worth having.
std::vector<std::string> protection_notes(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    std::string what = install::protection_of(de.path().filename().string());
    if (what.empty()) continue;
    std::string line = std::string("this game carries ") + de.path().filename().string() +
                       ", which is " + what
                       + ". The pack will be built and the game may still refuse to start; "
                         "a directory-backed CD-ROM drive cannot answer a raw-sector read.";
    if (std::find(out.begin(), out.end(), line) == out.end()) out.push_back(line);
  }
  return out;
}

// One line of the size table: the key dim, the size flush right in a column of
// its own, and whatever goes with it after. Only a row with a field in it is
// `framed`, lowered to the text inside the field; the rest keep the body's
// line, so the table reads at the rhythm of the paragraph under it.
void size_row(const char* key, uint64_t bytes, const ImVec4& colour = kText, bool framed = false) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  if (framed) ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", key);
  ImGui::TableSetColumnIndex(1);
  if (framed) ImGui::AlignTextToFramePadding();
  const std::string s = human_size(bytes);
  const float w = ImGui::CalcTextSize(s.c_str()).x;
  const float room = ImGui::GetContentRegionAvail().x;
  if (w < room) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - w);
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextUnformatted(s.c_str());
  ImGui::PopStyleColor();
  ImGui::TableSetColumnIndex(2);
}

}  // namespace

void Wizard::build_page() {
  PageWindow page(step_title("build the pack"), "Esc back", fonts_.big());
  step_top();
  begin_body();
  trouble_banner();

  const install::Candidate& c = build_->candidates()[candidate_pick_];
  uint64_t disc_bytes = 0, audio_bytes = 0;
  std::error_code ec;
  for (const disc::Disc& d : build_->discs()) {
    disc_bytes += d.info.size;
    for (const disc::AudioTrack& t : d.audio) audio_bytes += fs::file_size(t.file, ec);
  }

  // What the installer wrote to C: that is not the game: the DLLs and the
  // shared runtime it scattered around Windows. They travel whatever else does,
  // because the registry fragment names them and the fragment travels; the row
  // is here so that is visible rather than a surprise in the size.
  wine::SystemFiles sys = build_->outside(c.dir);

  section("size");
  // Sizes in a column of their own, flush right, so they read down on their
  // units; the discs' checkbox sits on the discs' own row.
  // No outer padding, so the keys start on the section rule's left edge, as
  // the paragraph under the table does.
  const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit;
  uint64_t total = c.bytes + sys.bytes + (draft_.embed_discs ? disc_bytes + audio_bytes : 0);
  // The one checkbox framed close, so its row is hardly taller than the rest.
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(ImGui::GetStyle().FramePadding.x, std::round(px(2))));
  if (ImGui::BeginTable("sizes", 3, tf)) {
    ImGui::TableSetupColumn("what");
    ImGui::TableSetupColumn("size");
    ImGui::TableSetupColumn("note", ImGuiTableColumnFlags_WidthStretch);
    size_row("game tree", c.bytes);
    size_row("system files", sys.bytes);
    ImGui::TextDisabled("%zu outside the game folder", sys.files);
    size_row("discs", disc_bytes, draft_.embed_discs ? kText : kDim, true);
    ImGui::Checkbox("include the discs", &draft_.embed_discs);
    size_row("cd audio", audio_bytes, draft_.embed_discs ? kText : kDim);
    // A rule over the sum, drawn as a thin filled bar the width of the first
    // two columns, as the rest of the terminal draws its hairlines.
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const float x0 = ImGui::GetCursorScreenPos().x;
    ImGui::TableSetColumnIndex(1);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x1 = p.x + ImGui::GetContentRegionAvail().x;
    const float hair = std::max(1.0f, std::round(px(1)));
    ImGui::Dummy(ImVec2(0, hair));
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + hair), u32(kLine));
    size_row("before dedup", total, kAccent);
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  vgap(6);
  ImGui::PushTextWrapPos(prose_wrap());
  ImGui::TextWrapped(
      "The installed files are copies of files on the discs, and mkdwarfs stores a byte "
      "once, so the pack will be smaller than this - usually much smaller.");
  if (!draft_.embed_discs) {
    ImGui::TextDisabled(
        "Without them, the game's page will say \"needs the original disc\", the same way "
        "the library already says it for a disc it cannot find.");
  }
  ImGui::PopTextWrapPos();

  for (const std::string& note : protection_notes(build_->installed_root() / c.dir)) {
    vgap(6);
    badge_line(BadgeKind::Warn, note, kWarm);
  }
  end_body();

  if (step_footer("Build it")) {
    draft_.install_dir = c.dir;
    install::Draft d = draft_;
    run_job("Building " + draft_.name, [this, d] {
      result_ = build_->write(d);
      root_matched_ = !expect_root_set_ || result_.root == expect_root_;
    }, Step::Done);
  }

  // The sources came from anywhere on the disk, and a recipe's disc references
  // resolve only against the top level of iso_dir(). Adding each source's
  // folder to the library paths is the same mechanism as dropping a folder on
  // the shelf, and it is what makes these discs findable again.
  config::Config cfg = config::load(config::config_file());
  bool dirty = false;
  for (const fs::path& s : draft_.sources) {
    std::string parent = s.parent_path().string();
    if (std::find(cfg.library_paths.begin(), cfg.library_paths.end(), parent) ==
        cfg.library_paths.end()) {
      cfg.library_paths.push_back(parent);
      dirty = true;
    }
  }
  if (dirty) config::save(config::config_file(), cfg);
}
void Wizard::done_page() {
  PageWindow page(step_title(draft_.name + " is in your library"), "Esc back", fonts_.big());
  step_top();
  begin_body();

  // The result's numbers in full-strength text and the words between them
  // dim: the numbers are what the page is here to say.
  badge(BadgeKind::Ok);
  ImGui::SameLine(0, badge_gap());
  const std::string files = std::to_string(result_.entries);
  const std::string tree = human_size(result_.tree_bytes);
  const std::string packed = human_size(result_.pack_bytes);
  const std::pair<const char*, bool> parts[] = {{files.c_str(), true}, {" files, ", false}, {tree.c_str(), true}, {", packed to ", false}, {packed.c_str(), true}};
  for (size_t i = 0; i < std::size(parts); ++i) {
    if (i) ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, parts[i].second ? kText : kDim);
    ImGui::TextUnformatted(parts[i].first);
    ImGui::PopStyleColor();
  }
  vgap(4);
  section("pack");
  if (kv_begin("pack")) {
    kv("one file", result_.pack.filename().string(), kCyan);
    kv_end();
  }
  if (expect_root_set_) {
    // Advisory now: with no answer file, two people clicking through the same
    // InstallShield need not produce the same bytes, so a mismatch is a fact
    // to report rather than a reason to throw the install away.
    vgap(6);
    badge_line(root_matched_ ? BadgeKind::Ok : BadgeKind::Warn,
               root_matched_ ? "the tree matches the recipe" : "the tree differs from the recipe",
               root_matched_ ? kText : kWarm);
  }
  end_body();

  // Play in the footer, where every other step keeps its way forward, and the
  // two smaller ways out on the footer's left, where the others keep Back. The
  // page opens on Play.
  const bool play = step_footer("Play it", true, true, [this](float top) {
    const float small_h = ImGui::GetTextLineHeight() + std::round(px(2)) * 2;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x,
                                     top + std::round((flow_footer_height() - small_h) * 0.5f)));
    // There is no separate export step in the format - install.h's opening
    // comment says the pack is simultaneously the installed game and the
    // shareable capsule - so this is a copy, and the button says so.
    if (small_button("Copy the .kgpack to your home directory")) {
      std::error_code ec;
      const char* home = std::getenv("HOME");
      fs::path out = fs::path(home ? home : ".") / result_.pack.filename();
      fs::copy_file(result_.pack, out, fs::copy_options::overwrite_existing, ec);
      status_ = ec ? ("could not copy it: " + ec.message()) : ("copied to " + out.string());
    }
    ImGui::SameLine();
    if (small_button("Back to the shelf")) {
      status_ = draft_.name + " is in your library";
      finished_ = true;
    }
  });
  if (play) {
    status_ = "play:" + draft_.id;
    finished_ = true;
  }
}

}  // namespace kg::gui
