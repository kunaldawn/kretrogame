// What a game's executable says about how it draws: the backend the player's
// "auto" would pick for it, and whether it can only draw through Glide.
#pragma once

#include <filesystem>
#include <string>

#include "../../util/pe.h"
#include "pack_facts.h"

namespace kg::bundle {

// ---- graphics: what "auto" would do ---------------------------------------------

struct AutoBackend {
  bool known = false;       // the executable was read
  std::string backend;      // dxvk | wined3d-vk | wined3d-gl | cnc-ddraw | native OpenGL
  std::string reason;       // backend::choose_backend's own sentence
};

// What the player's policy picks from these imports on a machine with a GPU
// and Vulkan 1.4: the question the author is asking is what the game wants,
// not what this machine has.
AutoBackend auto_backend(const pe::Imports& exe);

// The game's executable, read out of the pack's body with dwarfsextract, and
// its imports. `tool` is dwarfs-universal; `scratch` a directory this may
// fill and empties again. Never throws: a pack that cannot be read gives
// Imports with ok false and the reason.
pe::Imports read_exe_imports(const PackFacts& f, const std::filesystem::path& tool,
                             const std::filesystem::path& scratch);

// The executable imports Glide and no Direct3D, DirectDraw or OpenGL: there
// is nothing for the player to draw it with except a Glide wrapper.
bool glide_only(const pe::Imports& exe);

}  // namespace kg::bundle
