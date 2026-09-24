// The collection directory: where the discs and installers a person owns are
// kept, and the only place a recipe may name a file from.
#pragma once

#include <filesystem>
#include <string>

#include "../pack/kgpack.h"

namespace kg::install {

// Where discs are looked for.
std::filesystem::path iso_dir();
// Whether a name is one this may look up in the collection directory: a bare
// filename, and nothing that walks out of it. A recipe someone sent decides
// which file this opens, and for installer_exe which file Wine then runs, so
// the name has to be answerable inside iso_dir() or not at all.
bool is_collection_name(const std::string& name);
// Case-insensitive, because a manifest records a disc's name as its owner
// typed it and the file on disk may differ. Empty for a name is_collection_name
// refuses.
std::filesystem::path find_iso(const std::string& name);
// Matches a disc by what it *is* rather than what it is called.
std::filesystem::path find_iso_by_fingerprint(const DiscFingerprint& want);

}  // namespace kg::install
