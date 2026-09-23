#include "build.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <utility>

#include "../disc/drive.h"
#include "../session/session.h"
#include "../util/paths.h"
#include "discs.h"
#include "iso.h"
#include "keys.h"

namespace kg::install {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

Build::Source Build::classify(const fs::path& p) {
  Source s;
  s.path = p;
  s.kind = Source::Kind::Unreadable;
  std::error_code ec;
  if (!fs::exists(p, ec)) {
    s.trouble = "there is no such file";
    return s;
  }
  // A directory is a mounted CD or a disc somebody already extracted. It is
  // the one kind probe_into (container.cpp:117-132) returns nothing for, and
  // the one kind that costs nothing to accept.
  if (fs::is_directory(p, ec)) {
    s.kind = Source::Kind::Directory;
    return s;
  }
  if (!fs::is_regular_file(p, ec)) {
    s.trouble = "not a file or a directory";
    return s;
  }
  // By name only. This runs as the user adds files, and opening a 6.8 GB DVD
  // to find out what it is would make the list stutter for every drop.
  std::string name = p.filename().string();
  if (disc::is_disc_image(name)) {
    s.kind = Source::Kind::DiscImage;
    return s;
  }
  if (disc::is_archive(name)) {
    s.kind = Source::Kind::Archive;
    return s;
  }
  if (lower(p.extension().string()) == ".exe") {
    s.kind = Source::Kind::BareExe;
    return s;
  }
  s.trouble = name + " is not a disc image, an archive or an installer";
  return s;
}

fs::path staging_dir(const std::string& id) { return cache_dir() / ("install-" + id); }

std::string slug(std::string_view name) {
  // The same rule disc::set_id_from applies to a disc set's name, and
  // deliberately so: a set assembled from "Example Game (USA)" and the game id
  // derived from the same title have to agree, or the two halves of the
  // wizard would disagree about what this game is called.
  return disc::set_id_from(name);
}

std::string protection_of(std::string_view filename) {
  // SafeDisc and SecuROM read raw sectors below the filesystem. These are the
  // files each leaves beside the executable it wraps.
  static const struct { const char* file; const char* what; } kSigns[] = {
      {"drvmgt.dll", "SafeDisc"},
      {"secdrv.sys", "SafeDisc"},
      {"cmdlineext.dll", "SecuROM"},
      {"sintf32.dll", "SecuROM"},
  };
  std::string n = lower(std::string(filename));
  for (const auto& s : kSigns) {
    if (n == s.file) return s.what;
  }
  return {};
}

namespace {

// Lower sorts earlier. The numbers are spaced so a later tier can be inserted
// without renumbering the ones around it.
int exe_tier(const std::string& filename, bool prefer_setup) {
  std::string n = lower(filename);
  // An uninstaller is never what runs the game, and it is frequently the
  // largest executable in the directory - InstallShield's is a megabyte - so
  // this has to beat the size rule outright rather than nudge it.
  if (n.rfind("unins", 0) == 0 || n.find("uninstall") != std::string::npos) return 90;
  if (prefer_setup) {
    if (n.rfind("setup", 0) == 0) return 0;
    if (n.rfind("install", 0) == 0) return 10;
    if (n.rfind("autorun", 0) == 0) return 20;
  }
  return 50;
}

}  // namespace

std::vector<fs::path> rank_executables(const fs::path& dir, bool prefer_setup) {
  struct Row {
    fs::path rel;
    uint64_t size = 0;
    int tier = 0;
  };
  std::vector<Row> rows;
  std::error_code ec;

  auto consider = [&](const fs::directory_entry& de, const fs::path& rel) {
    std::error_code e2;
    if (!de.is_regular_file(e2)) return;
    if (lower(de.path().extension().string()) != ".exe") return;
    rows.push_back(Row{rel, de.file_size(e2), exe_tier(de.path().filename().string(),
                                                       prefer_setup)});
  };

  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    consider(de, de.path().filename());
    // The disc root and one directory down, and no further. A disc carrying
    // several games may keep each one's setup a directory in; nothing real is
    // two directories in, and a full walk of a 700 MB disc for a list nobody
    // reads to the bottom is not free.
    if (!prefer_setup) continue;
    std::error_code e2;
    if (!de.is_directory(e2)) continue;
    for (const fs::directory_entry& sub : fs::directory_iterator(de.path(), e2)) {
      consider(sub, de.path().filename() / sub.path().filename());
    }
  }

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.tier != b.tier) return a.tier < b.tier;
    if (a.size != b.size) return a.size > b.size;
    return a.rel.generic_string() < b.rel.generic_string();
  });

  std::vector<fs::path> out;
  out.reserve(rows.size());
  for (const Row& r : rows) out.push_back(r.rel);
  return out;
}

namespace {

size_t depth_of(const fs::path& p) {
  size_t n = 0;
  for (auto it = p.begin(); it != p.end(); ++it) ++n;
  return n;
}

}  // namespace

std::vector<Build::Candidate> rank_candidates(const fs::path& drive_c, const Tree::Diff& d) {
  // Every ancestor of every added file, not only the file's own directory.
  // An installer writes into Program Files\Publisher\Game, and the person
  // choosing has to be able to see the publisher directory as a choice - the
  // game is sometimes one level up from where the .exe landed.
  std::map<std::string, Build::Candidate> by_dir;
  for (const TreeEntry& te : d.added) {
    if (te.is_dir()) continue;
    fs::path dir = fs::path(te.path).parent_path();
    while (!dir.empty()) {
      Build::Candidate& c = by_dir[dir.generic_string()];
      c.dir = dir;
      ++c.files;
      c.bytes += te.size;
      dir = dir.parent_path();
    }
  }

  std::vector<Build::Candidate> out;
  out.reserve(by_dir.size());
  for (auto& kv : by_dir) out.push_back(std::move(kv.second));

  // Deepest first, then by file count. The deepest directory is not always the
  // game's - a save subdirectory the installer pre-created is deeper still -
  // which is precisely why this is a ranking shown to a person rather than an
  // answer. detect_install_dir had to guess; the wizard asks.
  std::sort(out.begin(), out.end(), [](const Build::Candidate& a, const Build::Candidate& b) {
    size_t da = depth_of(a.dir), db = depth_of(b.dir);
    if (da != db) return da > db;
    if (a.files != b.files) return a.files > b.files;
    return a.dir.generic_string() < b.dir.generic_string();
  });

  for (Build::Candidate& c : out) c.executables = rank_executables(drive_c / c.dir, false);
  return out;
}

Build::Candidate survey_tree(const fs::path& tree) {
  Build::Candidate c;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::recursive_directory_iterator(tree, ec)) {
    std::error_code e2;
    if (!de.is_regular_file(e2)) continue;
    ++c.files;
    c.bytes += de.file_size(e2);
  }
  // `dir` stays empty. Everything that reads a Candidate joins it onto
  // installed_root(), and for a copy that join is the tree itself.
  c.executables = rank_executables(tree, false);
  return c;
}

