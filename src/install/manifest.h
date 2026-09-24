// Game manifests: the human-authored TOML a local install starts from, and
// the directories they are looked for in.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "../pack/kgpack.h"
#include "../rt/env.h"

namespace kg::install {

// Reads a game manifest. Manifests are the human-authored source; they are
// compiled into a pack's metadata at install time.
Meta load_manifest(const std::filesystem::path& toml);

// Where local manifests are looked for, in preference order. None is required.
std::vector<std::filesystem::path> manifest_dirs(const rt::Env& e);
std::filesystem::path find_manifest(const rt::Env& e, const std::string& id);
std::vector<std::string> known_games(const rt::Env& e);

}  // namespace kg::install
