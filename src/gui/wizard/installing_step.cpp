#include "wizard.h"

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../palette.h"
#include "../widgets.h"

namespace kg::gui {

void Wizard::installing_page() {
  PageWindow page(("installing " + draft_.name).c_str(), "Esc abandon   F11 fullscreen", big_);

  // The stage is opened from the UI thread the moment the worker publishes a
  // display, and opens its own XOpenDisplay: an Xlib connection is not shared
  // between threads here. Each side has one, which is the simplest arrangement
  // that is correct.
  if (!stage_ && stage_trouble_.empty()) {
    std::string disp;
    { std::lock_guard<std::mutex> lk(display_mutex_); disp = display_; }
    if (!disp.empty()) {
      try {
        stage_ = std::make_unique<Stage>(ren_, disp);
      } catch (const std::exception& ex) {
        stage_trouble_ = ex.what();
      }
    }
  }

  // ImGui's keyboard nav is enabled globally, and it would eat the arrows and
  // Tab that drive an InstallShield dialog before XTest ever saw them. It goes
  // back on in leave_installing().
  ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

  float w = ImGui::GetContentRegionAvail().x;
  float h = ImGui::GetContentRegionAvail().y - 200.0f;
  if (h < 120.0f) h = 120.0f;
  if (stage_) {
    // False means only "the last frame is the newest one I have". The X
    // display outlives setup.exe by construction, so this could never have
    // been the signal that the install finished.
    if (!stage_->refresh() && !stage_dead_) {
      stage_dead_ = true;
      job_.log("the installer's display has gone; showing the last frame");
    }
    stage_->draw(ImVec2(w, h));
  } else {
    ImGui::Dummy(ImVec2(w, h));
    ImGui::TextDisabled("%s", stage_trouble_.empty()
                                  ? "waiting for the installer's window..."
                                  : stage_trouble_.c_str());
  }

  ImGui::Spacing();
  // A copy, taken under the worker's own lock. This step is the one with no
  // modal over it, which is exactly why it is the one place the read races:
  // the page draws every frame while the thread that assigns the error is
  // still running.
  const std::string oops = job_.error();
  if (!oops.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", oops.c_str());
    ImGui::PopStyleColor();
  }
  // A copy, taken once a frame, and never a reference into the Build.
  //
  // This page starts drawing on the frame the worker starts, and the worker
  // spends its first minutes inside mount_discs clearing drives_ and
  // push_backing into it. A `const std::vector&` held here across that
  // reallocation is a dangling reference in the middle of a frame - the
  // install crashing the shelf just as the first disc came up. `ready` is the
  // gate: until the mounts are done there is nothing to draw but a sentence.
  const install::Build::Mounted m =
      build_ ? build_->mounted() : install::Build::Mounted{};
  if (!m.ready) {
    ImGui::TextDisabled("%s", m.discs.empty() ? "reading the discs..."
                                              : "putting the discs in their drives...");
  } else {
    if (drive_pick_.size() != m.drives.size()) {
      drive_pick_.resize(m.drives.size());
      for (size_t i = 0; i < drive_pick_.size(); ++i) drive_pick_[i] = i;
    }
    for (size_t i = 0; i < m.drives.size(); ++i) {
      if (i) ImGui::SameLine(0, 24);
      ImGui::PushID(static_cast<int>(i));
      char letter = static_cast<char>('D' + i);
      const size_t in_it = drive_pick_[i] < m.discs.size() ? drive_pick_[i] : i;
      std::string preview = std::string(1, letter) + ":  " +
                            (in_it < m.discs.size() ? m.discs[in_it] : m.drives[i]);
      ImGui::SetNextItemWidth(240);
      if (ImGui::BeginCombo("##drive", preview.c_str())) {
        for (size_t d = 0; d < m.discs.size(); ++d) {
          if (ImGui::Selectable(m.discs[d].c_str(), d == in_it)) {
            // One symlink, the same change cmd_swap_game makes: Wine resolves
            // dosdevices on every open, so the disc changes with nothing to
            // restart and nothing moved out from under an open file. Some
            // installers do insist on one drive, which is the only reason this
            // menu exists.
            //
            // And it throws - a disc that is not mounted is said so rather
            // than silently ignored. Thrown from here it would unwind through
            // an open BeginCombo, an open Begin and out of gui::run, killing
            // the window and the running installer with it. Nothing that can
            // throw may leave a draw call.
            try {
              build_->swap_disc(static_cast<char>('d' + i), static_cast<int>(d));
              drive_pick_[i] = d;
              drive_trouble_.clear();
            } catch (const std::exception& ex) {
              drive_trouble_ = ex.what();
            }
          }
        }
        ImGui::EndCombo();
      }
      ImGui::PopID();
    }
    ImGui::TextDisabled("%s", m.drives.empty()
        ? "no disc: this installer carries its own game and runs from where it is"
        : m.drives.size() > 1
            ? "all the discs are in drives; it should not ask you to swap"
            : "the disc is in D:");
  }
  if (!drive_trouble_.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", drive_trouble_.c_str());
    ImGui::PopStyleColor();
  }

  if (!draft_.serial.empty()) {
    ImGui::Spacing();
    ImGui::Text("serial   %s", draft_.serial.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("copy")) SDL_SetClipboardText(draft_.serial.c_str());
    ImGui::TextDisabled("shown so you can read it while you type it. Nothing types it for you.");
  }

  ImGui::Spacing();
  // An atomic read of a number a thread of Build's own publishes once a
  // second. The walk itself, on this thread, in the middle of this frame,
  // would be a stall you can see on a large install, once a second, for the
  // whole of it.
  if (build_) files_ = build_->files_written_so_far();
  ImGui::Text("%zu files written to C:", files_);
  if (std::optional<std::string> last = job_.last_line()) ImGui::TextDisabled("%s", last->c_str());
  ImGui::TextDisabled(
      "Escape asks whether to abandon this install. It does not reach the installer - which "
      "matters, because Escape is also how you back out of an InstallShield dialog.");
}

}  // namespace kg::gui