size_t exe_index(const std::vector<fs::path>& exes, const fs::path& want) {
  std::string w = want.generic_string();
  std::replace(w.begin(), w.end(), '\\', '/');
  if (w.empty()) return exes.size();
  // A manifest names the executable, not a path to it, so the comparison is on
  // the filename either way. Case-insensitively: a disc spells its own names
  // however it likes and Game.exe arrives as GAME.EXE.
  w = lower(fs::path(w).filename().string());
  for (size_t i = 0; i < exes.size(); ++i) {
    if (lower(exes[i].filename().string()) == w) return i;
  }
  return exes.size();
}

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
  // The guard sits here because this is the single road to a setup: the wizard
  // reaches it from start_the_install and the CLI from install::run, and the
  // GUI's "rebuild it from your disc" button walks a stranger's recipe down it
  // without passing install::run at all. Refusing in one of the two callers is
  // refusing in neither.
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
  return work / (std::string("drive-") + static_cast<char>('d' + n - 1)) / r.path;
}

Draft::Method clamp_method(const std::vector<Draft::Method>& offered, Draft::Method want) {
  if (offered.empty()) return want;
  if (std::find(offered.begin(), offered.end(), want) != offered.end()) return want;
  // methods_for lists them in the order the page offers them, so the first is
  // what the combo would have shown anyway.
  return offered.front();
}

size_t setup_index(const std::vector<SetupChoice>& setups, const fs::path& setup) {
  // The wizard's own form, taken apart once: n is which disc, 1-based, and
  // zero means "whichever disc has it" - which is what a manifest, writing the
  // path alone, is saying.
  const SetupRef ref = split_setup_ref(setup);
  const std::string want = ref.path;
  const size_t which = ref.disc;
  if (want.empty()) return setups.size();

  for (size_t i = 0; i < setups.size(); ++i) {
    if (which && setups[i].first + 1 != which) continue;
    std::string have = setups[i].second.generic_string();
    std::replace(have.begin(), have.end(), '\\', '/');
    if (lower(have) == lower(want)) return i;
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
  if (m.recipe.method == "installer_exe") return path;

  // Which disc, as its place in the recipe's own list counting from 1: that is
  // the order draft_to_meta numbered against and the order open_sources will
  // assemble the set in, so the two forms are each other's inverse.
  for (size_t i = 0; i < m.recipe.discs.size(); ++i) {
    if (lower(m.recipe.discs[i]) == lower(m.recipe.setup_ref)) {
      return fs::path(std::to_string(i + 1)) / path;
    }
  }
  // A pack carries its discs twice - the recipe's references and the bodies
  // beside them - and an older one may only have the second list.
  for (size_t i = 0; i < m.discs.size(); ++i) {
    if (lower(m.discs[i].ref) == lower(m.recipe.setup_ref)) {
      return fs::path(std::to_string(i + 1)) / path;
    }
  }
  // A reference to a disc this recipe does not list. The path alone is what
  // was there before, and step 3 opens on whichever disc carries it.
  return path;
}

fs::path bare_exe(const std::vector<Build::Source>& sources) {
  for (const Build::Source& s : sources) {
    if (s.kind == Build::Source::Kind::BareExe) return s.path;
  }
  return {};
}

bool sources_are_enough(const std::vector<Build::Source>& sources, size_t discs) {
  return discs > 0 || !bare_exe(sources).empty();
}

std::vector<Draft::Method> methods_for(const std::vector<Build::Source>& sources, size_t discs) {
  std::vector<Draft::Method> out;
  bool bare = !bare_exe(sources).empty();
  // With neither a disc nor an .exe there is still a page to draw, and what it
  // should say is that it wants a disc.
  if (discs > 0 || !bare) out.push_back(Draft::Method::Installer);
  if (discs > 0) {
    out.push_back(Draft::Method::Copy);
    out.push_back(Draft::Method::Unzip);
  }
  if (bare) out.push_back(Draft::Method::InstallerExe);
  return out;
}

fs::path staging_drive_c(const fs::path& work) { return work / "prefix" / "drive_c"; }

std::vector<std::string> verify_list(const fs::path& dir, const fs::path& exe) {
  std::vector<std::string> out;
  std::string exe_name = exe.filename().string();
  if (!exe_name.empty()) out.push_back(exe_name);

  struct Row {
    std::string name;
    uint64_t size = 0;
  };
  std::vector<Row> rows;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    std::error_code e2;
    // Top level only, and no directories. A verify entry with a separator in
    // it is a claim about the shape of the tree as well as its contents, and
    // the shape is the part an installer is free to change between releases.
    if (!de.is_regular_file(e2)) continue;
    std::string n = de.path().filename().string();
    if (n == exe_name) continue;
    rows.push_back(Row{n, de.file_size(e2)});
  }
  // Largest first: the big immovable assets are what a different release of
  // the same game keeps, and a 40 KB readme is what it changes.
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.size != b.size) return a.size > b.size;
    return a.name < b.name;
  });

  for (const Row& r : rows) {
    if (out.size() >= 5) break;
    out.push_back(r.name);
  }
  return out;
}

std::vector<std::string> verify_for(const Draft& d, const fs::path& installed_root) {
  // Whatever step 6 read off the directory the user confirmed, when it read
  // one. Copy and unzip put the game at the staging tree and never touch
  // drive_c, so the fallback reads installed_root - whichever of the two the
  // method used - joined onto install_dir, which for a copy is empty because
  // there the tree is the game.
  if (!d.verify.empty()) return d.verify;
  return verify_list(installed_root / d.install_dir, d.exe);
}

