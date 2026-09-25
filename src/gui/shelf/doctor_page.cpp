// Doctor: the display, the graphics devices and whether they can be opened,
// the NVIDIA driver, the runtime, and what is wrong with any of it. The probe
// runs once, on the page's first frame.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../../gpu/probe.h"
#include "../palette.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {

namespace {

// `text` in `colour`, wrapped to the room left on the line at spaces only.
// ImGui also breaks after a full stop, which cuts a library's version number
// in two ("libfoo.so.595." / "84"); a word wider than the line is left to run
// on rather than split.
void wrap_at_spaces(const std::string& text, const ImVec4& colour) {
  const float wrap = ImGui::GetContentRegionAvail().x;
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  // Lines of one paragraph, a little apart but closer than separate rows.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, std::round(px(2))));
  std::string line;
  size_t i = 0;
  while (i <= text.size()) {
    size_t end = text.find(' ', i);
    if (end == std::string::npos) end = text.size();
    const std::string word = text.substr(i, end - i);
    std::string longer = line;
    if (!longer.empty()) longer += ' ';
    longer += word;
    if (!line.empty() && ImGui::CalcTextSize(longer.c_str()).x > wrap) {
      ImGui::TextUnformatted(line.c_str());
      line = word;
    } else {
      line = longer;
    }
    i = end + 1;
  }
  if (!line.empty()) ImGui::TextUnformatted(line.c_str());
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

}  // namespace

void DoctorPage::draw() {
  set_page_trail("kretro \xe2\x80\xba doctor");
  PageWindow page("Doctor", "Esc back", ctx_.fonts.big());
  if (!probed_) { report_ = gpu::probe(); gpu::materialize(report_); probed_ = true; }

  // A boot log: every check a row of its own, its verdict in a column of
  // badges down the left, and each section's verdict at the right end of its
  // heading, so what is wrong is found by reading one column. Nothing in it
  // to press, so not flattened: the report is one stop, which the page opens
  // on, and the keys scroll it (scroll_with_keys). It runs the page's width,
  // its scrollbar at the edge, and reads in a column down the middle.
  // Room inside its edges, so the glow round it while it has the focus
  // never sits on a badge or a heading's rule on a narrow page.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, px(12, 10));
  ImGui::BeginChild("checks", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar();
  scroll_with_keys();
  centre_column(content_max_w(Content::Reading));
  step_heading("Doctor");

  const bool shown = report_.wayland || report_.x11;
  section("display", shown ? BadgeKind::Ok : BadgeKind::Fail);
  if (report_.wayland) {
    badge(BadgeKind::Ok);
    ImGui::SameLine(0, badge_gap());
    ImGui::TextDisabled("wayland");
    ImGui::SameLine();
    ImGui::TextColored(kCyan, "%s", report_.wayland_display.c_str());
  }
  if (report_.x11) {
    badge(BadgeKind::Ok);
    ImGui::SameLine(0, badge_gap());
    ImGui::TextDisabled("x11    ");
    ImGui::SameLine();
    ImGui::TextColored(kCyan, "%s", report_.x_display.c_str());
  }
  if (!shown) {
    // A failure's detail in the text colour, not dim: it is the line on the
    // page that most wants reading.
    badge(BadgeKind::Fail);
    ImGui::SameLine(0, badge_gap());
    ImGui::TextUnformatted("none");
  }

  // The devices that can be opened, of those there are.
  size_t readable = 0;
  for (const gpu::Device& d : report_.devices) readable += d.readable ? 1 : 0;
  const std::string opened = std::to_string(readable) + "/" + std::to_string(report_.devices.size());
  section("graphics", readable == report_.devices.size() && readable > 0 ? BadgeKind::Ok : BadgeKind::Fail,
          opened.c_str());
  // One device to a line, as close together as the other sections' lines,
  // with the node, the vendor and the verdict each in a column of its own.
  float node_w = 0, vendor_w = 0;
  for (const gpu::Device& d : report_.devices) {
    node_w = std::max(node_w, ImGui::CalcTextSize(d.node.c_str()).x);
    vendor_w = std::max(vendor_w, ImGui::CalcTextSize(gpu::vendor_name(d.vendor)).x);
  }
  const float col_gap = std::round(px(12));
  for (const gpu::Device& d : report_.devices) {
    badge(d.readable ? BadgeKind::Ok : BadgeKind::Fail);
    ImGui::SameLine(0, badge_gap());
    const float x = ImGui::GetCursorPosX();
    ImGui::TextColored(kCyan, "%s", d.node.c_str());
    ImGui::SameLine(x + node_w + col_gap);
    ImGui::TextUnformatted(gpu::vendor_name(d.vendor));
    ImGui::SameLine(x + node_w + vendor_w + col_gap * 2);
    ImGui::TextColored(d.readable ? kDim : kError, "%s", d.readable ? "accessible" : "NOT ACCESSIBLE");
  }

  if (report_.nvidia_present()) {
    section("nvidia", BadgeKind::Info);
    badge(BadgeKind::Info);
    ImGui::SameLine(0, badge_gap());
    ImGui::TextDisabled("driver %s, %zu libraries linked", report_.nvidia_version().c_str(),
                        report_.nvidia_libs().size());
  }

  section("runtime", BadgeKind::Info);
  badge(BadgeKind::Info);
  ImGui::SameLine(0, badge_gap());
  ImGui::TextDisabled("%s", elide(nullptr, ctx_.env.root.string(), ImGui::GetContentRegionAvail().x).c_str());

  const std::vector<std::string> problems = report_.problem_lines();
  const std::string count = std::to_string(problems.size());
  if (problems.empty()) section("problems", BadgeKind::Ok);
  else section("problems", BadgeKind::Warn, count.c_str());
  if (problems.empty()) {
    badge_line(BadgeKind::Ok, "no problems found", kHealthy);
  } else {
    // The badge says it is a warning; the sentence is in the text colour, so
    // a warning does not shout louder than a failure's red.
    for (const std::string& p : problems) {
      begin_badge_block(BadgeKind::Warn);
      wrap_at_spaces(p, kText);
      end_badge_block();
    }
  }
  end_centre_column();
  scroll_with_keys_end();
  ImGui::EndChild();
  default_focus();
}

}  // namespace kg::gui::shelf
