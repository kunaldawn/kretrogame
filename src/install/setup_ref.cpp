#include "setup_ref.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#include "../util/text.h"
#include "staging.h"

namespace kg::install {
namespace fs = std::filesystem;

SetupRef split_setup_ref(const fs::path& setup) {
  SetupRef r;
  r.path = setup.generic_string();
  std::replace(r.path.begin(), r.path.end(), '\\', '/');
  const size_t slash = r.path.find('/');
  // A leading run of digits and nothing else. "2/Game3/Setup.exe" is disc two
  // and a path two components long; "Game3/Setup.exe" is a path two
  // components long on whichever disc has it, and peeling "Game3" off it
  // would run a Setup.exe that is not there.
  if (slash == std::string::npos || slash == 0) return r;
  if (r.path.find_first_not_of("0123456789") < slash) return r;
  r.disc = static_cast<size_t>(std::strtoul(r.path.substr(0, slash).c_str(), nullptr, 10));
  r.path = r.path.substr(slash + 1);
  return r;
}

bool setup_path_is_safe(const std::string& setup) {
  if (setup.empty() || setup.size() > 1024) return false;
  std::string s = setup;
  std::replace(s.begin(), s.end(), '\\', '/');
  fs::path p(s);
  if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
  for (const fs::path& part : p) {
    if (part == "..") return false;
  }
  return true;
}

fs::path staged_setup(const fs::path& work, const Draft& d) {
  // The guard sits here because this is the wizard's road to a setup: it
  // reaches it from start_the_install, and the GUI's "rebuild it from your
  // disc" button walks a stranger's recipe down it without passing
  // install::run at all. The CLI's install::run does not come this way - it
  // resolves recipe.setup under the mounted disc with resolve_ci - so refusing
  // in install::run alone would be refusing on one road of two.
  if (d.method == Draft::Method::InstallerExe) {
    // A path the person picked in step 1 is theirs and is absolute by nature.
    // One that arrived inside a pack is not, and install::run already refuses
    // it there; draft_from_meta clears it before it can reach here.
    return d.setup;
  }
  if (!setup_path_is_safe(d.setup.string())) {
    throw std::runtime_error(
        "this recipe names " + d.setup.string() +
        " as its installer, which is not a path on the disc it came with.\n"
        "  A pack only gets to name a file on its own discs.");
  }
  SetupRef r = split_setup_ref(d.setup);
  // Disc one when the form named none: a one-disc game's manifest writes the
  // path alone and the only disc there is is in D:.
  const size_t n = r.disc ? r.disc : 1;
  return staging_drive(work, n - 1) / r.path;
}

size_t setup_index(const std::vector<SetupChoice>& setups, const fs::path& setup) {
  // The wizard's own form, taken apart once: n is which disc, 1-based, and
  // zero means "whichever disc has it" - which is what a manifest, writing the
  // path alone, is saying.
  const SetupRef ref = split_setup_ref(setup);
  const std::string& want = ref.path;
  const size_t which = ref.disc;
  if (want.empty()) return setups.size();

  for (size_t i = 0; i < setups.size(); ++i) {
    if (which && setups[i].first + 1 != which) continue;
    std::string have = setups[i].second.generic_string();
    std::replace(have.begin(), have.end(), '\\', '/');
    if (to_lower(have) == to_lower(want)) return i;
  }
  return setups.size();
}

fs::path setup_from_recipe(const Meta& m) {
  // A disc spells its own separators, and a manifest copies whatever the disc
  // said. The wizard's form is joined onto a mount point on this filesystem,
  // so it is '/' from here on.
  std::string path = m.recipe.setup;
  std::replace(path.begin(), path.end(), '\\', '/');
  if (path.empty() || m.recipe.setup_ref.empty()) return path;
  // installer_exe records an absolute path to a file somebody downloaded and
  // names no disc; a setup_ref beside it would be a leftover, not an answer.
  if (m.recipe.method == kMethodInstallerExe) return path;

  // Which disc, as its place in the recipe's own list counting from 1: that is
  // the order draft_to_meta numbered against and the order open_sources will
  // assemble the set in, so the two forms are each other's inverse.
  for (size_t i = 0; i < m.recipe.discs.size(); ++i) {
    if (to_lower(m.recipe.discs[i]) == to_lower(m.recipe.setup_ref)) {
      return fs::path(std::to_string(i + 1)) / path;
    }
  }
  // A pack carries its discs twice - the recipe's references and the bodies
  // beside them - and an older one may only have the second list.
  for (size_t i = 0; i < m.discs.size(); ++i) {
    if (to_lower(m.discs[i].ref) == to_lower(m.recipe.setup_ref)) {
      return fs::path(std::to_string(i + 1)) / path;
    }
  }
  // A reference to a disc this recipe does not list. The path alone is what
  // was there before, and step 3 opens on whichever disc carries it.
  return path;
}

}  // namespace kg::install
