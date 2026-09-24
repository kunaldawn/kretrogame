#include <algorithm>
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
  ImGui::Spacing();
  ImGui::Separator();
  const char* what = browse_ == Browse::Folder ? "Choose a folder"
                     : browse_ == Browse::Dll  ? "Choose a file for this game"
                                               : "Choose a PNG";
  ImGui::TextUnformatted(what);
  ImGui::SameLine();
  if (ImGui::SmallButton("close")) browse_ = Browse::None;
  if (browse_ == Browse::Folder) {
    ImGui::SameLine();
    if (ImGui::SmallButton("use this folder")) take_file(browse_dir_);
  }
  FileListSpec spec;
  spec.child_id = "bundle-browse";
  spec.height = 280;
  spec.skip_hidden = true;
  // A folder is chosen with the button above, so its list offers no files;
  // a game's extra files may be anything, and every picture is a PNG.
  const Browse mode = browse_;
  spec.show_file = [mode](const fs::path& p) {
    if (mode == Browse::Folder) return false;
    return mode == Browse::Dll || to_lower(p.extension().string()) == ".png";
  };
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
