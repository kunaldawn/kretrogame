#include "source.h"

#include <algorithm>
#include <system_error>

#include "../disc/container.h"
#include "../util/text.h"

namespace kg::install {
namespace fs = std::filesystem;

Source classify_source(const fs::path& p) {
  Source s;
  s.path = p;
  s.kind = Source::Kind::Unreadable;
  std::error_code ec;
  if (!fs::exists(p, ec)) {
    s.trouble = "there is no such file";
    return s;
  }
  // A directory is a mounted CD or a disc somebody already extracted. It is
  // the one kind probe_into (disc/container.cpp) returns nothing for, and
  // the one kind that costs nothing to accept.
  if (fs::is_directory(p, ec)) {
    s.kind = Source::Kind::Directory;
    return s;
  }
  if (!fs::is_regular_file(p, ec)) {
    s.trouble = "not a file or a directory";
    return s;
  }
  // By name only. This runs as the user adds files, and opening a 6.8 GB DVD
  // to find out what it is would make the list stutter for every drop.
  std::string name = p.filename().string();
  if (disc::is_disc_image(name)) {
    s.kind = Source::Kind::DiscImage;
    return s;
  }
  if (disc::is_archive(name)) {
    s.kind = Source::Kind::Archive;
    return s;
  }
  if (to_lower(p.extension().string()) == ".exe") {
    s.kind = Source::Kind::BareExe;
    return s;
  }
  s.trouble = name + " is not a disc image, an archive or an installer";
  return s;
}

fs::path bare_exe(const std::vector<Source>& sources) {
  for (const Source& s : sources) {
    if (s.kind == Source::Kind::BareExe) return s.path;
  }
  return {};
}

bool sources_are_enough(const std::vector<Source>& sources, size_t discs) {
  return discs > 0 || !bare_exe(sources).empty();
}

std::vector<Draft::Method> methods_for(const std::vector<Source>& sources, size_t discs) {
  std::vector<Draft::Method> out;
  bool bare = !bare_exe(sources).empty();
  // With neither a disc nor an .exe there is still a page to draw, and what it
  // should say is that it wants a disc.
  if (discs > 0 || !bare) out.push_back(Draft::Method::Installer);
  if (discs > 0) {
    out.push_back(Draft::Method::Copy);
    out.push_back(Draft::Method::Unzip);
  }
  if (bare) out.push_back(Draft::Method::InstallerExe);
  return out;
}

Draft::Method clamp_method(const std::vector<Draft::Method>& offered, Draft::Method want) {
  if (offered.empty()) return want;
  if (std::find(offered.begin(), offered.end(), want) != offered.end()) return want;
  // methods_for lists them in the order the page offers them, so the first is
  // what the combo would have shown anyway.
  return offered.front();
}

}  // namespace kg::install
