#include "job_modal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "palette.h"
#include "scale.h"
#include "widgets.h"

namespace kg::gui {

namespace {

// How far along the job says it is, when its newest line carries a
// percentage ("mkdwarfs: 61% (412 MiB of 672 MiB)"): the first number from 0
// to 100 followed by a percent sign. A job that says nothing of the kind gets
// the bar that only shows it is working.
float progress_of(const std::optional<std::string>& line) {
  if (!line) return -1.0f;
  const std::string& s = *line;
  for (size_t p = s.find('%'); p != std::string::npos; p = s.find('%', p + 1)) {
    size_t b = p;
    while (b > 0 && std::isdigit(static_cast<unsigned char>(s[b - 1]))) --b;
    if (b == p || p - b > 3) continue;
    const int n = std::stoi(s.substr(b, p - b));
    if (n <= 100) return static_cast<float>(n) / 100.0f;
  }
  return -1.0f;
}

}  // namespace

JobModalAction draw_job_modal(Job& job, ImFont* big, const JobModalHooks& hooks) {
  JobModalAction action = JobModalAction::None;
  ImGui::OpenPopup("busy");
  // A share of the window, so a log is readable on a television and still
  // fits a small window; never narrower than a line of it needs, and never
  // wider than reads comfortably. The height is the most it may take: the
  // modal is as tall as its log needs up to that, so a short log does not sit
  // in a tall empty box.
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float room = std::round(px(24));
  const float max_w = std::max(1.0f, vp->WorkSize.x - room * 2);
  const float max_h = std::max(1.0f, vp->WorkSize.y - status_bar_height() - room * 2);
  const float w = std::min(std::clamp(vp->WorkSize.x * 0.62f, px(520), px(1000)), max_w);
  const float h = std::min(std::clamp(vp->WorkSize.y * 0.62f, px(320), px(760)), max_h);
  // Headed by the job's own title, drawn here rather than by the dialog, as
  // its colour says how the job ended.
  if (dialog_begin_px("busy", w, h)) {
    // The footer's height as the last frame drew it, in design pixels so a
    // change of scale does not leave it a frame out of step. A hook's height
    // is known only once it has drawn, so this frame makes room for what the
    // last one measured; a modal just opened starts afresh rather than from
    // whatever job came before it.
    static float footer_seen = 0;
    if (ImGui::IsWindowAppearing()) footer_seen = 0;
    const bool done = job.finished();
    const std::string error = done ? job.error() : std::string();
    ImGui::PushFont(big);
    // The title stays what the job is called; its colour says how it ended.
    ImGui::PushStyleColor(ImGuiCol_Text, error.empty() ? kAccent : kError);
    ImGui::TextUnformatted(elide(big, job.title(), ImGui::GetContentRegionAvail().x).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    title_rule();
    ImGui::Dummy(ImVec2(0, std::round(px(2))));

    // Where the job is: a spinner and the bar while it runs, and then how it
    // ended. The error is the line the reader came for, so it wraps rather
    // than being cut short.
    if (!done) {
      spinner();
      ImGui::SameLine(0, std::round(px(10)));
      block_progress(progress_of(job.last_line()), 0, job.cancelled() ? "stopping" : nullptr);
    } else if (error.empty()) {
      badge(BadgeKind::Ok);
      ImGui::SameLine(0, badge_gap());
      ImGui::TextDisabled("finished");
    } else {
      badge_line(BadgeKind::Fail, error, kError);
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    // The band at the foot: what it holds, the room above and below that,
    // and the gap before it.
    const float footer = std::max(std::round(px(40)), std::round(px(footer_seen))) + st.WindowPadding.y * 2 +
                         std::round(px(8)) + st.ItemSpacing.y;
    // Log lines closer together than a page's paragraphs.
    const float line_gap = std::round(px(2));
    const ImVec2 pad = px(12, 10);
    const std::vector<std::string> lines = job.lines();

    // The log is as tall as its lines, wrapped at the panel's width, but at
    // least a few lines tall, and no taller than the modal's height leaves.
    const float log_w = ImGui::GetContentRegionAvail().x;
    const float wrap = std::max(1.0f, log_w - pad.x * 2 - st.ScrollbarSize);
    float content = pad.y * 2;
    for (const std::string& l : lines) {
      content += ImGui::CalcTextSize(l.c_str(), nullptr, false, wrap).y + line_gap;
    }
    const float least = pad.y * 2 + (ImGui::GetTextLineHeight() + line_gap) * 5.0f;
    const float most = h - ImGui::GetCursorPosY() - st.ItemSpacing.y - footer;
    const float log_h = std::round(std::max(1.0f, std::min(std::max(content, least), most)));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, line_gap));
    begin_text_panel("log", ImVec2(log_w, log_h), pad);
    // Older lines dim and the newest in full, as a terminal's scrollback reads:
    // the eye goes to what is happening now.
    for (size_t i = 0; i < lines.size(); ++i) {
      const bool newest = i + 1 == lines.size();
      const bool failed = lines[i].rfind("failed:", 0) == 0;
      ImGui::PushStyleColor(ImGuiCol_Text, failed ? kError : newest && !done ? kText
                                                                             : kDim);
      ImGui::TextWrapped("%s", lines[i].c_str());
      ImGui::PopStyleColor();
    }
    // Follow the newest line, unless the reader has scrolled up to look back.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - px(40)) ImGui::SetScrollHereY(1.0f);
    // Popped before the panel ends, so the footer is an ordinary gap below it.
    ImGui::PopStyleVar();
    end_panel();

    dialog_footer();
    const float footer_top = ImGui::GetCursorPosY();
    if (done) {
      if (hooks.on_finished_frame) hooks.on_finished_frame();
      // Escape and the pad's B close a finished job's modal too (the safe
      // answer); a running one's they leave alone, as they always have.
      if (dialog_button("Close", DialogButton::Primary, true)) {
        if (hooks.on_close) hooks.on_close();
        ImGui::CloseCurrentPopup();
        action = JobModalAction::Close;
      }
    } else if (hooks.footer_while_running) {
      hooks.footer_while_running();
    }
    footer_seen = std::max(0.0f, ImGui::GetCursorPosY() - footer_top - st.ItemSpacing.y) / ui_scale();
    dialog_end();
  }
  return action;
}

}  // namespace kg::gui
