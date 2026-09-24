#include "file_list.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#include "format.h"
#include "imgui.h"

namespace kg::gui {
namespace fs = std::filesystem;

FilePick file_list(fs::path& dir, const FileListSpec& spec) {
  FilePick pick;
  std::error_code ec;
  ImGui::BeginChild(spec.child_id, ImVec2(0, spec.height), true);
  ImGui::TextDisabled("%s", dir.c_str());
  // Before the listing, so the parent is what this frame lists.
  if (ImGui::Selectable("..")) dir = dir.parent_path();
  std::vector<fs::path> dirs, files;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    if (spec.skip_hidden) {
      std::string n = de.path().filename().string();
      if (!n.empty() && n[0] == '.') continue;
    }
    if (de.is_directory(ec)) {
      dirs.push_back(de.path());
      continue;
    }
    if (!de.is_regular_file(ec)) continue;
    if (spec.show_file && !spec.show_file(de.path())) continue;
    files.push_back(de.path());
  }
  std::sort(dirs.begin(), dirs.end());
  std::sort(files.begin(), files.end());
  for (const fs::path& d : dirs) {
    ImGui::PushID(d.c_str());
    if (ImGui::Selectable((d.filename().string() + "/").c_str())) dir = d;
    if (spec.dir_button) {
      ImGui::SameLine(ImGui::GetWindowWidth() - 130);
      if (ImGui::SmallButton(spec.dir_button)) pick = {FilePick::DirButton, d};
    }
    ImGui::PopID();
  }
  for (const fs::path& f : files) {
    ImGui::PushID(f.c_str());
    if (ImGui::Selectable(f.filename().c_str())) pick = {FilePick::File, f};
    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    ImGui::TextDisabled("%s", human_size(fs::file_size(f, ec)).c_str());
    ImGui::PopID();
  }
  if (spec.empty_text && dirs.empty() && files.empty()) ImGui::TextDisabled("%s", spec.empty_text);
  ImGui::EndChild();
  return pick;
}

}  // namespace kg::gui
