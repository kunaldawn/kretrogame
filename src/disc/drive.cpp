#include "drive.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace kg::disc {

void write_drive_metadata(const fs::path& drive_dir, const std::string& label, uint32_t serial) {
  std::error_code ec;
  fs::create_directories(drive_dir, ec);
  {
    std::ofstream f(drive_dir / ".windows-label", std::ios::trunc);
    if (!f) throw std::runtime_error("disc: cannot write the drive label");
    f << label << "\n";
  }
  {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08x", serial);
    std::ofstream f(drive_dir / ".windows-serial", std::ios::trunc);
    if (!f) throw std::runtime_error("disc: cannot write the drive serial");
    f << hex << "\n";
  }
}

void repoint(const fs::path& prefix, char letter, const fs::path& tree) {
  char l = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
  fs::path dd = prefix / "dosdevices";
  std::error_code ec;
  fs::create_directories(dd, ec);
  fs::path link = dd / (std::string(1, l) + ":");
  fs::remove(link, ec);
  fs::create_directory_symlink(tree, link, ec);
  if (ec) {
    throw std::runtime_error("disc: cannot point " + std::string(1, l) + ": at " + tree.string());
  }
}

void attach_cdrom(const rt::Env& e, const fs::path& prefix, char letter, const fs::path& tree) {
  repoint(prefix, letter, tree);

  char l = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
  fs::path wine = rt::which(e, "wine");
  if (wine.empty()) throw std::runtime_error("disc: the runtime has no wine");
  // No display, as for every Wine command run before the compositor; see
  // rt::offscreen.
  rt::Env we = rt::offscreen(e);
  we.set("WINEPREFIX", prefix.string());
  auto r = rt::run(we, wine, {"reg", "add", "HKLM\\Software\\Wine\\Drives", "/v",
                              std::string(1, l) + ":", "/t", "REG_SZ", "/d", "cdrom", "/f"});
  if (!r.ok()) throw std::runtime_error("disc: cannot register the CD-ROM drive\n" + r.out);
}

void mount_cdrom(const rt::Env& e, const fs::path& prefix, char letter, const fs::path& tree,
                 const std::string& label, uint32_t serial) {
  write_drive_metadata(tree, label, serial);
  attach_cdrom(e, prefix, letter, tree);
}

}  // namespace kg::disc
