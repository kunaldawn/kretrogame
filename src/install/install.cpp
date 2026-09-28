#include "install.h"

#include "../disc/disc.h"
#include "../disc/iso.h"
#include "build.h"
#include "collection.h"
#include "discs.h"
#include "draft.h"
#include "keys.h"
#include "staging.h"
#include "survey.h"

#include <stdexcept>
#include <system_error>

#include "../util/fs_ci.h"
#include "../util/hash.h"
#include "../util/paths.h"
#include "../wine/registry.h"

namespace kg::install {
namespace fs = std::filesystem;

namespace {

// The file a wine_setup or installer_exe recipe runs: the installer in the
// collection, or the setup program on the disc its reference picks, found
// case-insensitively in that disc's staged drive under `work`.
fs::path recipe_setup(const Meta& m, const std::vector<disc::Disc>& media, const fs::path& work) {
  fs::path setup;
  if (m.recipe.method == kMethodInstallerExe) {
    // This is the one method whose recipe names a file to execute rather than
    // a disc to read, and a recipe can arrive from anybody: share.cpp's
    // import_pack hands a stranger's Meta straight to this function. The
    // installer has to be in the collection, so that what runs is a file its
    // owner put there.
    if (!is_collection_name(m.recipe.setup)) {
      throw std::runtime_error("this recipe's installer, " + m.recipe.setup +
                               ", is not in " + iso_dir().string() +
                               ".\n  A pack only gets to name a file in the collection; put "
                               "the installer there and try again.");
    }
    setup = find_iso(m.recipe.setup);
    if (setup.empty()) throw std::runtime_error("no installer called " + m.recipe.setup);
    // find_iso answers relative to the collection directory, and we run the
    // installer from its own folder - so a relative path would be resolved
    // twice and Wine would look for iso/iso/Setup.exe.
    setup = fs::absolute(setup);
  } else {
    DiscRef sr = parse_disc_ref(m.recipe.setup_ref.empty()
                                    ? (m.recipe.discs.empty() ? "" : m.recipe.discs[0])
                                    : m.recipe.setup_ref);
    int which = pick_disc(media, sr);
    if (which < 0) which = 0;
    fs::path drive = staging_drive(work, static_cast<size_t>(which));
    setup = resolve_ci(drive, m.recipe.setup);
    if (setup.empty()) {
      throw std::runtime_error(media[static_cast<size_t>(which)].label + " has no " +
                               m.recipe.setup);
    }
  }
  return setup;
}

}  // namespace

Result run(const rt::Env& e, Meta m, const Options& opt,
           const std::function<void(const std::string&)>& say) {
  if (!e.valid()) throw std::runtime_error("no runtime; install needs the bundled tools");
  ensure_state_dirs();

  fs::path out = game_pack(m.id);
  std::error_code ec;
  if (fs::exists(out, ec) && !opt.force) {
    throw std::runtime_error(m.name + " is already installed. Use --force to reinstall.");
  }

  // This is not the GUI's install path. `kretro install <id>` opens the
  // wizard; what is here is rebuilding a recipe - share.cpp's import_pack
  // calls it - and being the thing tests/unit/test_install.cpp can drive with no
  // display. It runs the same Build the wizard does, one step at a time, and
  // supplies from the manifest every answer the wizard would have asked for.
  Build b(e, staging_dir(m.id), say);
  b.keep_tree = opt.keep_tree;

  // A recipe that came from someone else names the disc as *they* had it filed.
  // Where the name does not resolve, fall back to matching by fingerprint, so
  // the same pressing under a different filename is still recognised. The
  // label or index part of the reference is kept either way.
  std::vector<std::string> refs = m.recipe.discs;
  for (size_t i = 0; i < refs.size(); ++i) {
    DiscRef r = parse_disc_ref(refs[i]);
    if (!find_iso(r.archive).empty()) continue;
    if (i >= m.recipe.fingerprints.size()) continue;
    fs::path p = find_iso_by_fingerprint(m.recipe.fingerprints[i]);
    if (p.empty()) continue;
    std::string suffix;
    if (!r.label.empty()) suffix = "#" + r.label;
    else if (r.index > 0) suffix = "#" + std::to_string(r.index);
    refs[i] = p.filename().string() + suffix;
  }

  // resolve_discs, not open_sources: a manifest names one disc *inside* an
  // archive by its volume label, which a list of whole files cannot say.
  std::vector<disc::Disc> media = resolve_discs(e, refs, staging_dir(m.id) / "media", say);
  if (media.empty() && m.recipe.method != kMethodInstallerExe) {
    throw std::runtime_error(m.id + ": the manifest names no disc");
  }

  if (!media.empty()) say("identifying " + media[0].label);
  m.recipe.fingerprints.clear();
  for (const disc::Disc& d : media) {
    iso::Info info = iso::scan(d.iso, /*full=*/true);
    if (!info.valid_iso9660) say("  warning: " + d.label + " has no ISO 9660 volume descriptor");
    say("  " + d.label + "  " + to_hex(info.whole).substr(0, 16));
    m.recipe.fingerprints.push_back(iso::fingerprint(info));
  }
  b.adopt_discs(media);

  if (m.recipe.method == kMethodCopy) {
    b.copy_from_disc(m.recipe.subdir);
  } else if (m.recipe.method == kMethodUnzip) {
    b.unzip_from_disc(m.recipe.member, m.recipe.subdir);
  } else if (m.recipe.method == kMethodWineSetup || m.recipe.method == kMethodInstallerExe) {
    b.prepare_prefix(m.run.windows_version);
    b.mount_discs();
    m.discs = b.drives();

    // With no auto-swap, a person changing discs mid-install is the normal
    // case rather than the fallback, so this is said every time.
    if (media.size() > 1) {
      say("  This set has " + std::to_string(media.size()) +
          " discs and they are all mounted at once, so most installers");
      say("  never ask. If this one does, run in another terminal:");
      for (size_t i = 0; i < media.size(); ++i) {
        say("    kretro swap " + m.id + " " + std::to_string(i + 1) + "    # " + media[i].label);
      }
    }

    fs::path setup = recipe_setup(m, media, staging_dir(m.id));

    std::string key = key_for(load_keys(keys_file()), m.id);
    if (!key.empty()) say("  a stored serial is available: " + key);

    b.snapshot_before();
    b.run_setup(setup, /*headless=*/false, [](const std::string&) {});
    b.diff_after();

    // Exactly detect_install_dir's rule, over a list that is now ranked rather
    // than sorted on the spot: the deepest directory holding every verify
    // entry at once. It fails the same way when none does.
    fs::path drive_c = staging_drive_c(staging_dir(m.id));
    fs::path installed = find_install_dir(b.candidates(), drive_c, m.recipe.verify);
    if (installed.empty()) {
      throw std::runtime_error(
          "cannot tell where the game was installed: nothing under C: holds " +
          (m.recipe.verify.empty() ? m.run.exe : m.recipe.verify[0]));
    }
    m.install.install_dir = installed.generic_string();
    say("  installed to C:\\" + m.install.install_dir);
  } else {
    throw std::runtime_error("unknown install method: " + m.recipe.method);
  }

  // The staging branch filled m.discs from the drive letters. Copy and unzip
  // mounted nothing, and their packs carry their discs too - write() lays out
  // the discs m.discs names, so an empty list here would be a pack that
  // admits to no disc.
  if (m.discs.empty()) {
    for (size_t i = 0; i < media.size(); ++i) {
      m.discs.push_back(
          disc_entry(media[i], i < m.recipe.discs.size() ? m.recipe.discs[i] : disc_ref_for(media[i])));
    }
  }

  Result res = b.write(m);

  if (opt.expect_root_set) {
    // Advisory: two people clicking through InstallShield need
    // not produce the same bytes, so a mismatch is reported rather than
    // refused, and root_matched says which happened.
    res.root_matched = res.root == opt.expect_root;
    if (res.root_matched) say("the tree matches the recipe");
    else say("the tree differs from the recipe: " + to_hex(res.root) + " not " +
             to_hex(opt.expect_root));
  }
  return res;
}

}  // namespace kg::install
