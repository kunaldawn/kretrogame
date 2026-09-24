// The verified memo: which packs this state has already checked.
#pragma once

#include <filesystem>
#include <string>

#include "../util/hash.h"

namespace kg::player {

// Which packs have already been checked against their BLAKE3, per bundle
// version. Hashing a pack reads every byte of it, gigabytes for a game that
// carries its disc; doing it on every launch would make the second launch as
// slow as the first. The entry's hash is part of the key, so a rebuilt pack
// under the same version string is still checked.
bool pack_verified(const std::filesystem::path& memo, const std::string& game,
                   const std::string& version, const Hash& h);
void remember_verified(const std::filesystem::path& memo, const std::string& game,
                       const std::string& version, const Hash& h);

}  // namespace kg::player
