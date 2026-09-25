#include "wizard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include "../format.h"
#include "../palette.h"
#include "../widgets.h"
#include "words.h"

namespace kg::gui {
namespace fs = std::filesystem;

namespace {

// A header row of dim column names, the numeric ones set to the right.
void header_row(const char* const* names, int n, int right_from) {
  ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
  for (int c = 0; c < n; ++c) {
    ImGui::TableSetColumnIndex(c);
    if (c >= right_from) right_aligned(names[c], kDim);
    else ImGui::TextDisabled("%s", names[c]);
  }
}

// The width of a numeric column: the widest of its header and its values. A
// fitted column is measured from its rows alone, not its header, so a header
// wider than every value ran past the column's right edge and the values
// stopped short of it.
float numeric_width(const char* head, const std::vector<std::string>& values) {
  float w = ImGui::CalcTextSize(head).x;
  for (const std::string& v : values) w = std::max(w, ImGui::CalcTextSize(v.c_str()).x);
  return std::ceil(w);
}

// Rows framed closer than a form's, so a radio is the size of the text beside
// it rather than towering over it. Pushed round a table of picks.
void push_pick_rows() {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(ImGui::GetStyle().FramePadding.x, std::round(px(2))));
}

// A radio whose label is the row's name, cut to the room left, so the name
// picks the row as the circle does. The ID is fixed, whatever the label.
bool pick_radio(const std::string& name, bool on) {
  const float room = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() -
                     ImGui::GetStyle().ItemInnerSpacing.x;
  ImGui::PushStyleColor(ImGuiCol_Text, on ? kAccent : kText);
  const bool pressed = ImGui::RadioButton((elide(nullptr, name, room) + "###pick").c_str(), on);
  ImGui::PopStyleColor();
  return pressed;
}

}  // namespace

using wizard_detail::where_word;

