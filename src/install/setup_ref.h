// Naming an installer: which disc, and the path on it.
//
// The wizard writes "<n>/<path on disc n>", a manifest writes the path alone
// and names the disc in a field of its own, and a recipe carries the two
// halves as recipe.setup_ref and recipe.setup. Everything that reads one form
// and writes another is here, together with the guard on what a pack may name.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "../pack/kgpack.h"
#include "draft.h"

namespace kg::install {

// The wizard's setup form taken apart: which disc, and the path on it.
//
// "<n>/<path on disc n>" is what step 3 writes, because which executable is
// not an answer without which disc. A path alone - a manifest's, or an
// installer somebody downloaded - names no disc and is left exactly as it is,
// and *that* is the whole of the care this needs: a real setup may be
// Game3/Setup.exe on the disc, and reading "Game3" as a disc number ran
// Setup.exe from the root of a disc that has none. Only a leading run of
// digits is a number, the same rule setup_index has always used.
struct SetupRef {
  size_t disc = 0;    // 1-based, in the order open_sources assembled the set;
                      // zero when the form named no disc at all
  std::string path;   // what is left, with forward slashes
};
SetupRef split_setup_ref(const std::filesystem::path& setup);

// Is this a setup path a pack is allowed to name? A recipe arrives from
// whoever sent it, and the file it names is executed under Wine - so an
// absolute path, or one that climbs out with "..", is a stranger choosing a
// file on your machine rather than one on the disc he sent you.
// install::is_collection_name says this about the archive; this says it about
// the path inside. staged_setup enforces it on the wizard's road to a setup,
// and draft_from_meta clears a setup that fails it; install::run takes a road
// of its own, resolving the path under the mounted disc with resolve_ci.
bool setup_path_is_safe(const std::string& setup);

// The file run_setup is actually handed, given where this install staged
// itself. mount_discs puts disc n at work/drive-<'d'+n-1>, so this is that
// join and the inverse of what step 3 wrote. An installer_exe's setup is
// already a path on this filesystem - nothing is mounted for it - and comes
// back unchanged.
std::filesystem::path staged_setup(const std::filesystem::path& work, const Draft& d);

// Step 3's list of possible installers: which disc, and the path on it.
using SetupChoice = std::pair<size_t, std::filesystem::path>;

// Which entry of that list a draft already names, or setups.size() when none
// does. The wizard writes "<n>/<path on disc n>" because which executable is
// not an answer without which disc; a manifest writes the path alone and says
// which disc in a field of its own. Both forms resolve here, either slash and
// either case, because a disc spells SETUP.EXE however it likes.
//
// This is what keeps an accepted preset's setup. A disc may carry INSTALL.EXE
// and SETUP.EXE, only one of them installs the game, and knowing
// which is most of why that manifest exists - so step 3 ranking its own list
// must not silently answer the question again.
size_t setup_index(const std::vector<SetupChoice>& setups,
                   const std::filesystem::path& setup);

// The other direction: a recipe's two fields back into the wizard's one.
//
// draft_to_meta splits "<n>/<path>" into recipe.setup_ref, which names the
// disc, and recipe.setup, which is the path on it. Reading the path back on its
// own is how an installer that lives on disc 2 - an expansion on a
// compilation's second disc, anything whose setup is not on the first disc -
// gets run off disc 1 instead, quietly and four steps later. So the number is
// found again here, from the disc the reference names.
//
// The path alone when nothing names a disc, which is every one-disc game and
// every installer_exe: those record an absolute path on this filesystem and no
// disc at all.
std::filesystem::path setup_from_recipe(const Meta& m);

}  // namespace kg::install
