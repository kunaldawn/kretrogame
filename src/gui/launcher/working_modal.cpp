// The launcher's own "working" modal: smaller than the shelf's job modal, with
// no log, only the job's title and how far it has got.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {

namespace {

// The fraction a progress text such as "61%" stands for, or -1 when it does
// not start with one: the check of a pack reports a percentage, other jobs
// nothing at all.
float fraction_of(const std::string& s) {
  size_t n = 0;
  while (n < s.size() && n < 3 && std::isdigit(static_cast<unsigned char>(s[n]))) ++n;
  if (n == 0 || n >= s.size() || s[n] != '%') return -1.0f;
  return static_cast<float>(std::min(100, std::stoi(s.substr(0, n)))) / 100.0f;
}

}  // namespace

void working_modal(LauncherContext& ctx) {
  ImGui::OpenPopup("working");
  if (dialog_begin("working", 640, ctx.job.title())) {
    std::string p = ctx.progress_text();
    const float f = fraction_of(p);
    spinner();
    ImGui::SameLine(0, std::round(px(10)));
    if (f >= 0) block_progress(f);
    else block_progress(-1.0f, 0, p.empty() ? "working\u2026" : p.c_str());
    vgap(4);
    dialog_end();
  }
}

}  // namespace kg::gui::launcher