void Wizard::where_page() {
  PageWindow page(step_title("where did it install?"), "Esc back", fonts_.big());
  step_top();

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
    begin_body();
    // What the install failed with, above the page that explains what an
    // install with nothing to show for it means. This is the step a failed
    // install lands on, and until it did, the page below was unreachable.
    trouble_banner();
    // An empty page, drawn as the others draw one, with the way back in the
    // footer where every step keeps its way forward.
    empty_state(off_disc ? "Nothing came off the disc. It was stopped, or what step 3 named is not "
                           "on this disc under that name."
                         : "The installer wrote nothing to C:. It did not run, or it was "
                           "cancelled, or what ran was a front end rather than the setup itself.",
                "\xe2\x96\xa1");  // U+25A1, an empty box
    end_body();
    if (step_footer(off_disc ? "Back to what to take off the disc" : "Back to the list of executables", true, true)) {
      step_ = Step::What;
    }
    return;
  }

  if (candidate_pick_ >= c.size()) candidate_pick_ = 0;
  begin_body();
  trouble_banner();
  section("candidates");
  // The directory, then its file count and size in columns of their own,
  // measured, where a printf padding ran them together at larger sizes.
  static const char* const heads[] = {"directory", "files", "size"};
  std::vector<std::string> counts, sizes;
  for (const install::Candidate& k : c) {
    counts.push_back(std::to_string(k.files));
    sizes.push_back(human_size(k.bytes));
  }
  const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
  push_pick_rows();
  if (ImGui::BeginTable("candidates", 3, tf)) {
    ImGui::TableSetupColumn("directory", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("files", ImGuiTableColumnFlags_WidthFixed, numeric_width(heads[1], counts));
    ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, numeric_width(heads[2], sizes));
    header_row(heads, 3, 1);
    for (size_t i = 0; i < c.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      if (pick_radio(where_word(c[i].dir), i == candidate_pick_)) candidate_pick_ = i;
      ImGui::TableSetColumnIndex(1);
      ImGui::AlignTextToFramePadding();
      right_aligned(counts[i], kText);
      ImGui::TableSetColumnIndex(2);
      ImGui::AlignTextToFramePadding();
      right_aligned(sizes[i], kText);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  vgap(6);
  // One candidate with no directory is a copy or an unzip: no installer ran,
  // nothing was written to C:, and there was no diff to rank. What came off
  // the disc is the game, whole, and there is nothing here to choose.
  ImGui::PushTextWrapPos(prose_wrap());
  if (c.size() == 1 && c[0].dir.empty()) {
    ImGui::TextDisabled(
        "Nothing ran an installer, so there is nothing to choose: this is what came off the "
        "disc, and the pack holds it as it is.");
  } else {
    ImGui::TextDisabled(
        "The first one is almost certainly it. Pick another if the game is somewhere else.");
    ImGui::TextDisabled(
        "This is the whole of what source.verify used to do, done by looking rather than by "
        "being told.");
  }
  ImGui::PopTextWrapPos();
  end_body();

  if (step_footer("Next: what runs it")) {
    draft_.install_dir = c[candidate_pick_].dir;
    // The same courtesy step 3 does for the setup. A preset or a recipe that
    // names the exe is answering the one question ranking cannot: a game's
    // expansions may all run the base game's exe, and a manifest may name
    // Gamew.exe because the larger GAME.EXE beside it is the DOS build.
    exe_pick_ = install::exe_index(c[candidate_pick_].executables, draft_.exe);
    if (exe_pick_ >= c[candidate_pick_].executables.size()) exe_pick_ = 0;
    step_ = Step::Runs;
  }
}
void Wizard::runs_page() {
  PageWindow page(step_title("what runs the game?"), "Esc back", fonts_.big());
  step_top();

  const install::Candidate& c = build_->candidates()[candidate_pick_];
  if (c.executables.empty()) {
    begin_body();
    empty_state(("Nothing in " + where_word(c.dir) + " looks like a program. Try another directory.")
                    .c_str(),
                "\xe2\x96\xa1");  // U+25A1, an empty box
    end_body();
    if (step_footer("Back to the directories", true, true)) step_ = Step::Where;
    return;
  }

  begin_body();
  section("programs");
  // Sizes read here once for the column's width and its cells. A file that
  // will not stat has no size to show, and file_size's error value printed as
  // one is a number sixteen million terabytes long.
  std::vector<std::string> sizes;
  std::vector<bool> sized;
  for (const fs::path& e : c.executables) {
    // installed_root(), not drive_c: a copy or unzip install never wrote to C:
    // and its tree is the staging tree, so this is where the file whose size
    // is about to be printed actually is.
    std::error_code ec;
    const uintmax_t n = fs::file_size(build_->installed_root() / c.dir / e, ec);
    sized.push_back(!ec);
    sizes.push_back(ec ? std::string("-") : human_size(n));
  }
  static const char* const heads[] = {"program", "size"};
  const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
  push_pick_rows();
  if (ImGui::BeginTable("programs", 2, tf)) {
    ImGui::TableSetupColumn("program", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, numeric_width(heads[1], sizes));
    header_row(heads, 2, 1);
    for (size_t i = 0; i < c.executables.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      if (pick_radio(c.executables[i].string(), i == exe_pick_)) exe_pick_ = i;
      ImGui::TableSetColumnIndex(1);
      ImGui::AlignTextToFramePadding();
      right_aligned(sizes[i], sized[i] ? kText : kDim);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  draft_.exe = c.executables[exe_pick_];

  vgap(8);
  section("arguments");
  ImGui::SetNextItemWidth(field_width(420));
  ImGui::InputText("##args", args_buf_, sizeof(args_buf_));
  draft_.args = args_buf_;
  // Its own line with its own hint, rather than tucked into an advanced
  // section, because this field is sometimes the entire difference between
  // two games.
  ImGui::PushTextWrapPos(prose_wrap());
  ImGui::TextDisabled(
      "Some games are the same program twice: expansions that run the base game's exe "
      "differ only here.");
  ImGui::PopTextWrapPos();
  end_body();

  if (step_footer("Next: how it looks")) {
    // Written into the pack now, while the tree is still on disk and still
    // exactly what the user just looked at. After the build there is nothing
    // left to look at: the staging tree is gone.
    draft_.verify = install::verify_list(
        build_->installed_root() / draft_.install_dir, draft_.exe);
    step_ = Step::Presentation;
  }
}
void Wizard::presentation_page() {
  PageWindow page(step_title("how it looks"), "Esc back", fonts_.big());
  step_top();
  begin_body();

  section("resolution");
  // Keys in a dim column before their fields, as the identity step shows its
  // own, rather than bright labels trailing after the step buttons.
  int w = static_cast<int>(draft_.width), h = static_cast<int>(draft_.height);
  if (form_begin("resolution", {"width", "height"})) {
    form_row("width");
    ImGui::SetNextItemWidth(field_width(160));
    if (ImGui::InputInt("##width", &w) && w > 0) draft_.width = static_cast<uint32_t>(w);
    form_row("height");
    ImGui::SetNextItemWidth(field_width(160));
    if (ImGui::InputInt("##height", &h) && h > 0) draft_.height = static_cast<uint32_t>(h);
    ImGui::EndTable();
  }
  ImGui::PushTextWrapPos(prose_wrap());
  ImGui::TextDisabled(
      "the resolution the game renders at, not the size of the window. How that reaches "
      "your panel is a setting, on the Settings screen, and it applies to every game.");
  ImGui::PopTextWrapPos();

  vgap(8);
  section("renderer");
  ImGui::Checkbox("dgVoodoo", &draft_.dgvoodoo);
  // The page opens here rather than on the width above it: a field the focus
  // opens on is one a stray key types into.
  default_focus();
  ImGui::PushTextWrapPos(prose_wrap());
  ImGui::TextDisabled("a Direct3D wrapper, for games whose own renderer no longer works");
  ImGui::PopTextWrapPos();
  end_body();

  if (step_footer("Next: build the pack")) step_ = Step::Build;
}

}  // namespace kg::gui
