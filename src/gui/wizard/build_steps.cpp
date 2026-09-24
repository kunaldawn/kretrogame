#include "wizard.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <system_error>
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

}  // namespace

void Wizard::build_page() {
  PageWindow page("build the pack", "Esc back", big_);
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

  ImGui::Text("  game tree     %14s", human_size(c.bytes).c_str());
  ImGui::Text("  system files  %14s", human_size(sys.bytes).c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("%zu outside the game folder", sys.files);
  ImGui::Text("  discs         %14s", human_size(disc_bytes).c_str());
  ImGui::SameLine();
  ImGui::Checkbox("include the discs", &draft_.embed_discs);
  ImGui::Text("  cd audio      %14s", human_size(audio_bytes).c_str());
  ImGui::Separator();
  uint64_t total = c.bytes + sys.bytes + (draft_.embed_discs ? disc_bytes + audio_bytes : 0);
  ImGui::Text("  before dedup  %14s", human_size(total).c_str());
  ImGui::TextWrapped(
      "The installed files are copies of files on the discs, and mkdwarfs stores a byte "
      "once, so the pack will be smaller than this - usually much smaller.");
  if (!draft_.embed_discs) {
    ImGui::TextDisabled(
        "Without them, the game's page will say \"needs the original disc\", the same way "
        "the library already says it for a disc it cannot find.");
  }

  for (const std::string& note : protection_notes(build_->installed_root() / c.dir)) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", note.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (ImGui::Button("Build it", ImVec2(-1, 60))) {
    draft_.install_dir = c.dir;
    install::Draft d = draft_;
    run_job("Building " + draft_.name, [this, d] {
      result_ = build_->write(d);
      root_matched_ = !expect_root_set_ || result_.root == expect_root_;
    }, Step::Done);
  }
  ImGui::PopFont();

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
  PageWindow page((draft_.name + " is in your library").c_str(), "Esc back", big_);

  ImGui::TextDisabled("%zu files, %s, packed to %s", result_.entries,
                      human_size(result_.tree_bytes).c_str(),
                      human_size(result_.pack_bytes).c_str());
  ImGui::TextDisabled("one file: %s", result_.pack.filename().c_str());
  if (expect_root_set_) {
    // Advisory now: with no answer file, two people clicking through the same
    // InstallShield need not produce the same bytes, so a mismatch is a fact
    // to report rather than a reason to throw the install away.
    ImGui::Spacing();
    ImGui::TextWrapped("%s", root_matched_ ? "the tree matches the recipe"
                                           : "the tree differs from the recipe");
  }

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (ImGui::Button("Play it", ImVec2(-1, 60))) {
    status_ = "play:" + draft_.id;
    finished_ = true;
  }
  ImGui::PopFont();
  ImGui::Spacing();
  // There is no separate export step in the format - install.h's opening
  // comment says the pack is simultaneously the installed game and the
  // shareable capsule - so this is a copy, and the button says so.
  if (ImGui::Button("Copy the .kgpack to your home directory")) {
    std::error_code ec;
    const char* home = std::getenv("HOME");
    fs::path out = fs::path(home ? home : ".") / result_.pack.filename();
    fs::copy_file(result_.pack, out, fs::copy_options::overwrite_existing, ec);
    status_ = ec ? ("could not copy it: " + ec.message()) : ("copied to " + out.string());
  }
  ImGui::SameLine();
  if (ImGui::Button("Back to the shelf")) {
    status_ = draft_.name + " is in your library";
    finished_ = true;
  }
}

}  // namespace kg::gui
