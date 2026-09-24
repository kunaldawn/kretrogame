// The staging layout an install lays itself out in, as one set of functions.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace kg::install {

// Where a running install stages itself: cache_dir()/install-<id>, with each
// disc at work/drive-<letter> and the Wine prefix at work/prefix. `kretro swap
// <id> <n>` finds an install it did not start through these same functions,
// so this layout is part of the CLI's contract and not an implementation
// detail.
std::filesystem::path staging_dir(const std::string& id);

// The drive letter disc `disc_index` (0-based) is mounted at: 'd', 'e', ...
char staging_drive_letter(size_t disc_index);
// work/drive-<letter>: where disc `disc_index` (0-based) is laid out.
std::filesystem::path staging_drive(const std::filesystem::path& work, size_t disc_index);
// work/prefix: the install's Wine prefix.
std::filesystem::path staging_prefix(const std::filesystem::path& work);

// Where the installer wrote, under a staging directory: the C: of the throwaway
// prefix the install ran in.
//
// Seven places walk it, diff it, rank what is under it or move the game out of
// it, and install::run's manifest path is an eighth. All of them ask here, so
// that "where an install stages itself" is one sentence rather than eight
// spellings of the same three path components - which is what it was, and this
// function had no caller at all.
std::filesystem::path staging_drive_c(const std::filesystem::path& work);

// The id a staging directory is named for: install-<id> gives <id>, and any
// other name is its own answer. The inverse of staging_dir, read back off the
// path rather than passed around beside it and allowed to disagree.
std::string id_of_staging(const std::filesystem::path& work);

}  // namespace kg::install
