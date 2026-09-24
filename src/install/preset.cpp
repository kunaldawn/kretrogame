#include "preset.h"

#include <algorithm>
#include <exception>

#include "../util/text.h"
#include "discs.h"
#include "manifest.h"
#include "setup_ref.h"

namespace kg::install {
namespace fs = std::filesystem;

namespace {

// Identity, at whatever strength is available without hashing gigabytes.
// disc::open scans with full=false, so info.whole is normally
// absent at step 2 and the size-and-volume-label pair is as strong as this can
// honestly get; the whole hash is used when a recipe import has already
// produced one.
bool preset_same_disc(const DiscFingerprint& want, const disc::Disc& got) {
  if (!want.size || want.size != got.info.size) return false;
  if (got.info.has_whole) return want.blake3 == got.info.whole;
  return !want.volume_id.empty() && iequals(want.volume_id, got.info.volume_id);
}

}  // namespace

Preset score_preset(const Meta& m, const fs::path& from, const std::vector<disc::Disc>& discs) {
  Preset p;
  p.id = m.id;
  p.name = m.name;
  p.manifest = from;
  p.of = m.recipe.discs.size();
  // installer_exe names no disc at all - the game is a bare downloaded
  // executable - so there is nothing here to recognise it by.
  if (m.recipe.discs.empty()) return p;

  for (size_t i = 0; i < m.recipe.discs.size(); ++i) {
    DiscRef r = parse_disc_ref(m.recipe.discs[i]);
    bool hit = false;
    if (i < m.recipe.fingerprints.size()) {
      for (const disc::Disc& d : discs) {
        if (preset_same_disc(m.recipe.fingerprints[i], d)) {
          hit = true;
          p.by_fingerprint = true;
          break;
        }
      }
    }
    if (!hit) {
      for (const disc::Disc& d : discs) {
        // A label is the stable handle a manifest uses; a bare reference means
        // the whole archive, and then the filename is all there is to go on.
        if (!r.label.empty() && iequals(r.label, d.label)) { hit = true; break; }
        if (r.label.empty() && !r.archive.empty() &&
            iequals(r.archive, d.source.filename().string())) { hit = true; break; }
      }
    }
    if (hit) ++p.matched;
  }
  return p;
}

std::vector<Preset> match_presets(const rt::Env& e, const std::vector<disc::Disc>& discs) {
  std::vector<Preset> out;
  if (discs.empty()) return out;
  for (const std::string& id : known_games(e)) {
    fs::path mp = find_manifest(e, id);
    if (mp.empty()) continue;
    try {
      Preset p = score_preset(load_manifest(mp), mp, discs);
      if (p.matched) out.push_back(p);
    } catch (const std::exception&) {
      // A manifest that will not parse is a broken file on this machine, not a
      // reason to stop offering the others that do.
    }
  }
  // Identity first, then how much of the manifest is accounted for, then how
  // little of it is missing: a compilation's manifests may each name one disc
  // of the same zip, and the one whose disc is actually here should be the
  // offer.
  std::sort(out.begin(), out.end(), [](const Preset& a, const Preset& b) {
    if (a.by_fingerprint != b.by_fingerprint) return a.by_fingerprint;
    if (a.matched != b.matched) return a.matched > b.matched;
    if (a.of - a.matched != b.of - b.matched) return a.of - a.matched < b.of - b.matched;
    return a.id < b.id;
  });
  return out;
}

void apply_preset(const Meta& m, Draft* d) {
  if (!d) return;
  d->id = m.id;
  d->name = m.name;
  d->year = m.year;
  if (!m.recipe.setup.empty()) d->setup = setup_from_recipe(m);
  // The method, and the two fields that pick which build on the disc. Without
  // these a preset for an unzip title - method="unzip", member="Data2.zip",
  // subdir="Example Game 1.02" - is accepted as an installer disc with nothing
  // named, which is not a worse prefill but a wrong one: the manifest exists to
  // say the compilation disc carries several games as zips and which of them
  // this is. A manifest that names no method leaves the draft's alone.
  if (std::optional<Draft::Method> mm = method_from_recipe(m.recipe.method)) d->method = *mm;
  d->member = m.recipe.member;
  d->subdir = m.recipe.subdir;
  d->exe = m.run.exe;
  d->args = m.run.args;
  if (m.run.width) d->width = m.run.width;
  if (m.run.height) d->height = m.run.height;
  if (!m.run.windows_version.empty()) d->windows_version = m.run.windows_version;
  d->dgvoodoo = m.runtime.dgvoodoo;
}

}  // namespace kg::install
