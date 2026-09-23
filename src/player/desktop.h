// "Add to applications menu": a .desktop file and an icon, with consent.
//
// The launcher offers it once. Yes writes two files and nothing else:
//
//   $XDG_DATA_HOME/applications/kretro-<bundle-id>.desktop
//   $XDG_DATA_HOME/icons/kretro-<bundle-id>.png
//
// both named for the bundle, so a newer build of the same bundle replaces the
// entry rather than adding a second one, and the entry points at the file that
// was run - KRETRO_SELF - wherever that is now. A setting removes both. These
// are the only files a player ever writes outside its state and its cache.
#pragma once

#include <filesystem>
#include <string>

namespace kg::player {

struct DesktopPaths {
  std::filesystem::path entry;
  std::filesystem::path icon;
};

// From XDG_DATA_HOME, or ~/.local/share, as a desktop looks for them. Both
// empty with neither: install_desktop_entry then refuses.
DesktopPaths desktop_paths(const std::string& bundle_id, const std::string& xdg_data_home,
                           const std::string& home);

// The text of the .desktop file. `icon` empty means a generic games icon.
std::string desktop_entry(const std::string& title, const std::string& bundle_id,
                          const std::filesystem::path& exe, const std::filesystem::path& icon);

// Writes both files; `icon_png` may be empty, and then no icon file is
// written. Throws std::runtime_error naming the file that could not be.
void install_desktop_entry(const DesktopPaths& where, const std::string& title,
                           const std::string& bundle_id, const std::filesystem::path& exe,
                           const std::string& icon_png);
// Removes both, and says whether there was anything to remove.
bool remove_desktop_entry(const DesktopPaths& where);

}  // namespace kg::player