fs::path resolve_path_ci_public(const fs::path& root, const std::string& want) {
  fs::path at = root;
  std::error_code ec;
  size_t start = 0;
  while (start <= want.size()) {
    size_t slash = want.find_first_of("/\\", start);
    std::string part = want.substr(start, slash == std::string::npos ? std::string::npos
                                                                     : slash - start);
    if (!part.empty()) {
      fs::path found;
      for (const fs::directory_entry& de : fs::directory_iterator(at, ec)) {
        if (lower(de.path().filename().string()) == lower(part)) { found = de.path(); break; }
      }
      if (found.empty()) return {};
      at = found;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return at;
}

std::string disc_ref_for(const disc::Disc& d) {
  // The form both resolvers already parse (discs.cpp:218). The archive is
  // named without its directory because that is how a manifest names it and
  // how find_iso looks for it; the absolute path travels beside it in
  // Meta::Disc::source for when the name does not resolve.
  std::string archive = d.source.filename().string();
  if (d.label.empty()) return archive;
  return archive + "#" + d.label;
}

namespace {

const char* method_name(Draft::Method m) {
  switch (m) {
    case Draft::Method::Installer: return "wine_setup";
    case Draft::Method::InstallerExe: return "installer_exe";
    case Draft::Method::Copy: return "copy";
    case Draft::Method::Unzip: return "unzip";
  }
  return "wine_setup";
}

}  // namespace

Meta draft_to_meta(const Draft& d, const std::vector<disc::Disc>& discs) {
  Meta m;
  m.id = d.id;
  m.name = d.name;
  m.year = d.year;

  m.recipe.method = method_name(d.method);
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
  // Everything the wizard writes carries its discs and its registry, so the
  // body has the rooted layout Part 2.1 describes.
  m.layout = "rooted";

  for (const disc::Disc& disc_ : discs) {
    Meta::Disc e;
    e.label = disc_.label;
    e.serial = disc_.serial;
    e.ref = disc_ref_for(disc_);
    std::error_code ec;
    e.source = fs::absolute(disc_.source, ec).lexically_normal().string();
    e.embedded = d.embed_discs;
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
    // form. The one that used to live here read any text before the first
    // slash as a number, so "3dfx/Setup.exe" - a real directory name on a real
    // disc - was stored as disc three with the path truncated to Setup.exe,
    // and the truncation was baked into the pack rather than merely acted on
    // once. split_setup_ref peels a leading component only when it is all
    // digits, which is the rule this always meant.
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
    // opens the pack - find_iso answers only inside the collection directory
    // now, so a rebuild from a shared recipe looks for the installer in
    // iso_dir() and says plainly that it is not there. A recipe used to be
    // able to name any file on the importer's machine and have Wine run it.
    std::error_code ec;
    m.recipe.setup = fs::absolute(d.setup, ec).lexically_normal().string();
  }

  // d.serial is not copied anywhere. It goes to the vault under d.id and into
  // no pack, which is the whole of keys.h:1-7.
  return m;
}

Build::Build(const rt::Env& e, fs::path work, Say say)
    : env_(e), work_(std::move(work)), say_(std::move(say)) {
  std::error_code ec;
  // Whatever is there is from an install that did not finish. Building on top
  // of a half-extracted drive tree would mount a disc that is missing files
  // and blame the disc for it.
  fs::remove_all(work_, ec);
  prefix_ = work_ / "prefix";
  home_ = work_ / "home";
  tree_dir_ = work_ / "tree";
  fs::create_directories(prefix_, ec);
  fs::create_directories(home_ / ".config", ec);
}

Build::~Build() {
  // Before anything else, and before the early return below: the counting
  // thread reads prefix_ and this object, and a Build that kept its tree would
  // otherwise leave a thread walking a destroyed one.
  stop_counting();
  if (keep_tree) return;
  std::error_code ec;
  fs::remove_all(work_, ec);
}

std::string Build::id() const {
  std::string n = work_.filename().string();
  return n.rfind("install-", 0) == 0 ? n.substr(8) : n;
}

const fs::path& Build::work_dir() const { return work_; }

// The game's own journal, which is what makes an install worth photographing:
// a game added and not yet played has a picture of its own installer rather
// than a coloured rectangle. It is saves/<id>/journal, and <id> is read back
// out of the staging directory's name - so it follows a rehome, which is the
// whole reason a rehome exists.
fs::path Build::journal_dir() const { return saves_dir() / id() / "journal"; }

namespace {

// Re-roots one path that lived under `from` so that it lives under `to`.
// Anything outside `from` - a disc's source file, off in the user's own
// collection - is left exactly as it was.
void reroot(fs::path& p, const fs::path& from, const fs::path& to) {
  if (p.empty()) return;
  fs::path rel = p.lexically_relative(from);
  if (rel.empty() || rel.native() == "." || *rel.begin() == "..") return;
  p = to / rel;
}

}  // namespace

bool Build::rehome(const std::string& id) {
  if (id.empty()) return true;
  fs::path want = staging_dir(id);
  if (want == work_) return true;

  std::error_code ec;
  // Whatever is under that name is from an install that did not finish - the
  // same thing the constructor says about the tree it is handed, and for the
  // same reason: a rename cannot move onto a directory that is not empty.
  fs::remove_all(want, ec);
  fs::create_directories(want.parent_path(), ec);
  fs::rename(work_, want, ec);
  if (ec) {
    // Both names are under cache_dir(), so this is one filesystem and a rename
    // is a rename. If it failed anyway, the tree is still whole where it was
    // and the install can go on; what is lost is the swap command and the
    // journal's name, which is a sentence rather than a stopped install.
    say_("could not rename the staging tree to " + want.filename().string() + ": " +
         ec.message());
    return false;
  }

  fs::path from = work_;
  work_ = want;
  prefix_ = work_ / "prefix";
  home_ = work_ / "home";
  tree_dir_ = work_ / "tree";
  // A prefix that has already been booted holds absolute paths of its own: the
  // user's directories under drive_c are symlinks into home_, which has just
  // moved with everything else. The .kretro-ready stamp is what makes
  // prepare_prefix skip wineboot, so dropping it has the next prepare repair
  // those links instead of leaving an installer writing into a tree that is no
  // longer there. Normally there is no stamp to drop - step 2 comes before the
  // first prepare - and this is the price of coming back to step 2 afterwards.
  fs::remove(prefix_ / ".kretro-ready", ec);
  // The discs were opened into the old tree and hold paths into it: the
  // normalised ISO, what iso::Info recorded about it, and any audio ripped
  // beside it. A tree that moved and a disc set that did not is a set of paths
  // to files that are no longer there.
  std::lock_guard<std::mutex> lk(mounts_mu_);
  for (disc::Disc& d : discs_) {
    reroot(d.iso, from, work_);
    reroot(d.info.path, from, work_);
    reroot(d.source, from, work_);
    for (disc::AudioTrack& t : d.audio) reroot(t.file, from, work_);
  }
  for (fs::path& t : disc_trees_) reroot(t, from, work_);
  return true;
}

void Build::cancel() {
  cancelled_ = true;
  // Part 5: "during step 4 it also terminates the compositor's process group,
  // which the worker's RAII guards then tear down as usual."
  //
  // The flag alone is not enough there, and this is the one step where that
  // matters. Every other step checks cancelled_ between chunks of its own
  // work; step 4 is blocked inside run_in_compositor waiting for an installer
  // that is itself waiting for a person, so it will not reach a check until
  // the person it is waiting for finishes the install they just abandoned.
  // Killing the group makes that waitpid return, and the WestonGuard, XGuard
  // and CaptureGuard in run_in_compositor do the rest on the way out.
  //
  // Zero when no installer is running, which is every other moment, so a
  // cancel from any other step is exactly the flag it always was.
  pid_t g = setup_pgid_.load();
  if (g > 0) ::kill(-g, SIGTERM);
}
bool Build::cancelled() const { return cancelled_; }

const std::vector<disc::Disc>& Build::discs() const { return discs_; }
const std::vector<Build::Source>& Build::sources() const { return sources_; }
const std::vector<Meta::Disc>& Build::drives() const { return drives_; }

Build::Mounted Build::mounted() const {
  Mounted m;
  std::lock_guard<std::mutex> lk(mounts_mu_);
  m.ready = mounts_ready_;
  m.drives.reserve(drives_.size());
  for (const Meta::Disc& d : drives_) m.drives.push_back(d.label);
  m.discs.reserve(discs_.size());
  for (const disc::Disc& d : discs_) m.discs.push_back(d.label);
  return m;
}
const std::vector<Build::Candidate>& Build::candidates() const { return candidates_; }
const std::string& Build::registry_fragment() const { return fragment_; }

void Build::adopt_discs(std::vector<disc::Disc> discs) {
  std::lock_guard<std::mutex> lk(mounts_mu_);
  discs_ = std::move(discs);
}

namespace {

// A mounted CD, or a disc somebody already extracted. probe_into
// (container.cpp:117-132) returns candidates only for disc images and
// archives, so this is the one source kind the existing pipeline cannot see at
// all - and it is a few dozen lines, because there is nothing to normalise.
disc::Disc open_directory(const fs::path& dir) {
  disc::Disc d;
  d.source = dir;
  d.iso = dir;                 // there is no image: the tree is the disc
  d.info.path = dir;
  d.info.volume_id = dir.filename().string();

  // assemble() collapses a duplicate dump by (size, prefix hash), and a
  // directory has neither unless it is given them. The listing - names and
  // sizes, sorted - is decisive between two different discs and costs a stat
  // per file, where hashing the contents of a 700 MB mount would not be
  // something a person would sit through while adding sources.
  std::vector<std::string> lines;
  std::error_code ec;
  uint64_t total = 0;
  for (const fs::directory_entry& de : fs::recursive_directory_iterator(dir, ec)) {
    std::error_code e2;
    if (!de.is_regular_file(e2)) continue;
    uint64_t sz = de.file_size(e2);
    total += sz;
    lines.push_back(fs::relative(de.path(), dir, e2).generic_string() + "\t" +
                    std::to_string(sz) + "\n");
  }
  std::sort(lines.begin(), lines.end());
  std::string canon;
  for (const std::string& l : lines) canon += l;
  d.info.size = total;
  d.info.prefix = hash_string(canon);

  d.label = d.info.volume_id;
  d.serial = disc::volume_serial(d.info);
  return d;
}

}  // namespace

void Build::open_sources(const std::vector<fs::path>& sources) {
  sources_.clear();
  discs_.clear();
  std::error_code ec;

  // Probing several gigabytes is slow, so this runs on the worker, says which
  // source it is on, and gives up between sources when asked to.
  std::vector<disc::Disc> opened;
  size_t n = 0;
  for (const fs::path& p : sources) {
    if (cancelled_) return;
    ++n;
    Source s = classify(p);
    say_("reading " + p.filename().string() + "  (" + std::to_string(n) + " of " +
         std::to_string(sources.size()) + ")");
    try {
      switch (s.kind) {
        case Source::Kind::Directory:
          opened.push_back(open_directory(p));
          break;
        case Source::Kind::DiscImage:
        case Source::Kind::Archive: {
          // probe finds every disc inside this one file, archives inside
          // archives included; open materialises and normalises each.
          std::vector<disc::Candidate> cands = disc::probe(env_, p);
          if (cands.empty()) {
            s.kind = Source::Kind::Unreadable;
            s.trouble = "no disc image inside it";
            break;
          }
          int i = 0;
          for (const disc::Candidate& c : cands) {
            if (cancelled_) return;
            fs::path w = work_ / "media" /
                         ("source-" + std::to_string(n) + "-" + std::to_string(i++));
            fs::create_directories(w, ec);
            opened.push_back(disc::open(env_, c, w, /*rip_audio=*/true));
          }
          break;
        }
        case Source::Kind::BareExe:
          // Not a disc at all. It selects the installer_exe method, and the
          // set can be empty.
          break;
        case Source::Kind::Unreadable:
          break;
      }
    } catch (const std::exception& ex) {
      // One bad zip in three does not cost the other two.
      s.kind = Source::Kind::Unreadable;
      s.trouble = ex.what();
    }
    sources_.push_back(s);
  }

  // One assemble over everything, and this is the whole of what is new here:
  // assemble traverses nothing, so the only way three separately-handed-over
  // dumps of the same disc become one disc and two alternates is to give it
  // the concatenation rather than one source at a time.
  std::string name = sources.empty() ? "" : sources[0].stem().string();
  disc::DiscSet set = disc::assemble(disc::set_id_from(name), name, std::move(opened));
  {
    // mounted() is read on the UI thread from the frame the worker starts, so
    // the one assignment that gives this Build its discs happens under the
    // same lock as everything else the drive row reads.
    std::lock_guard<std::mutex> lk(mounts_mu_);
    discs_ = std::move(set.discs);
  }
  for (const disc::Disc& a : set.alternates) {
    say_("  also: " + a.label + " (alternate dump, bytes differ)");
  }
  say_(std::to_string(discs_.size()) + (discs_.size() == 1 ? " disc" : " discs") + ", one game");
}

void Build::prepare_prefix(const std::string& windows_version) {
  if (cancelled_) return;
  // A staging prefix, thrown away afterwards: the installer must not be able
  // to leave anything in the prefix the game will actually run in.
  //
  // windows_version is asked before the install, not after, because
  // prepare_prefix applies it before the installer runs and the installer is
  // exactly where it matters - some repacked installers refuse 9x, then refuse
  // XP, and want Vista or later.
  Meta m;
  m.run.windows_version = windows_version;
  m.run.width = 800;
  m.run.height = 600;
  // False, and this is load-bearing: a recipe's Meta arrives here carrying a
  // registry fragment, and applying it would put the game's own keys into the
  // reg_before snapshot, where diff_reg would find them equal on both sides
  // and drop them. The rebuilt pack would come out with an empty registry.
  session::prepare_prefix(env_, prefix_, home_, m, /*apply_registry=*/false, say_);
}

void Build::mount_discs() {
  std::error_code ec;
  {
    // Nothing to draw until this finishes. The wizard's install page is
    // already on screen and asking; `ready` is what makes it wait rather than
    // iterate a vector this function is in the middle of rebuilding.
    std::lock_guard<std::mutex> lk(mounts_mu_);
    mounts_ready_ = false;
    drives_.clear();
  }
  // Parallel to discs_, and filled here rather than looked up later: these are
  // the trees write() lays into the body, and they are extracted once whatever
  // else happens to them.
  disc_trees_.assign(discs_.size(), fs::path{});
  // Every disc mounted at once, as a CD-ROM carrying its real label and
  // serial. Installers that scan the drives then never ask for a swap, and a
  // CD check at install time passes because the disc genuinely is there.
  say_("mounting " + std::to_string(discs_.size()) +
       (discs_.size() == 1 ? " disc" : " discs"));
  for (size_t i = 0; i < discs_.size(); ++i) {
    if (cancelled_) return;
    char letter = static_cast<char>('d' + i);
    fs::path tree = work_ / (std::string("drive-") + letter);

    // One call for every kind of source. A directory - a mounted CD, or a
    // disc somebody already extracted - is copied rather than symlinked into
    // place: this tree is also the tree write() lays into the pack body, and a
    // farm of links into the builder's own filesystem would reach the
    // recipient as a CD-ROM drive full of dangling names.
    std::string derr;
    if (!iso::extract_subtree(env_, discs_[i].iso, "", tree, &derr)) {
      throw std::runtime_error("could not read " + discs_[i].label + ":\n" + derr);
    }

    disc::mount_cdrom(env_, prefix_, letter, tree, discs_[i].label, discs_[i].serial);
    disc_trees_[i] = tree;
    say_(std::string("  ") + static_cast<char>(std::toupper(letter)) + ": " + discs_[i].label);

    Meta::Disc d;
    d.label = discs_[i].label;
    d.serial = discs_[i].serial;
    d.ref = disc_ref_for(discs_[i]);
    d.source = fs::absolute(discs_[i].source, ec).lexically_normal().string();
    d.embedded = true;
    {
      std::lock_guard<std::mutex> lk(mounts_mu_);
      drives_.push_back(d);
    }
  }
  std::lock_guard<std::mutex> lk(mounts_mu_);
  mounts_ready_ = true;
}

void Build::swap_disc(char letter, int disc_index) {
  // Exactly what `kretro swap <id> <n>` does, and against the same layout:
  // cmd_swap_game builds work/drive-<'d'+n-1> by hand. With the in-GUI
  // menu the command is the second way to change a disc rather than the only
  // one, but it has to keep working, so this must not move.
  fs::path tree = work_ / (std::string("drive-") + static_cast<char>('d' + disc_index));
  std::error_code ec;
  if (!fs::exists(tree, ec)) {
    throw std::runtime_error("disc " + std::to_string(disc_index + 1) + " is not mounted");
  }
  std::string label;
  {
    std::ifstream f(tree / ".windows-label");
    std::getline(f, label);
  }
  disc::repoint(prefix_, letter, tree);
  say_(std::string(1, static_cast<char>(std::toupper(letter))) + ": is now " +
       (label.empty() ? tree.filename().string() : label));
}

void Build::snapshot_before() {
  if (cancelled_) return;
  // A previous run that was killed rather than finished can leave a wineserver
  // holding this prefix, and it will fight the one we are about to start over
  // a directory that no longer exists.
  //
  // Guarded on env_.valid() so that the tier 1 tests which drive
  // snapshot_before over a synthesised drive_c provably never reach a spawn:
  // Part 9 forbids a unit test running Wine, and "rt::which finds nothing on
  // this machine" is a coincidence rather than a guarantee.
  if (env_.valid()) {
    fs::path ws = rt::which(env_, "wineserver");
    if (!ws.empty()) {
      rt::Env we = env_;
      we.set("WINEPREFIX", prefix_.string());
      rt::run(we, ws, {"-k"});
    }
  }
  // This walks drive_c and both .reg files once. On a prepared prefix that is
  // thousands of entries, which is long enough to look like a hang if nothing
  // says otherwise.
  say_("reading the prefix as it is now");
  reg_before_ = snapshot_prefix(prefix_);
  c_before_ = Tree::from_directory(staging_drive_c(work_));
  say_("  " + std::to_string(c_before_.size()) + " files, " +
       std::to_string(reg_before_.size()) + " registry values");
}

void Build::run_setup(const fs::path& setup, bool headless,
                      std::function<void(const std::string&)> on_display) {
  if (cancelled_) return;
  headless_ = headless;
  on_display_ = std::move(on_display);

  rt::Env we = env_;
  we.set("WINEPREFIX", prefix_.string());
  rt::confine_home(we, home_);

  session::CompositorOptions co;
  co.width = 800;
  co.height = 600;
  co.scale = 1;
  co.socket_suffix = "install-" + id();
  co.home = home_;
  // An installer that puts up a dialog and quits leaves nothing behind to
  // explain itself. Photograph the session while it runs, into the game's own
  // journal, so a game installed and not yet played has a picture of its own
  // installer rather than a coloured rectangle.
  co.capture = true;
  co.capture_dir = journal_dir();
  co.first_capture_after = 4;
  co.keep_every_frame = true;
  // What comes off this screen is the installer, so the tile it leaves is a
  // stand-in and says so. The first frame of the first real play replaces it.
  co.title_is_provisional = true;
  co.pause_on_blur = false;
  // InstallShield's setup.exe launches a child and exits within a second, so
  // this is also the answer to "when has the installer finished": waiting on
  // the process group rather than on setup.exe. Advancing when setup.exe exits
  // would advance in the middle of every InstallShield install.
  co.wait_for_processes = true;
  // The three fields Milestone 4 added, and the whole of the coupling between
  // this engine and the panel the person is looking at.
  co.headless = headless_;
  // Never during an install, whatever the caller asked for. `kretro input`
  // would be a second writer on this display's pointer - the GUI process is
  // already holding the pad and the stage is already injecting XTest events
  // into the same display - and two writers on one pointer is a cursor that
  // fights the hand moving it. A gamepad is for playing, not for clicking
  // Next.
  co.input_helper = false;
  // Fired on the compositor's own thread, the instant Xwayland answers on its
  // socket and before the installer is launched. Both things that want it get
  // it: display_ so that anything on this Build can find the display without a
  // second copy of the plumbing, and the caller's callback so that the UI
  // thread can open its Stage. The lock is released before the callback runs -
  // on_display_ ends up in ImGui code, and holding a Build's mutex across a
  // frame is how a hang gets built.
  co.on_display_ready = [this](const std::string& d) {
    {
      std::lock_guard<std::mutex> lk(display_mu_);
      display_ = d;
    }
    if (on_display_) on_display_(d);
  };
  // What cancel() acts on, for exactly as long as there is something to act
  // on. Cleared below whichever way run_in_compositor leaves, so that a cancel
  // arriving a moment late signals nothing rather than a pid the kernel has
  // since given to somebody else.
  co.on_pgid = [this](pid_t g) { setup_pgid_ = g; };

  say_("running the installer - click through it");
  fs::path wine = rt::find_wine(env_.root);
  struct PgidGuard {
    std::atomic<pid_t>& p;
    ~PgidGuard() { p = 0; }
  } pgid_guard{setup_pgid_};
  // "files written to C:" is only a moving number while there is an installer
  // writing them, so the counter lives exactly as long as this call - however
  // it leaves, thrown out of or returned from.
  struct CountGuard {
    Build* b;
    ~CountGuard() { b->stop_counting(); }
  } count_guard{this};
  start_counting();
  session::CompositorResult cr =
      session::run_in_compositor(env_, we, wine, {setup.string()}, setup.parent_path(), co, say_);
  if (cr.status != 0) {
    say_("  the installer exited with status " + std::to_string(cr.status));
  }
}

size_t count_entries(const fs::path& dir) {
  std::error_code ec;
  size_t n = 0;
  for (auto it = fs::recursive_directory_iterator(dir, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    ++n;
  }
  return n;
}

size_t Build::files_written_so_far() const { return files_written_.load(); }

void Build::start_counting() {
  if (counter_.joinable()) return;
  counting_stop_ = false;
  files_written_ = 0;
  // Its own thread, and the whole reason for one: this walk used to happen on
  // whichever thread asked for the number, and during an install the only
  // thread that asks is the one drawing the frame. A recursive walk of a
  // drive_c an installer is filling costs tens of milliseconds on a large
  // game, and it was being paid once a second in the middle of a frame.
  fs::path c = staging_drive_c(work_);
  counter_ = std::thread([this, c] {
    while (!counting_stop_.load()) {
      files_written_ = count_entries(c);
      // A second between walks, slept in twentieths so that stopping does not
      // have to wait out a whole one.
      for (int i = 0; i < 20 && !counting_stop_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  });
}

void Build::stop_counting() {
  counting_stop_ = true;
  if (counter_.joinable()) counter_.join();
}

void Build::copy_from_disc(const std::string& subdir) {
  if (cancelled_) return;
  if (discs_.empty()) throw std::runtime_error("no disc to copy from");
  say_("extracting (copy)");
  // A second attempt replaces the first rather than landing on top of it: the
  // wizard can go back to step 3, name a different directory and try again,
  // and half of a wrong answer left underneath would go into the pack.
  std::error_code rec;
  fs::remove_all(tree_dir_, rec);
  std::string err;
  std::vector<std::string> listing = iso::list(env_, discs_[0].iso);
  // Joliet casing is not predictable, so the requested spelling is resolved
  // against what the disc actually says - a manifest's `PC` may be `pc`.
  std::string real = subdir.empty() ? "" : iso::resolve(listing, subdir);
  if (!subdir.empty() && real.empty()) {
    throw std::runtime_error("the disc has no directory called " + subdir);
  }
  if (!iso::extract_subtree(env_, discs_[0].iso, real, tree_dir_, &err)) {
    throw std::runtime_error("extraction failed:\n" + err);
  }
}

void Build::unzip_from_disc(const std::string& member, const std::string& subdir) {
  if (cancelled_) return;
  if (discs_.empty()) throw std::runtime_error("no disc to unpack from");
  say_("extracting (unzip)");
  // As above; and the rename at the end of this cannot move a tree onto a
  // non-empty one, so without it a second try fails on the first one's
  // leavings rather than replacing them.
  std::error_code ec;
  fs::remove_all(tree_dir_, ec);
  fs::remove_all(work_ / "member", ec);
  fs::remove_all(work_ / "unpacked", ec);
  std::vector<std::string> listing = iso::list(env_, discs_[0].iso);
  std::string real = iso::resolve(listing, member);
  if (real.empty()) throw std::runtime_error("the disc has no member called " + member);

  fs::path staged = work_ / "member";
  say_("  taking " + real + " off the disc");
  std::string err;
  if (!iso::extract_member(env_, discs_[0].iso, real, staged, &err)) {
    throw std::runtime_error("could not take " + real + " off the disc:\n" + err);
  }
  fs::path archive;
  for (const fs::directory_entry& de : fs::directory_iterator(staged, ec)) archive = de.path();
  if (archive.empty()) throw std::runtime_error("nothing came out of " + real);

  // The archive is an anchor: it is the part of the disc this install actually
  // depends on, so its hash is what a rebuild has to match. It is hashed from
  // the extracted file rather than streamed out of 7z, because a member can be
  // gigabytes and streaming it would mean holding all of it.
  say_("  hashing " + archive.filename().string());
  anchor_ = Anchor{real, fs::file_size(archive, ec), hash_file(archive)};
  have_anchor_ = true;

  fs::path z = rt::which(env_, "7z");
  fs::path unpacked = work_ / "unpacked";
  fs::create_directories(unpacked, ec);
  say_("  unpacking " + archive.filename().string());
  ProcResult r = rt::run(env_, z, {"x", "-y", "-o" + unpacked.string(), archive.string()});
  if (!r.ok()) {
    throw std::runtime_error("could not unpack " + archive.filename().string() + ":\n" + r.out);
  }

  fs::path from = unpacked;
  if (!subdir.empty()) {
    bool found = false;
    for (const fs::directory_entry& de : fs::directory_iterator(unpacked, ec)) {
      if (lower(de.path().filename().string()) == lower(subdir)) {
        from = de.path();
        found = true;
        break;
      }
    }
    if (!found) throw std::runtime_error("the archive has no directory called " + subdir);
  }
  fs::rename(from, tree_dir_, ec);
  if (ec) throw std::runtime_error("cannot move the game into place: " + ec.message());
}

void Build::diff_after() {
  std::vector<RegValue> reg_after = snapshot_prefix(prefix_);
  // The serial the person typed into the installer is theirs, not the game's,
  // and keys.h says so twice. It is held out of the fragment here, which is the
  // only place the fragment is made: everything downstream - Meta.registry,
  // registry.reg in the body, every recipe exported from the pack - reads what
  // this line produced. Said out loud rather than dropped quietly, because a
  // game whose key does not come back will ask for it on first run, and the
  // vault has it under this id for exactly that moment.
  std::vector<std::string> serials;
  fragment_ = to_reg_fragment(without_serials(diff_reg(reg_before_, reg_after), &serials));
  for (const std::string& s : serials) {
    say_("  keeping your serial out of the pack: " + s);
  }

  Tree c_after = Tree::from_directory(staging_drive_c(work_));
  Tree::Diff cdiff = c_before_.diff_to(c_after);
  if (cdiff.added.empty() && cdiff.changed.empty()) {
    throw std::runtime_error(
        "the installer wrote nothing to C:. It did not run, or it was cancelled.");
  }
  say_("  it wrote " + std::to_string(cdiff.added.size()) + " files to C:");
  // Both lists, because a DLL an installer replaced is as much a part of what
  // it did as one it added, and the fragment that registers it does not
  // distinguish. This is kept whole and filtered in write(), once a person has
  // said which of these directories is the game.
  c_written_.clear();
  c_written_.reserve(cdiff.added.size() + cdiff.changed.size());
  for (const TreeEntry& t : cdiff.added) c_written_.push_back(t);
  for (const TreeEntry& t : cdiff.changed) c_written_.push_back(t);
  // Where it landed is now a question with a ranked list of answers rather
  // than a single guess that needed source.verify to be right in advance.
  candidates_ = rank_candidates(staging_drive_c(work_), cdiff);
}

SystemFiles Build::outside(const fs::path& install_dir) const {
  SystemFiles out;
  const std::string dir = install_dir.generic_string();
  for (const TreeEntry& t : c_written_) {
    if (!t.is_regular() && !t.is_symlink()) continue;
    if (!is_outside_the_game(t.path, dir)) continue;
    ++out.files;
    if (t.is_regular()) out.bytes += t.size;
  }
  return out;
}

fs::path Build::installed_root() const {
  std::error_code ec;
  return fs::exists(tree_dir_, ec) ? tree_dir_ : staging_drive_c(work_);
}

void Build::survey_extracted() {
  if (cancelled_) return;
  std::error_code ec;
  if (!fs::exists(tree_dir_, ec)) {
    throw std::runtime_error("nothing came off the disc: there is no tree to pack");
  }
  // No installer ran, so there is no registry to diff either: the pack gets an
  // empty fragment rather than a stale one - and nothing was written to C: at
  // all, so there is no system/ to carry.
  fragment_.clear();
  c_written_.clear();
  Candidate c = survey_tree(tree_dir_);
  if (c.files == 0) {
    throw std::runtime_error(
        "nothing came off the disc: what was copied is an empty directory");
  }
  say_("  " + std::to_string(c.files) + " files came off the disc");
  candidates_ = {std::move(c)};
}

namespace {

const char* dwarfs_tool_env() {
  const char* v = std::getenv("KRETRO_DWARFS");
  return (v && *v) ? v : nullptr;
}

// Case-insensitive existence check against a real directory listing, because
// what a disc calls GAME.EXE a manifest may call Game.exe.
bool exists_ci_under(const fs::path& root, const std::string& want) {
  std::error_code ec;
  fs::path cur = root;
  std::string w = want;
  std::replace(w.begin(), w.end(), '\\', '/');
  std::stringstream ss(w);
  std::string part;
  while (std::getline(ss, part, '/')) {
    if (part.empty()) continue;
    bool found = false;
    for (const fs::directory_entry& de : fs::directory_iterator(cur, ec)) {
      if (lower(de.path().filename().string()) == lower(part)) {
        cur = de.path();
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

}  // namespace

Result Build::write(Meta m) {
  std::error_code ec;

  // The installer methods leave the game under C: and say where; copy and
  // unzip have already put it at tree_dir_. One place, either way, before
  // anything hashes it.
  if (!fs::exists(tree_dir_, ec) && !m.install.install_dir.empty()) {
    fs::rename(staging_drive_c(work_) / m.install.install_dir, tree_dir_, ec);
    if (ec) throw std::runtime_error("cannot move the game into place: " + ec.message());
  }
  if (!fs::exists(tree_dir_, ec)) throw std::runtime_error("there is no installed tree to pack");

  if (m.registry.fragment.empty()) m.registry.fragment = fragment_;
  if (have_anchor_ && !m.recipe.fingerprints.empty()) {
    m.recipe.fingerprints[0].anchors.push_back(anchor_);
  }

  say_("verifying");
  for (const std::string& v : m.recipe.verify) {
    if (!exists_ci_under(tree_dir_, v)) {
      throw std::runtime_error("the extracted game has no " + v +
                               "; the disc is not what the manifest expects");
    }
  }
  if (!exists_ci_under(tree_dir_, m.run.exe)) {
    throw std::runtime_error("the extracted game has no " + m.run.exe);
  }

  // Every disc travels, whatever method installed from it. An installer game
  // already has one extracted tree per drive letter; a copy or unzip game
  // never mounted its disc as a drive, so its tree is pulled off the image
  // here. mkdwarfs deduplicates the installed files against the disc files
  // they were copied from, so for a copy game game/ is very nearly free and
  // for an installer game the immovable assets - the data archives, the
  // video - are stored once. That dedup is what makes carrying everything
  // affordable rather than merely honest.
  //
  // All discs or none. lay_out_body numbers them 1..n and Meta.discs[i] is
  // disc i+1 for every reader of that layout, so leaving one out would
  // renumber the rest; Draft::embed_discs is one checkbox for the same reason.
  bool embed = true;
  for (const Meta::Disc& d : m.discs) {
    if (!d.embedded) embed = false;
  }
  disc_trees_.resize(discs_.size());
  std::vector<BodyDisc> body_discs;
  if (embed) {
    for (size_t i = 0; i < discs_.size(); ++i) {
      if (disc_trees_[i].empty()) {
        fs::path t = work_ / ("disc-tree-" + std::to_string(i + 1));
        fs::create_directories(t, ec);
        say_("  reading " + discs_[i].label + " for the pack");
        std::string derr;
        if (!iso::extract_subtree(env_, discs_[i].iso, "", t, &derr)) {
          throw std::runtime_error("could not read " + discs_[i].label + ":\n" + derr);
        }
        disc_trees_[i] = t;
      }
      std::vector<fs::path> tracks;
      for (const disc::AudioTrack& a : discs_[i].audio) {
        if (!a.file.empty()) tracks.push_back(a.file);
      }
      body_discs.push_back(BodyDisc{disc_trees_[i], discs_[i].label, discs_[i].serial, tracks});
    }
  }

  // What the installer put on the machine that is not the game: the DLLs, the
  // OCXs, the shared runtime it dropped into windows/system32. The registry
  // fragment names those files by path and is applied verbatim on the machine
  // that opens this pack, so leaving them behind ships a set of registrations
  // pointing at nothing. Collected now, because this is the first moment both
  // facts exist at once - the diff, and which directory the game is.
  fs::path system_dir = work_ / "system";
  {
    std::error_code se;
    fs::remove_all(system_dir, se);
    std::vector<std::string> paths;
    for (const TreeEntry& t : c_written_) {
      if (!t.is_regular() && !t.is_symlink() && !t.is_dir()) continue;
      if (is_outside_the_game(t.path, m.install.install_dir)) paths.push_back(t.path);
    }
    SystemFiles got = gather_system_files(staging_drive_c(work_), paths, system_dir);
    m.system.files = static_cast<uint32_t>(got.files);
    m.system.bytes = got.bytes;
    if (got.files) {
      say_("  " + std::to_string(got.files) +
           " files the installer wrote outside the game travel too");
    }
  }

  say_("laying out the body");
  fs::path stage = work_ / "body";
  lay_out_body(stage, tree_dir_, body_discs, m.registry.fragment, system_dir);
  // Every pack this engine writes is rooted, discs or no discs: registry.reg
  // is in the body either way, and Milestone 2's open_layers only looks for
  // the game at image/game when the layout says to. draft_to_meta already
  // says "rooted"; this is the line that makes that true.
  m.layout = "rooted";

  // The tree covers game/ and nothing else, with paths relative to it. That is
  // what keeps the Merkle root meaning the identity of the installed game:
  // this pack and a pack of the same install built without its discs have the
  // same root, `kretro verify` reports the game rather than the game plus two
  // gigabytes of disc, and a session's exit-time diff compares like with like.
  // What is outside game/ is covered by the body hash instead.
  say_("hashing the tree");
  m.tree = Tree::from_directory(stage / "game");

  say_("packing");
  const char* tool = dwarfs_tool_env();
  if (!tool) throw std::runtime_error("no DwarFS tool (KRETRO_DWARFS is unset)");
  fs::path body = work_ / "body.dwarfs";
  // --categorize stores what is already compressed - disc images of video,
  // the textures in a zipped data file - raw instead of squeezing it for nothing, and 4 MiB
  // blocks (-S 22) keep a random read to one block's worth of decompression.
  // A pack is played from a mount, so how fast it reads matters more than the
  // last few percent of size. Neither flag touches the Merkle root, which
  // covers the files, not how the body stores them.
  ProcResult r = kg::run({std::string(tool), "--tool=mkdwarfs", "-i", stage.string(), "-o",
                          body.string(), "--categorize", "-S", "22", "--log-level=error",
                          "--no-progress", "-f"});
  if (!r.ok()) throw std::runtime_error("mkdwarfs failed:\n" + r.out);
  // Said in the pack, so a pack made before these flags can be told from one
  // made with them, and offered a repack when it goes into a player.
  m.body.packing = kBodyPacking;

  fs::path out = games_dir() / (m.id + ".kgpack");
  WriteOptions wo;
  wo.kind = Kind::Game;
  wo.body = body;
  write_pack(out, m, wo);

  Result res;
  res.pack = out;
  res.tree_bytes = m.tree.total_bytes();
  res.entries = m.tree.size();
  res.pack_bytes = fs::file_size(out, ec);
  res.root = m.tree.root();
  return res;
}

Result Build::write(const Draft& d) {
  Meta m = draft_to_meta(d, discs_);
  // The verify list is read off the confirmed directory rather than named in
  // advance, which is the whole of what source.verify used to do. Step 6 does
  // that reading, the moment the user picks the executable, and draft_to_meta
  // carries the answer here - so this asks for one only when the draft brought
  // none. It used to compute one unconditionally, over the top of the list it
  // had just been handed: the field was filled, carried and discarded on
  // arrival, and two answers to one question is one answer too many.
  m.recipe.verify = verify_for(d, installed_root());
  return write(std::move(m));
}

std::string IdClash::sentence() const {
  std::vector<std::string> parts;
  if (pack) parts.push_back("a pack");
  if (saves) parts.push_back("saves");
  if (prefix) parts.push_back("a Wine prefix");
  if (manifest) parts.push_back("a manifest");
  std::string s;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) s += (i + 1 == parts.size()) ? " and " : ", ";
    s += parts[i];
  }
  return s;
}

IdClash id_clash(const rt::Env& e, const std::string& id) {
  IdClash c;
  std::error_code ec;
  if (id.empty()) return c;
  c.pack = fs::exists(games_dir() / (id + ".kgpack"), ec);
  c.saves = fs::exists(saves_dir() / id, ec);
  c.prefix = fs::exists(prefixes_dir() / id, ec);
  for (const fs::path& d : manifest_dirs(e)) {
    if (fs::exists(d / (id + ".toml"), ec)) { c.manifest = true; break; }
  }
  return c;
}

std::string next_free_id(const rt::Env& e, const std::string& id) {
  for (int n = 2; n < 100; ++n) {
    std::string candidate = id + "-" + std::to_string(n);
    if (!id_clash(e, candidate).any()) return candidate;
  }
  return id + "-new";
}

Prefill draft_from_meta(const rt::Env& e, const Meta& m) {
  // find_iso and find_iso_by_fingerprint read the collection directory out of
  // the environment rather than being handed one, so nothing below needs `e`.
  // It stays in the signature because resolving a disc is exactly the kind of
  // thing that grows a runtime dependency, and every caller already has one.
  (void)e;
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

  // The four strings install.cpp:682 accepts, and the four Draft::Method
  // values they became.
  if (m.recipe.method == "copy") p.draft.method = Draft::Method::Copy;
  else if (m.recipe.method == "unzip") p.draft.method = Draft::Method::Unzip;
  else if (m.recipe.method == "installer_exe") p.draft.method = Draft::Method::InstallerExe;
  else p.draft.method = Draft::Method::Installer;

  for (size_t i = 0; i < m.recipe.discs.size(); ++i) {
    DiscRef r = parse_disc_ref(m.recipe.discs[i]);
    fs::path found = find_iso(r.archive);
    if (found.empty() && i < m.recipe.fingerprints.size()) {
      found = find_iso_by_fingerprint(m.recipe.fingerprints[i]);
    }
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

namespace {

bool preset_ci_equal(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Identity, at whatever strength is available without hashing gigabytes.
// disc::open scans with full=false (disc.cpp:126), so info.whole is normally
// absent at step 2 and the size-and-volume-label pair is as strong as this can
// honestly get; the whole hash is used when a recipe import has already
// produced one.
bool preset_same_disc(const DiscFingerprint& want, const disc::Disc& got) {
  if (!want.size || want.size != got.info.size) return false;
  if (got.info.has_whole) return want.blake3 == got.info.whole;
  return !want.volume_id.empty() && preset_ci_equal(want.volume_id, got.info.volume_id);
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
        if (!r.label.empty() && preset_ci_equal(r.label, d.label)) { hit = true; break; }
        if (r.label.empty() && !r.archive.empty() &&
            preset_ci_equal(r.archive, d.source.filename().string())) { hit = true; break; }
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
  // this is.
  if (m.recipe.method == "copy") d->method = Draft::Method::Copy;
  else if (m.recipe.method == "unzip") d->method = Draft::Method::Unzip;
  else if (m.recipe.method == "installer_exe") d->method = Draft::Method::InstallerExe;
  else if (!m.recipe.method.empty()) d->method = Draft::Method::Installer;
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
