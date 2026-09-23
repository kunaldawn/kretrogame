#include "desktop.h"

#include <fstream>
#include <stdexcept>

#include "state.h"

namespace kg::player {
namespace fs = std::filesystem;

DesktopPaths desktop_paths(const std::string& bundle_id, const std::string& xdg_data_home,
                           const std::string& home) {
  fs::path data;
  if (!xdg_data_home.empty() && fs::path(xdg_data_home).is_absolute()) data = xdg_data_home;
  else if (!home.empty()) data = fs::path(home) / ".local" / "share";
  // No menu of the person's own to add to. /tmp/.local was the stand-in, and
  // a directory anyone can make first is one whose files anyone can point
  // elsewhere; no desktop reads a menu from there anyway.
  else return {};
  const std::string name = "kretro-" + bundle_id;
  return {data / "applications" / (name + ".desktop"), data / "icons" / (name + ".png")};
}

namespace {

// A value in a .desktop file: newlines and backslashes escaped, as the
// Desktop Entry spec wants for a string.
std::string entry_value(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (c == '\\') o += "\\\\";
    else o.push_back(c);
  }
  return o;
}

// The executable as an Exec argument: quoted, with the characters the spec
// reserves inside quotes escaped, and every % doubled so no field code is
// read out of a directory name. Then the whole thing is escaped once more as
// a string value, because the spec applies both.
std::string exec_arg(const fs::path& exe) {
  std::string q = "\"";
  for (char c : exe.string()) {
    if (c == '"' || c == '`' || c == '$' || c == '\\') q.push_back('\\');
    if (c == '%') q.push_back('%');
    q.push_back(c);
  }
  return entry_value(q + "\"");
}

}  // namespace

std::string desktop_entry(const std::string& title, const std::string& bundle_id,
                          const fs::path& exe, const fs::path& icon) {
  std::string s;
  s += "[Desktop Entry]\n";
  s += "Type=Application\n";
  s += "Version=1.5\n";
  s += "Name=" + entry_value(title) + "\n";
  s += "Comment=" + entry_value("Play " + title) + "\n";
  s += "Exec=" + exec_arg(exe) + "\n";
  // An absolute path rather than a theme name: the icon is ours alone and a
  // theme lookup would only be a slower way to find the same file.
  s += "Icon=" + entry_value(icon.empty() ? std::string("applications-games") : icon.string()) + "\n";
  s += "Terminal=false\n";
  s += "Categories=Game;\n";
  s += "StartupNotify=true\n";
  // Which bundle made this, so removing it later is removing ours and only ours.
  s += "X-Kretro-Bundle=" + entry_value(bundle_id) + "\n";
  return s;
}

void install_desktop_entry(const DesktopPaths& where, const std::string& title,
                           const std::string& bundle_id, const fs::path& exe,
                           const std::string& icon_png) {
  if (where.entry.empty()) {
    throw std::runtime_error("there is no home directory here, so no applications menu to add this to");
  }
  fs::path icon;
  if (!icon_png.empty()) {
    write_atomically(where.icon, icon_png);
    icon = where.icon;
  }
  write_atomically(where.entry, desktop_entry(title, bundle_id, exe, icon));
  std::error_code ec;
  // Executable, because some desktops refuse to launch an entry that is not
  // marked trusted, and that is how a user-written one is marked.
  fs::permissions(where.entry, fs::perms::owner_exec, fs::perm_options::add, ec);
}

bool remove_desktop_entry(const DesktopPaths& where) {
  std::error_code ec;
  bool a = fs::remove(where.entry, ec);
  bool b = fs::remove(where.icon, ec);
  return a || b;
}

}  // namespace kg::player
