// Doctor: the display, the graphics devices and whether they can be opened,
// the NVIDIA driver, the runtime, and what is wrong with any of it. The probe
// runs once, on the page's first frame.
#include <string>
#include <vector>

#include "../../gpu/probe.h"
#include "../palette.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {

void DoctorPage::draw() {
  PageWindow page("Doctor", "Esc back", ctx_.fonts.big);
  if (!probed_) { report_ = gpu::probe(); gpu::materialize(report_); probed_ = true; }

  ImGui::Text("display");
  if (report_.wayland) ImGui::TextDisabled("  wayland   %s", report_.wayland_display.c_str());
  if (report_.x11) ImGui::TextDisabled("  x11       %s", report_.x_display.c_str());
  ImGui::Spacing();
  ImGui::Text("graphics");
  for (const gpu::Device& d : report_.devices) {
    ImGui::TextDisabled("  %-22s %-8s %s", d.node.c_str(), gpu::vendor_name(d.vendor),
                        d.readable ? "accessible" : "NOT ACCESSIBLE");
  }
  if (report_.nvidia_present()) {
    ImGui::Spacing();
    ImGui::Text("nvidia");
    ImGui::TextDisabled("  driver %s, %zu libraries linked", report_.nvidia_version().c_str(),
                        report_.nvidia_libs().size());
  }
  ImGui::Spacing();
  ImGui::Text("runtime");
  ImGui::TextDisabled("  %s", ctx_.env.root.c_str());
  ImGui::Spacing();
  const std::vector<std::string> problems = report_.problem_lines();
  if (problems.empty()) {
    ImGui::TextColored(kHealthy, "no problems found");
  } else {
    for (const std::string& p : problems) {
      ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      ImGui::TextWrapped("%s", p.c_str());
      ImGui::PopStyleColor();
    }
  }
}

}  // namespace kg::gui::shelf
