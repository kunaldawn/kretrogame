#include "wizard.h"

#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

#include "../format.h"
#include "../widgets.h"
#include "words.h"

namespace kg::gui {
namespace fs = std::filesystem;

using wizard_detail::where_word;

void Wizard::where_page() {
  PageWindow page("where did it install?", "Esc back", big_);
  // What the install failed with, above the page that explains what an install
  // with nothing to show for it means. This is the step a failed install lands
  // on, and until it did, the page below was unreachable.
  trouble_banner();

  // A Build there always is by the time anything reaches this step - it is
  // what ran the install - but this page is also the one a failure arrives at,
  // and a page that dereferences its way out of the program is a worse answer
  // than an empty list.
  const std::vector<install::Candidate> nothing;
  const std::vector<install::Candidate>& c = build_ ? build_->candidates() : nothing;
  if (c.empty()) {
    // The old engine threw here, when no directory held the game. A page is
    // better: the usual cause is having launched a DemoShield front end rather
    // than the setup, and that is fixable one step back.
    bool off_disc = draft_.method == install::Draft::Method::Copy ||
                    draft_.method == install::Draft::Method::Unzip;
    ImGui::TextWrapped(
        "%s", off_disc
                  ? "Nothing came off the disc. It was stopped, or what step 3 named is not "
                    "on this disc under that name."
                  : "The installer wrote nothing to C:. It did not run, or it was cancelled, "
                    "or what ran was a front end rather than the setup itself.");
    ImGui::Spacing();
    if (ImGui::Button(off_disc ? "Back to what to take off the disc"
                               : "Back to the list of executables")) {
      step_ = Step::What;
    }
    return;
  }

  if (candidate_pick_ >= c.size()) candidate_pick_ = 0;
  for (size_t i = 0; i < c.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    char row[512];
    std::snprintf(row, sizeof(row), "%-44s %8zu files   %s", where_word(c[i].dir).c_str(),
                  c[i].files, human_size(c[i].bytes).c_str());
    if (ImGui::RadioButton(row, i == candidate_pick_)) candidate_pick_ = i;
    ImGui::PopID();
  }
  ImGui::Spacing();
  // One candidate with no directory is a copy or an unzip: no installer ran,
  // nothing was written to C:, and there was no diff to rank. What came off
  // the disc is the game, whole, and there is nothing here to choose.
  if (c.size() == 1 && c[0].dir.empty()) {
    ImGui::TextWrapped(
        "Nothing ran an installer, so there is nothing to choose: this is what came off the "
        "disc, and the pack holds it as it is.");
  } else {
    ImGui::TextWrapped(
        "The first one is almost certainly it. Pick another if the game is somewhere else.");
    ImGui::TextDisabled(
        "This is the whole of what source.verify used to do, done by looking rather than by "
        "being told.");
  }

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (ImGui::Button("Next: what runs it", ImVec2(-1, 60))) {
    draft_.install_dir = c[candidate_pick_].dir;
    // The same courtesy step 3 does for the setup. A preset or a recipe that
    // names the exe is answering the one question ranking cannot: a game's
    // expansions may all run the base game's exe, and a manifest may name
    // Gamew.exe because the larger GAME.EXE beside it is the DOS build.
    exe_pick_ = install::exe_index(c[candidate_pick_].executables, draft_.exe);
    if (exe_pick_ >= c[candidate_pick_].executables.size()) exe_pick_ = 0;
    step_ = Step::Runs;
  }
  ImGui::PopFont();
}
void Wizard::runs_page() {
  PageWindow page("what runs the game?", "Esc back", big_);

  const install::Candidate& c = build_->candidates()[candidate_pick_];
  if (c.executables.empty()) {
    ImGui::TextWrapped("Nothing in %s looks like a program. Try another directory.",
                       where_word(c.dir).c_str());
    ImGui::Spacing();
    if (ImGui::Button("Back to the directories")) step_ = Step::Where;
    return;
  }

  std::error_code ec;
  for (size_t i = 0; i < c.executables.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    // installed_root(), not drive_c: a copy or unzip install never wrote to C:
    // and its tree is the staging tree, so this is where the file whose size
    // is about to be printed actually is.
    fs::path full = build_->installed_root() / c.dir / c.executables[i];
    char row[512];
    std::snprintf(row, sizeof(row), "%-40s %10s", c.executables[i].c_str(),
                  human_size(fs::file_size(full, ec)).c_str());
    if (ImGui::RadioButton(row, i == exe_pick_)) exe_pick_ = i;
    ImGui::PopID();
  }
  draft_.exe = c.executables[exe_pick_];

  ImGui::Spacing();
  ImGui::TextDisabled("arguments");
  ImGui::SetNextItemWidth(420);
  ImGui::InputText("##args", args_buf_, sizeof(args_buf_));
  draft_.args = args_buf_;
  // Its own line with its own hint, rather than tucked into an advanced
  // section, because this field is sometimes the entire difference between
  // two games.
  ImGui::TextWrapped(
      "Some games are the same program twice: expansions that run the base game's exe "
      "differ only here.");

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (ImGui::Button("Next: how it looks", ImVec2(-1, 60))) {
    // Written into the pack now, while the tree is still on disk and still
    // exactly what the user just looked at. After the build there is nothing
    // left to look at: the staging tree is gone.
    draft_.verify = install::verify_list(
        build_->installed_root() / draft_.install_dir, draft_.exe);
    step_ = Step::Presentation;
  }
  ImGui::PopFont();
}
void Wizard::presentation_page() {
  PageWindow page("how it looks", "Esc back", big_);

  int w = static_cast<int>(draft_.width), h = static_cast<int>(draft_.height);
  ImGui::SetNextItemWidth(140);
  if (ImGui::InputInt("width", &w) && w > 0) draft_.width = static_cast<uint32_t>(w);
  ImGui::SetNextItemWidth(140);
  if (ImGui::InputInt("height", &h) && h > 0) draft_.height = static_cast<uint32_t>(h);
  ImGui::TextDisabled(
      "the resolution the game renders at, not the size of the window. How that reaches "
      "your panel is a setting, on the Settings screen, and it applies to every game.");

  ImGui::Spacing();
  ImGui::Checkbox("dgVoodoo", &draft_.dgvoodoo);
  ImGui::TextDisabled("a Direct3D wrapper, for games whose own renderer no longer works");

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (ImGui::Button("Next: build the pack", ImVec2(-1, 60))) step_ = Step::Build;
  ImGui::PopFont();
}

}  // namespace kg::gui
