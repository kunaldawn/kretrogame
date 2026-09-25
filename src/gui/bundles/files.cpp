#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <system_error>

#include "../../util/file_io.h"
#include "../../util/safe_names.h"
#include "../../util/text.h"
#include "../file_list.h"
#include "../format.h"
#include "../widgets.h"
#include "bundles_page.h"
#include "choices.h"

namespace kg::gui {
namespace fs = std::filesystem;
using namespace kg::bundle;
using bundles_detail::is_png;

namespace {

// The pictures go into bundle.meta whole, and bundle.meta is read into memory
// whole by every player. Generous for a picture; small beside 64 MiB.
constexpr uint64_t kMaxPicture = 8ull << 20;
// A dgVoodoo DLL is a few hundred kilobytes.
constexpr uint64_t kMaxExtraFile = 16ull << 20;

}  // namespace

// ---- files -----------------------------------------------------------------------------------

// The same plain list over std::filesystem the wizard and the Import page use,
// and for the same reason: no dialog from another toolkit that a pad cannot
// drive.
void Bundles::browser() {
  const bool opened = browse_shown_ != browse_;
  browse_shown_ = browse_;
  const ImGuiStyle& st = ImGui::GetStyle();
  const float top = ImGui::GetCursorPosY();
  ImGui::Spacing();
  const char* what = browse_ == Browse::Folder ? "Choose a folder"
                     : browse_ == Browse::Dll  ? "Choose a file for this game"
                                               : "Choose a PNG";
  // The heading and the list's own buttons share a row, the buttons where
  // the heading's rule ends, so the list starts straight under its heading.
  const bool folder = browse_ == Browse::Folder;
  const float buttons_w =
      small_button_width("close") + (folder ? small_button_width("use this folder") + st.ItemSpacing.x : 0.0f);
  if (ImGui::BeginTable("browse-head", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
    ImGui::TableSetupColumn("heading", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("buttons", ImGuiTableColumnFlags_WidthFixed, buttons_w);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    section(what);
    ImGui::TableSetColumnIndex(1);
    // section() opens with a Spacing; the same here, less the small
    // buttons' own padding above their text, puts their words on its line.
    ImGui::Spacing();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - std::round(px(2)));
    if (folder) {
      if (small_button("use this folder")) take_file(browse_dir_);
      ImGui::SameLine();
    }
    if (small_button("close")) browse_ = Browse::None;
    ImGui::EndTable();
  }
  // The list heads the pane (bundle_page); the first frame scrolls the pane
  // back to its top, wherever the step had been scrolled to. The list
  // takes half the pane from there, so the step it fills in is still in
  // sight under it, and the keys go on from the list's last entry to the
  // step's first field.
  if (opened) ImGui::SetScrollY(0.0f);
  const float head = ImGui::GetCursorPosY() - top;
  const float fill = ImGui::GetWindowHeight() * 0.5f - head;
  FileListSpec spec;
  spec.child_id = "bundle-browse";
  // In design pixels, as file_list takes it, and never so short that the
  // list shows only a line or two.
  spec.height = std::max(200.0f, std::floor(fill / ui_scale()));
  spec.skip_hidden = true;
  // A folder is chosen with the button above, so its list offers no files;
  // a game's extra files may be anything, and every picture is a PNG.
  const Browse mode = browse_;
  spec.show_file = [mode](const fs::path& p) {
    if (mode == Browse::Folder) return false;
    return mode == Browse::Dll || to_lower(p.extension().string()) == ".png";
  };
  // A step that comes up with its list open opens on the list, which is what
  // the pane has just been scrolled to.
  if (opened) default_focus_next();
  FilePick pick = file_list(browse_dir_, spec);
  if (pick.kind == FilePick::File) take_file(pick.path);
}

void Bundles::take_file(const fs::path& p) {
  std::error_code ec;
  if (browse_ == Browse::Folder) {
    draft_.out_dir = p.string();
    dirty_ = true;
    browse_ = Browse::None;
    return;
  }
  uint64_t size = fs::file_size(p, ec);
  uint64_t cap = browse_ == Browse::Dll ? kMaxExtraFile : kMaxPicture;
  if (ec || size > cap) {
    trouble_ = p.filename().string() + " is " + human_size(size) + ", more than the " + human_size(cap) +
               " a bundle takes for one.";
    return;
  }
  std::string bytes = read_file_or_empty(p);
  if (browse_ != Browse::Dll && !is_png(bytes)) {
    trouble_ = p.filename().string() + " is not a PNG.";
    return;
  }
  trouble_.clear();
  switch (browse_) {
    case Browse::Banner: draft_.banner = bytes; draft_.banner_from = p.string(); break;
    case Browse::Icon: draft_.icon = bytes; draft_.icon_from = p.string(); break;
    case Browse::Cover:
      if (game_pick_ < draft_.games.size()) {
        draft_.games[game_pick_].cover = bytes;
        draft_.games[game_pick_].cover_from = p.string();
      }
      break;
    case Browse::Dll:
      if (game_pick_ < draft_.games.size()) {
        std::string name = p.filename().string();
        if (!kg::id_is_safe(name)) {
          trouble_ = name + " cannot be placed beside a game under that name: rename it first.";
          return;
        }
        auto& dl = draft_.games[game_pick_].extra_dlls;
        dl.erase(std::remove_if(dl.begin(), dl.end(), [&](const GameMeta::Dll& x) { return x.name == name; }),
                 dl.end());
        dl.push_back({name, bytes});
      }
      break;
    default: break;
  }
  dirty_ = true;
  browse_ = Browse::None;
}

}  // namespace kg::gui
