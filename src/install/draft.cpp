#include "draft.h"

#include <algorithm>

#include "../disc/iso.h"
#include "discs.h"
#include "setup_ref.h"

namespace kg::install {
namespace fs = std::filesystem;

std::string_view recipe_method(Draft::Method m) {
  switch (m) {
    case Draft::Method::Installer: return kMethodWineSetup;
    case Draft::Method::InstallerExe: return kMethodInstallerExe;
    case Draft::Method::Copy: return kMethodCopy;
    case Draft::Method::Unzip: return kMethodUnzip;
  }
  return kMethodWineSetup;
}

std::optional<Draft::Method> method_from_recipe(std::string_view s) {
  // The four strings install.cpp's method branch accepts, and the four
  // Draft::Method values they became.
  if (s.empty()) return std::nullopt;
  if (s == kMethodCopy) return Draft::Method::Copy;
  if (s == kMethodUnzip) return Draft::Method::Unzip;
  if (s == kMethodInstallerExe) return Draft::Method::InstallerExe;
  return Draft::Method::Installer;
}

Meta draft_to_meta(const Draft& d, const std::vector<disc::Disc>& discs) {
  Meta m;
  m.id = d.id;
  m.name = d.name;
  m.year = d.year;

  m.recipe.method = std::string(recipe_method(d.method));
  m.recipe.member = d.member;
  m.recipe.subdir = d.subdir;
  m.recipe.verify = d.verify;

  m.run.exe = d.exe.generic_string();
  m.run.args = d.args;
  m.run.width = d.width;
  m.run.height = d.height;
  m.run.windows_version = d.windows_version;
  m.runtime.dgvoodoo = d.dgvoodoo;
  m.install.install_dir = d.install_dir.generic_string();

  for (const disc::Disc& disc_ : discs) {
    Meta::Disc e = disc_entry(disc_, disc_ref_for(disc_));
    m.discs.push_back(e);
    m.recipe.discs.push_back(e.ref);
    // What the disc *is*, as against what it is called. install::run has
    // recorded this since the format existed and the wizard never did, so every
    // pack the wizard wrote came out with an empty recipe.fingerprints:
    // `kretro export <id> --recipe` refused it outright, and a recipient whose
    // copy of the disc is filed under a different name had nothing to resolve
    // it by. It also silently cost the unzip anchor, which write() only records
    // when there is a fingerprint to hang it on.
    //
    // The hash is the one disc::open already read - the first 64 MB rather than
    // the whole image - because that is what the wizard has in hand and
    // find_iso_by_fingerprint accepts either.
    m.recipe.fingerprints.push_back(iso::fingerprint(disc_.info));
  }

  if (d.method == Draft::Method::Installer) {
    // Draft::setup is "<n>/<path on that disc>" - which exe, on which disc.
    // Here that becomes the two fields the engine reads: a disc reference and
    // a path within it.
    // Through split_setup_ref, not a second hand-rolled parse of the same
    // form. Reading any text before the first slash as a number would store
    // "3dfx/Setup.exe" - a real directory name on a real disc - as disc three
    // with the path truncated to Setup.exe, and bake the truncation into the
    // pack rather than merely act on it once. split_setup_ref peels a leading
    // component only when it is all digits.
    const SetupRef r = split_setup_ref(d.setup);
    if (r.disc >= 1 && r.disc <= m.discs.size()) {
      m.recipe.setup_ref = m.discs[r.disc - 1].ref;
      m.recipe.setup = r.path;
    } else {
      m.recipe.setup = d.setup.generic_string();
    }
  } else if (d.method == Draft::Method::InstallerExe) {
    // Absolute: this is the file that was actually run, recorded as what the
    // pack was made from. It is a record and not an instruction to whoever
    // opens the pack - find_iso answers only inside the collection directory,
    // so a rebuild from a shared recipe looks for the installer in iso_dir()
    // and says plainly that it is not there. A recipe must not be able to name
    // any file on the importer's machine and have Wine run it.
    std::error_code ec;
    m.recipe.setup = fs::absolute(d.setup, ec).lexically_normal().string();
  }

  // d.serial is not copied anywhere. It goes to the vault under d.id and into
  // no pack, which is the whole of the policy at the top of keys.h.
  return m;
}

Prefill draft_from_meta(const Meta& m) {
  // find_iso and find_iso_by_fingerprint read the collection directory out of
  // the environment rather than being handed one, so nothing below needs a
  // runtime.
  Prefill p;
  p.draft.id = m.id;
  p.draft.name = m.name.empty() ? m.id : m.name;
  p.draft.year = m.year;
  p.draft.setup = setup_from_recipe(m);
  // Everything above this line came out of a file somebody sent. For
  // installer_exe the setup is a path to a program that will be run under
  // Wine, and setup_from_recipe hands it back verbatim because the wizard's
  // own bare-.exe source legitimately holds an absolute path there. A pack's
  // does not get that privilege: install::run refuses one that is not in the
  // collection, and the wizard's rebuild button never goes through
  // install::run, so the same refusal has to happen where the pack is read.
  // Cleared rather than thrown on, because the rest of the prefill is still
  // worth having - step 3 will ask which installer, which is the right
  // question to be asked about a recipe that named a file you do not have.
  if (!setup_path_is_safe(p.draft.setup.string())) {
    p.draft.setup.clear();
    p.unsafe_setup = m.recipe.setup;
  }
  p.draft.member = m.recipe.member;
  p.draft.subdir = m.recipe.subdir;
  p.draft.exe = m.run.exe;
  p.draft.args = m.run.args;
  if (m.run.width) p.draft.width = m.run.width;
  if (m.run.height) p.draft.height = m.run.height;
  if (!m.run.windows_version.empty()) p.draft.windows_version = m.run.windows_version;
  p.draft.dgvoodoo = m.runtime.dgvoodoo;

  // An empty method and one this build does not know both open as an
  // installer disc, which is what the wizard would have offered anyway.
  p.draft.method = method_from_recipe(m.recipe.method).value_or(Draft::Method::Installer);

  for (size_t i = 0; i < m.recipe.discs.size(); ++i) {
    DiscRef r = parse_disc_ref(m.recipe.discs[i]);
    fs::path found = locate_disc(r, m.recipe.fingerprints, i);
    if (found.empty()) {
      p.missing.push_back(m.recipe.discs[i]);
      continue;
    }
    if (std::find(p.draft.sources.begin(), p.draft.sources.end(), found) ==
        p.draft.sources.end()) {
      p.draft.sources.push_back(found);
    }
  }
  return p;
}

bool prefill_serial(Draft& d, const std::vector<StoredKey>& keys) {
  if (!d.serial.empty() || d.id.empty()) return false;
  std::string known = key_for(keys, d.id);
  if (known.empty()) return false;
  d.serial = known;
  return true;
}

}  // namespace kg::install
