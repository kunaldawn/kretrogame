#include "wizard.h"

#include <algorithm>
#include <cmath>
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
  PageWindow page(("installing " + draft_.name).c_str(), "Esc abandon   F11 fullscreen", fonts_.big());
  step_top();

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
  // Tab that drive an InstallShield dialog before XTest ever saw them. It is
  // off only while nothing is up over the stage: the Abandon? question takes
  // its answer from the keyboard like any other. It goes back on in
  // leave_installing(), and on every other step (Wizard::draw).
  if (confirm_abandon_ || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  } else {
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
  }

  // The stage takes whatever the strip under it leaves, the strip measured on
  // the frame before; the first frame's guess is about the strip's usual size.
  const float line = ImGui::GetTextLineHeightWithSpacing();
  if (strip_h_ <= 0) strip_h_ = (line * 8 + ImGui::GetFrameHeightWithSpacing()) / ui_scale();
  float w = ImGui::GetContentRegionAvail().x;
  float h = ImGui::GetContentRegionAvail().y - px(strip_h_) - ImGui::GetStyle().ItemSpacing.y;
  if (h < px(120)) h = px(120);
  if (stage_) {
    // False means only "the last frame is the newest one I have". The X
    // display outlives setup.exe by construction, so this could never have
    // been the signal that the install finished.
    if (!stage_->refresh() && !stage_dead_) {
      stage_dead_ = true;
      job_.log("the installer's display has gone; showing the last frame");
    }
    stage_->draw(ImVec2(w, h));
    // The page opens on the stage, so a keyboard alone can type into the
    // installer without the mouse over it first.
    default_focus();
  } else {
    // An empty screen with a prompt waiting on it, framed like the stage it
    // stands in for.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float hair = std::max(1.0f, std::round(px(1)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), u32(kBg1));
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + hair), u32(kLine));
    dl->AddRectFilled(ImVec2(at.x, at.y + h - hair), ImVec2(at.x + w, at.y + h), u32(kLine));
    dl->AddRectFilled(at, ImVec2(at.x + hair, at.y + h), u32(kLine));
    dl->AddRectFilled(ImVec2(at.x + w - hair, at.y), ImVec2(at.x + w, at.y + h), u32(kLine));
    if (stage_trouble_.empty()) {
      // Not while a popup is up: the prompt sits where a centred modal's head
      // does, and poked out above it.
      if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        ImGui::Dummy(ImVec2(w, h));
      } else {
        empty_state("waiting for the installer's window...", nullptr, h);
      }
    } else {
      // empty_state draws its message dim, and this one is a failure, so it
      // is said in the warning colour in the middle of the frame instead.
      ImGui::Dummy(ImVec2(w, h));
      const float pad = px(16);
      const std::string msg = elide(nullptr, stage_trouble_, w - pad * 2);
      const ImVec2 sz = ImGui::CalcTextSize(msg.c_str());
      dl->AddText(ImVec2(std::round(at.x + (w - sz.x) * 0.5f), std::round(at.y + (h - sz.y) * 0.45f)),
                  u32(kWarn), msg.c_str());
    }
  }

  const float strip_top = ImGui::GetCursorPosY();
  vgap(2);
  // Working, and what the worker said last, as one status line.
  if (job_.running() && !job_.finished()) {
    spinner();
    ImGui::SameLine(0, px(10));
    std::optional<std::string> last = job_.last_line();
    block_progress(-1.0f, 0, last ? last->c_str() : nullptr);
  } else if (std::optional<std::string> last = job_.last_line()) {
    // A chip where the spinner was, so the line reads as the status it is
    // rather than a stray sentence over the drives.
    badge(BadgeKind::Info);
    ImGui::SameLine(0, badge_gap());
    elided_text(*last, kDim);
  }
  // A copy, taken under the worker's own lock. This step is the one with no
  // modal over it, which is exactly why it is the one place the read races:
  // the page draws every frame while the thread that assigns the error is
  // still running.
  const std::string oops = job_.error();
  if (!oops.empty()) {
    badge_line(BadgeKind::Fail, oops, kWarn);
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
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("drives");
    ImGui::SameLine(0, px(12));
    ImGui::TextDisabled("%s", m.discs.empty() ? "reading the discs..."
                                              : "putting the discs in their drives...");
  } else {
    if (drive_pick_.size() != m.drives.size()) {
      drive_pick_.resize(m.drives.size());
      for (size_t i = 0; i < drive_pick_.size(); ++i) drive_pick_[i] = i;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("drives");
    // As many combos to a line as fit, rather than a row that runs off the
    // right edge on a small window with several discs.
    const float combo_w = field_width(240);
    const float gap = px(24);
    float x_end = ImGui::GetItemRectMax().x;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (size_t i = 0; i < m.drives.size(); ++i) {
      if (x_end + gap + combo_w <= right) ImGui::SameLine(0, i ? gap : px(12));
      x_end = ImGui::GetCursorScreenPos().x + combo_w;
      ImGui::PushID(static_cast<int>(i));
      char letter = static_cast<char>('D' + i);
      const size_t in_it = drive_pick_[i] < m.discs.size() ? drive_pick_[i] : i;
      std::string preview = std::string(1, letter) + ": " +
                            (in_it < m.discs.size() ? m.discs[in_it] : m.drives[i]);
      ImGui::SetNextItemWidth(combo_w);
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
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", m.drives.empty()
        ? "no disc: this installer carries its own game and runs from where it is"
        : m.drives.size() > 1
            ? "all the discs are in drives; it should not ask you to swap"
            : "the disc is in D:");
    ImGui::PopTextWrapPos();
  }
  if (!drive_trouble_.empty()) {
    badge_line(BadgeKind::Warn, drive_trouble_, kWarm);
  }

  if (!draft_.serial.empty()) {
    vgap(2);
    // Lowered to the text inside the copy button beside it, so the two sit
    // on one baseline.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("serial");
    ImGui::SameLine(0, px(12));
    ImGui::PushStyleColor(ImGuiCol_Text, kCyan);
    ImGui::TextUnformatted(draft_.serial.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(0, px(12));
    if (small_button("copy")) SDL_SetClipboardText(draft_.serial.c_str());
    ImGui::PushTextWrapPos(prose_wrap());
    ImGui::TextDisabled("shown so you can read it while you type it. Nothing types it for you.");
    ImGui::PopTextWrapPos();
  }

  vgap(2);
  // An atomic read of a number a thread of Build's own publishes once a
  // second. The walk itself, on this thread, in the middle of this frame,
  // would be a stall you can see on a large install, once a second, for the
  // whole of it.
  if (build_) files_ = build_->files_written_so_far();
  // The count in full-strength text and the words after it dim, as the done
  // page gives its numbers; a lone accent digit read as a stray glyph.
  ImGui::Text("%zu", files_);
  ImGui::SameLine(0, ImGui::CalcTextSize(" ").x);
  ImGui::TextDisabled("files written to C:");
  ImGui::PushTextWrapPos(prose_wrap());
  ImGui::TextDisabled(
      "Escape asks whether to abandon this install. It does not reach the installer - which "
      "matters, because Escape is also how you back out of an InstallShield dialog.");
  ImGui::PopTextWrapPos();
  strip_h_ = (ImGui::GetCursorPosY() - strip_top) / ui_scale();
}

}  // namespace kg::gui
