// Build's installer steps: the staging prefix, the discs mounted as drives,
// the snapshot taken before, and the installer itself running in a
// compositor. The only part of Build that reaches into session/.
#include "build.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "../disc/drive.h"
#include "../disc/members.h"
#include "../session/compositor.h"
#include "../session/prefix.h"
#include "discs.h"
#include "staging.h"

namespace kg::install {
namespace fs = std::filesystem;

namespace {

// The installer's screen, in the staging prefix and in the compositor it runs in.
constexpr int kInstallerWidth = 800, kInstallerHeight = 600;

}  // namespace

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
  m.run.width = kInstallerWidth;
  m.run.height = kInstallerHeight;
  // False, and this is load-bearing: a recipe's Meta arrives here carrying a
  // registry fragment, and applying it would put the game's own keys into the
  // reg_before snapshot, where diff_reg would find them equal on both sides
  // and drop them. The rebuilt pack would come out with an empty registry.
  session::prepare_prefix(env_, prefix_, home_, m, /*apply_registry=*/false, say_);
}

void Build::mount_discs() {
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
    char letter = staging_drive_letter(i);
    fs::path tree = staging_drive(work_, i);

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

    Meta::Disc d = disc_entry(discs_[i], disc_ref_for(discs_[i]), /*embedded=*/true);
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
  // both find the disc at install::staging_drive(work, n - 1). With the in-GUI
  // menu the command is the second way to change a disc rather than the only
  // one, but it has to keep working, so this must not move.
  fs::path tree = staging_drive(work_, static_cast<size_t>(disc_index));
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
  // a unit test must never run Wine, and "rt::which finds nothing on
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
  reg_before_ = wine::snapshot_prefix(prefix_);
  c_before_ = Tree::from_directory(staging_drive_c(work_));
  say_("  " + std::to_string(c_before_.size()) + " files, " +
       std::to_string(reg_before_.size()) + " registry values");
}

void Build::run_setup(const fs::path& setup, bool headless,
                      std::function<void(const std::string&)> on_display) {
  if (cancelled_) return;

  rt::Env we = rt::wine_env(env_, prefix_, home_);

  session::CompositorOptions co;
  co.width = kInstallerWidth;
  co.height = kInstallerHeight;
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
  // These three fields are the whole of the coupling between this engine and
  // the panel the person is looking at.
  co.headless = headless;
  // Never during an install, whatever the caller asked for. `kretro input`
  // would be a second writer on this display's pointer - the GUI process is
  // already holding the pad and the stage is already injecting XTest events
  // into the same display - and two writers on one pointer is a cursor that
  // fights the hand moving it. A gamepad is for playing, not for clicking
  // Next.
  co.input_helper = false;
  // Fired on the compositor's own thread, the instant Xwayland answers on its
  // socket and before the installer is launched. It goes straight to the
  // caller's callback, so that the UI thread can open its Stage; that callback
  // ends up in ImGui code, so nothing of this Build's is locked around it.
  co.on_display_ready = [on_display = std::move(on_display)](const std::string& d) {
    if (on_display) on_display(d);
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

size_t Build::files_written_so_far() const { return files_written_.load(); }

void Build::start_counting() {
  if (counter_.joinable()) return;
  counting_stop_ = false;
  files_written_ = 0;
  // Its own thread, and the whole reason for one: during an install the only
  // thread that asks for the number is the one drawing the frame. A recursive
  // walk of a drive_c an installer is filling costs tens of milliseconds on a
  // large game, and on the asking thread it would be paid once a second in
  // the middle of a frame.
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

}  // namespace kg::install
