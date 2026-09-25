// The bundle's own settings: the applications-menu entry, the data folder,
// the licences, and About, which is the doctor with a Save button.
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>

#include "../../util/paths.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::launcher {
namespace fs = std::filesystem;

void BundlePage::draw() {
  const bundle::BundleMeta& m = ctx_.p.bundle().meta;
  set_chrome_path("~/settings");
  set_trail(ctx_, std::string(), "settings");
  PageWindow page((m.title + " - settings").c_str(), "Esc back", ctx_.w.fonts().big());
  // Looked for when the page comes up, not on every frame; the buttons below
  // that change it look again.
  if (disk_.due("")) {
    std::error_code ec;
    installed_ = fs::exists(ctx_.desktop_paths().entry, ec);
  }
  const bool installed = installed_;
  // Rows in a column of a readable width down the middle of the page, each
  // a name and where it stands at the left and what changes it at the right.
  begin_scroll("rows", ImVec2(0, 0));
  centre_column(content_max_w(Content::Form));
  step_heading("Settings");

  nav_section_begin("menu");
  section("applications menu");
  setting_begin("entry", installed ? "in your applications menu" : "not in your applications menu");
  if (installed) {
    const bool remove = ghost_button("Remove from the applications menu");
    default_focus();
    if (remove) {
      player::remove_desktop_entry(ctx_.desktop_paths());
      ctx_.state.desktop_installed = false;
      ctx_.save_state();
      ctx_.status = "removed from your applications menu";
      disk_.invalidate();
    }
  } else {
    const bool add = ghost_button("Add to the applications menu");
    default_focus();
    if (add) {
      ctx_.add_desktop_entry();
      disk_.invalidate();
    }
  }
  setting_end();
  nav_section_end();

  nav_section_begin("data");
  section("data");
  const std::string folder = state_dir().string();
  setting_begin("folder", folder.c_str());
  if (ghost_button("Open the data folder")) {
    std::string url = "file://" + state_dir().string();
    if (SDL_OpenURL(url.c_str()) != 0) ctx_.status = "could not open it: " + std::string(SDL_GetError());
  }
  setting_end();
  nav_section_end();

  nav_section_begin("about");
  section("about");
  float key_w = 0;
  for (const char* k : {"bundle", "version", "built by"}) key_w = std::max(key_w, ImGui::CalcTextSize(k).x);
  if (kv_begin("about", key_w + std::round(px(12)))) {
    kv("bundle", m.id, kCyan);
    kv("version", m.version);
    if (!m.kretro_version.empty()) kv("built by", "kretro " + m.kretro_version);
    kv_end();
  }
  vgap(6);
  if (ghost_button("Licences")) ctx_.go(Screen::Licenses);
  ImGui::SameLine();
  if (ghost_button("About and diagnostics")) ctx_.go(Screen::About);
  nav_section_end();
  if (!ctx_.status.empty()) {
    vgap(8);
    ImGui::TextWrapped("%s", ctx_.status.c_str());
  }
  end_centre_column();
  end_scroll();
}

void LicensesPage::draw() {
  set_chrome_path("~/settings/licences");
  set_trail(ctx_, std::string(), "settings \xe2\x80\xba licences");
  PageWindow page("Licences", "Esc back", ctx_.w.fonts().big());
  if (licenses_text_.empty()) licenses_text_ = ctx_.p.licenses_text();
  // A scrolling panel for a page of text, in the same frame as the shelf's
  // job log, in a column of a readable width.
  centre_column(content_max_w(Content::Reading));
  step_heading("Licences");
  begin_text_panel("text", ImVec2(0, 0), px(16, 12));
  // The text is paragraphs, lists indented by two spaces, and a line ending
  // in a colon before each list: that line is drawn as the list's heading.
  size_t at = 0;
  const std::string& t = licenses_text_;
  while (at < t.size()) {
    size_t nl = t.find('\n', at);
    if (nl == std::string::npos) nl = t.size();
    const std::string line = t.substr(at, nl - at);
    at = nl + 1;
    if (line.empty()) {
      vgap(6);
    } else if (line.rfind("  ", 0) == 0) {
      ImGui::Indent(std::round(px(24)));
      ImGui::TextWrapped("%s", line.c_str() + 2);
      ImGui::Unindent(std::round(px(24)));
    } else if (line.back() == ':') {
      // A heading that names where the notices are, "Notices, in /usr/...",
      // is a short heading and the place under it in the colour paths are
      // shown in, so a deep install does not push the heading's rule off the
      // panel.
      const std::string head = line.substr(0, line.size() - 1);
      const size_t in = head.find(", in /");
      if (in == std::string::npos) {
        section(head.c_str());
      } else {
        std::string label = head.substr(0, in);
        label[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(label[0])));
        section(label.c_str());
        colored_text(kCyan, head.substr(in + 5));
      }
      vgap(2);
    } else {
      // Paragraphs at a comfortable reading width, their lines a little
      // apart, as the notes on the settings pages are.
      spaced_text(line, kText, std::round(px(820)));
    }
  }
  edge_fades(kBg1, 40);
  end_panel();
  // The page is its text: it opens on it, so the arrows and PageDown scroll
  // it straight away.
  default_focus();
  end_centre_column();
}

void AboutPage::draw() {
  set_chrome_path("~/settings/about");
  set_trail(ctx_, std::string(), "settings \xe2\x80\xba about");
  PageWindow page("About and diagnostics", "Esc back", ctx_.w.fonts().big());
  centre_column(content_max_w(Content::Reading));
  step_heading("About and diagnostics");
  if (!ctx_.checked && !ctx_.job.running()) {
    empty_state("not checked yet");
    end_centre_column();
    return;
  }
  if (!have_report_) {
    report_ = ctx_.report;
    have_report_ = true;
  }
  if (primary_button("Save report")) ctx_.save_report();
  default_focus();
  ImGui::SameLine(0, std::round(px(16)));
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextWrapped("the saved copy has no home directory and no user name in it");
  ImGui::PopStyleColor();
  if (!ctx_.status.empty()) ImGui::TextWrapped("%s", ctx_.status.c_str());
  vgap(4);
  // What doctor::render prints, drawn as the page's own sections: the same
  // headings, labels and values, and the problems as badges.
  begin_text_panel("report", ImVec2(0, 0), px(16, 12));
  // One key column for every section, so the values line up down the page.
  float key_w = 0;
  for (const player::doctor::Section& sec : report_.sections) {
    for (const player::doctor::Line& l : sec.lines) key_w = std::max(key_w, ImGui::CalcTextSize(l.label.c_str()).x);
  }
  key_w += std::round(px(12));
  for (const player::doctor::Section& sec : report_.sections) {
    section(sec.title.c_str());
    if (kv_begin(sec.title.c_str(), key_w)) {
      for (const player::doctor::Line& l : sec.lines) kv(l.label.c_str(), l.value);
      kv_end();
    }
    vgap(6);
  }
  section("problems");
  if (report_.problems.empty()) {
    badge_line(BadgeKind::Ok, "no problems found");
  }
  for (const gpu::Problem& pr : report_.problems) {
    badge_line(pr.blocking() ? BadgeKind::Fail : BadgeKind::Warn, pr.line(), pr.blocking() ? kWarn : kWarm);
  }
  if (!report_.log_tail.empty()) {
    vgap(6);
    section("last session log");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", report_.log_tail.c_str());
    ImGui::PopStyleColor();
  }
  edge_fades(kBg1, 40);
  end_panel();
  end_centre_column();
}

}  // namespace kg::gui::launcher
