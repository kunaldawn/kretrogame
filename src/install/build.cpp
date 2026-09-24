#include "build.h"

#include <csignal>
#include <mutex>
#include <utility>

#include "../util/paths.h"
#include "staging.h"

namespace kg::install {
namespace fs = std::filesystem;

Build::Build(const rt::Env& e, fs::path work, Say say)
    : env_(e), work_(std::move(work)), say_(std::move(say)) {
  std::error_code ec;
  // Whatever is there is from an install that did not finish. Building on top
  // of a half-extracted drive tree would mount a disc that is missing files
  // and blame the disc for it.
  fs::remove_all(work_, ec);
  prefix_ = staging_prefix(work_);
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

std::string Build::id() const { return id_of_staging(work_); }

const fs::path& Build::work_dir() const { return work_; }

// The game's own journal, which is what makes an install worth photographing:
// a game added and not yet played has a picture of its own installer rather
// than a coloured rectangle. It is saves/<id>/journal, and <id> is read back
// out of the staging directory's name - so it follows a rehome, which is the
// whole reason a rehome exists.
//
// Not session::journal_dir: that starts from game_saves_dir(id), and this from
// saves_dir()/<id>, which need not be the same directory.
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
  prefix_ = staging_prefix(work_);
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
  // During step 4 this also terminates the compositor's process group, which
  // the worker's RAII guards then tear down as usual.
  //
  // The flag alone is not enough there, and this is the one step where that
  // matters. Every other step checks cancelled_ between chunks of its own
  // work; step 4 is blocked inside run_in_compositor waiting for an installer
  // that is itself waiting for a person, so it will not reach a check until
  // the person it is waiting for finishes the install they just abandoned.
  // Killing the group makes that waitpid return, and the ScopedChild guards
  // in run_in_compositor do the rest on the way out.
  //
  // Zero when no installer is running, which is every other moment, so a
  // cancel from any other step is exactly the flag it always was.
  pid_t g = setup_pgid_.load();
  if (g > 0) ::kill(-g, SIGTERM);
}
bool Build::cancelled() const { return cancelled_; }

const std::vector<disc::Disc>& Build::discs() const { return discs_; }
const std::vector<Source>& Build::sources() const { return sources_; }
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
const std::vector<Candidate>& Build::candidates() const { return candidates_; }

void Build::adopt_discs(std::vector<disc::Disc> discs) {
  std::lock_guard<std::mutex> lk(mounts_mu_);
  discs_ = std::move(discs);
}

}  // namespace kg::install
