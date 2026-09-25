#include "file_list.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "focus.h"
#include "format.h"
#include "imgui.h"
#include "palette.h"
#include "scale.h"
#include "widgets.h"

namespace kg::gui {
namespace fs = std::filesystem;

namespace {

// Where the list is looking, as a terminal prompt shows a directory: the
// folders in cyan, the slashes between them dim. When it is too long for
// the line, the folders nearest the root go first and an ellipsis stands for
// them, since the end of a path is the part that says where one is.
void breadcrumb(const fs::path& dir) {
  std::vector<std::string> parts;
  for (const fs::path& p : dir) {
    const std::string s = p.string();
    if (!s.empty() && s != "/") parts.push_back(s);
  }
  const float avail = ImGui::GetContentRegionAvail().x;
  const float slash = ImGui::CalcTextSize("/").x;
  const float ell = ImGui::CalcTextSize("\xe2\x80\xa6/").x;
  size_t first = 0;
  auto width = [&](size_t from) {
    float w = from > 0 ? ell : 0.0f;
    for (size_t i = from; i < parts.size(); ++i) w += slash + ImGui::CalcTextSize(parts[i].c_str()).x;
    return w;
  };
  while (first + 1 < parts.size() && width(first) > avail) ++first;

  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextUnformatted(first > 0 ? "\xe2\x80\xa6" : "");
  ImGui::PopStyleColor();
  for (size_t i = first; i < parts.size(); ++i) {
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted("/");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, kCyan);
    ImGui::TextUnformatted(elide(nullptr, parts[i], avail - slash).c_str());
    ImGui::PopStyleColor();
  }
  if (parts.empty()) {
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted("/");
    ImGui::PopStyleColor();
  }
}

// A rule across the list under the breadcrumb, on whole pixels.
void rule() {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float thin = std::max(1.0f, std::round(px(1)));
  const float w = ImGui::GetContentRegionAvail().x;
  ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + w, at.y + thin), u32(kLine));
  ImGui::Dummy(ImVec2(w, thin));
}

// A row's label: the name, or when it is wider than the row, the name cut
// short with an ellipsis rather than clipped through a glyph. The ID stays the
// whole name either way, after "###", so a row keeps its identity whatever
// width it is drawn at.
std::string row_label(const std::string& name, float w) {
  const std::string shown = elide(nullptr, name, w);
  return shown == name ? name : shown + "###" + name;
}

// What a list last read of its folder: every folder and regular file in it,
// sorted, with the files' sizes.
struct Listing {
  Refresh refresh;
  std::vector<fs::path> dirs;
  std::vector<std::pair<fs::path, uint64_t>> files;
};

// The listings, one for each list on screen, by its child window's ID. A
// folder is read when its list comes up, when the list moves to another
// folder, and every couple of seconds, rather than on every frame: a folder of
// a few hundred files is a directory read and a few hundred file sizes a
// frame, and the page drawing it stutters.
std::unordered_map<ImGuiID, Listing> g_listings;

void read_listing(Listing& l, const fs::path& dir) {
  std::error_code ec;
  l.dirs.clear();
  l.files.clear();
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    if (de.is_directory(ec)) {
      l.dirs.push_back(de.path());
      continue;
    }
    if (!de.is_regular_file(ec)) continue;
    std::error_code sec;
    l.files.emplace_back(de.path(), static_cast<uint64_t>(fs::file_size(de.path(), sec)));
  }
  std::sort(l.dirs.begin(), l.dirs.end());
  std::sort(l.files.begin(), l.files.end());
}

bool hidden(const fs::path& p) {
  const std::string n = p.filename().string();
  return !n.empty() && n[0] == '.';
}

}  // namespace

FilePick file_list(fs::path& dir, const FileListSpec& spec) {
  FilePick pick;
  Listing& listing = g_listings[ImGui::GetID(spec.child_id)];
  begin_panel(spec.child_id, ImVec2(0, std::round(px(spec.height))), px(12, 10));
  // Rows closer together than a page's paragraphs: this is a listing.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, std::round(px(3))));
  breadcrumb(dir);
  rule();

  // The right edge of the rows, measured once the scrollbar has taken its
  // share, so what is right-aligned sits inside the list at every scale.
  const float gap = std::round(px(12));
  const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
  const float left = ImGui::GetCursorPosX();

  // Before the listing, so the parent is what this frame lists.
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  if (ImGui::Selectable("..")) dir = dir.parent_path();
  ImGui::PopStyleColor();
  if (listing.refresh.due(dir.string(), 2.0)) read_listing(listing, dir);
  std::vector<fs::path> dirs;
  std::vector<std::pair<fs::path, uint64_t>> files;
  for (const fs::path& d : listing.dirs) {
    if (!(spec.skip_hidden && hidden(d))) dirs.push_back(d);
  }
  for (const auto& f : listing.files) {
    if (spec.skip_hidden && hidden(f.first)) continue;
    if (spec.show_file && !spec.show_file(f.first)) continue;
    files.push_back(f);
  }

  // The folder button's width, as small_button lays it out, so the row's
  // name stops short of it rather than running under it.
  const float button_w = spec.dir_button ? small_button_width(spec.dir_button) : 0.0f;
  for (const fs::path& d : dirs) {
    ImGui::PushID(d.c_str());
    const float name_w = std::max(1.0f, right - left - (spec.dir_button ? button_w + gap : 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    if (ImGui::Selectable(row_label(d.filename().string() + "/", name_w).c_str(), false, 0, ImVec2(name_w, 0))) {
      dir = d;
    }
    ImGui::PopStyleColor();
    if (spec.dir_button) {
      ImGui::SameLine(std::max(left, right - button_w));
      if (small_button(spec.dir_button)) pick = {FilePick::DirButton, d};
    }
    ImGui::PopID();
  }
  for (const auto& [f, bytes] : files) {
    ImGui::PushID(f.c_str());
    const std::string size = human_size(bytes);
    const float size_w = ImGui::CalcTextSize(size.c_str()).x;
    const float name_w = std::max(1.0f, right - left - size_w - gap);
    if (ImGui::Selectable(row_label(f.filename().string(), name_w).c_str(), false, 0, ImVec2(name_w, 0))) {
      pick = {FilePick::File, f};
    }
    ImGui::SameLine(std::max(left, right - size_w));
    ImGui::TextDisabled("%s", size.c_str());
    ImGui::PopID();
  }
  if (spec.empty_text && dirs.empty() && files.empty()) ImGui::TextDisabled("%s", spec.empty_text);
  ImGui::PopStyleVar();
  end_panel();
  return pick;
}

}  // namespace kg::gui
