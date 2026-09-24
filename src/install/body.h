// The body of a rooted pack: the directory mkdwarfs is pointed at.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kg::install {

// One disc, as it will appear in the body.
struct BodyDisc {
  std::filesystem::path tree;                 // the extracted disc
  std::string label;
  uint32_t serial = 0;
  std::vector<std::filesystem::path> audio;   // the ripped tracks
};

// Lays the body out under `root`, which mkdwarfs is then pointed at:
//
//   game/            the installed tree, byte for byte what a flat body was
//   system/          what the installer wrote to C: outside the game directory,
//                    relative to drive_c: windows/system32/msvcrt.dll and the
//                    rest of what a 1998 setup scatters around the machine
//   discs/<n>/       disc n, with .windows-label and .windows-serial already
//                    written - at play time the tree is a read-only mount and
//                    nothing can put them there
//   discs/<n>/audio/ the FLAC tracks ripped from that disc
//   registry.reg     the fragment, as text, for a person who mounts the image
//
// `tree`, `system` and each disc tree are moved where the filesystem allows and
// hardlinked otherwise: they are already gigabytes and already inside the
// staging directory, and a copy here would double the disk an install needs.
// Everything named is consumed - do not read `tree` or a disc's tree after.
//
// `system` may be empty, and is for every copy and unzip install: no installer
// ran, so nothing was written outside the tree to carry.
void lay_out_body(const std::filesystem::path& root, const std::filesystem::path& tree,
                  const std::vector<BodyDisc>& discs, const std::string& registry_fragment,
                  const std::filesystem::path& system = {});

}  // namespace kg::install
