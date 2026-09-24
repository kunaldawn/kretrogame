// What an installer wrote to C: outside the game's own directory: picking it
// out of the diff, carrying it in the body's system/, and putting it back into
// a prefix at play time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kg::wine {

// Whether a path the installer wrote, relative to drive_c, belongs in system/.
//
// Everything it wrote that is not under the game's own directory does - that is
// the whole point - except the scratch it left behind. An installer unpacks
// itself into C:\windows\Temp or the user's Temp and does not clean up; those
// megabytes are not part of the game by anybody's reading, and carrying them
// would put an InstallShield self-extraction into every pack.
//
// `install_dir` is compared case-insensitively and with either slash, because
// it is a Windows path that reached us through a Linux filesystem.
bool is_outside_the_game(const std::string& path, const std::string& install_dir);

// Collects `paths`, relative to `drive_c`, into `into`, keeping their layout.
// Returns how many files landed and how many bytes they are. Missing entries
// are skipped rather than fatal: the game's own directory has already been
// moved out of drive_c by the time this runs, and an installer's temporary file
// may have been deleted by the installer itself between the diff and here.
struct SystemFiles {
  size_t files = 0;
  uint64_t bytes = 0;
};
SystemFiles gather_system_files(const std::filesystem::path& drive_c,
                                const std::vector<std::string>& paths,
                                const std::filesystem::path& into);

// The other end: puts a pack's system/ back into a prefix's drive_c. Returns
// how many files it placed.
//
// It overwrites, and has to. What is here is what the installer put on the
// machine that built the pack, on top of what wineboot had just laid down
// there; the registry fragment that is imported immediately after names these
// files by path, so restoring anything less than what the installer left would
// register a COM class against a DLL that is not there.
size_t restore_system_files(const std::filesystem::path& system,
                            const std::filesystem::path& drive_c);

}  // namespace kg::wine
