// Presenting a disc to Wine as a CD-ROM.
//
// Most CD checks of this era ask Windows for the volume label of the drive the
// game was installed from. Wine reads that label from a .windows-label file in
// the drive's directory and the serial from .windows-serial, and it reads the
// drive's type from the registry. Give it all three, correctly, and the check
// passes because the disc genuinely is there - no patched executable involved.
//
// Raw-sector protection (SafeDisc, SecuROM) reads below the filesystem and
// cannot be satisfied this way. That is a stated limit, not an oversight.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "../rt/env.h"

namespace kg::disc {

// Writes .windows-label and .windows-serial into a drive directory.
void write_drive_metadata(const std::filesystem::path& drive_dir, const std::string& label,
                          uint32_t serial);

// Points dosdevices/<letter>: at `tree`. Wine resolves the symlink on each
// open, so re-pointing swaps the disc with nothing to restart.
void repoint(const std::filesystem::path& prefix, char letter, const std::filesystem::path& tree);

// Points dosdevices/<letter>: at `tree` and registers it as a CD-ROM. The tree
// must already carry its metadata: at play time it is a read-only DwarFS mount,
// which is the whole reason those two files travel inside the pack. Pointing
// the drive at the saves overlay instead would work exactly once and then put
// .windows-label into every snapshot and every save export.
void attach_cdrom(const rt::Env& e, const std::filesystem::path& prefix, char letter,
                  const std::filesystem::path& tree);

// Both halves, for the install path, which mounts disc trees it just extracted
// and can still write to them.
void mount_cdrom(const rt::Env& e, const std::filesystem::path& prefix, char letter,
                 const std::filesystem::path& tree, const std::string& label, uint32_t serial);

}  // namespace kg::disc
