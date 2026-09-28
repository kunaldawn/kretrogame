// An install, taken apart into the moments a person can be present for.
//
// install::run was one call of about 450 lines because nobody was in the room:
// it mounted, ran, diffed and packed without ever needing to ask. The wizard's
// whole point is the asking, so each thing the engine learns has to be
// available before the next thing happens - which is all this class is.
//
// The wizard's own arithmetic lives beside it, in draft.h, setup_ref.h,
// source.h, survey.h, preset.h, game_id.h and staging.h, and deliberately not
// in src/gui: the test binaries link LIB_OBJ and cannot see a translation unit
// that includes SDL. Slugging an id, ranking candidates, ranking executables,
// choosing a verify list and compiling a Draft are the parts that can be wrong
// in a way nobody would notice until a rebuild fails, so they are the parts
// that get tested.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <vector>

#include "../disc/disc.h"
#include "../pack/kgpack.h"
#include "../pack/tree.h"
#include "../rt/env.h"
#include "../wine/registry.h"
#include "../wine/system_files.h"
#include "body.h"
#include "draft.h"
#include "install.h"
#include "set_merge.h"
#include "source.h"
#include "survey.h"

namespace kg::install {

// Every long step reports through one of these. The CLI prints them; the
// wizard puts them in a strip under the stage.
using Say = std::function<void(const std::string&)>;

// An install in progress, one step at a time.
class Build {
 public:
  Build(const rt::Env& e, std::filesystem::path work, Say say);
  // Removes the staging tree unless keep_tree was set. This is the only thing
  // that deletes it, so a cancelled or crashed install does not leave
  // gigabytes behind under cache_dir().
  ~Build();

  Build(const Build&) = delete;
  Build& operator=(const Build&) = delete;

  // Named by the destructor above, so it has to be reachable. install::run
  // sets it from Options::keep_tree.
  bool keep_tree = false;

  // --- the name -------------------------------------------------------
  // Where this install is staging itself, and where the frames taken while the
  // installer runs are kept. Both are read off the staging directory's own
  // name, which is why the name matters beyond tidiness: `kretro swap <id> <n>`
  // finds a running install at cache_dir()/install-<id> and nowhere else, and
  // the journal belongs to the game the pack is about to become.
  const std::filesystem::path& work_dir() const;
  std::filesystem::path journal_dir() const;

  // Renames the staging tree for the id the user settled on.
  //
  // Step 1 has to open the discs before step 2 can offer a name for what came
  // off them, so the tree exists before there is an id to call it by. Left
  // alone it stayed cache_dir()/install-new: the swap command could never
  // address a wizard install, and the installer's frames landed in saves/new/,
  // a journal belonging to a game that does not exist.
  //
  // Everything the disc set points at inside the tree moves with it. False,
  // with a line through say_, when the rename itself failed - the install goes
  // on under the old name rather than losing its discs to a half-move.
  //
  // Between steps, on the thread that draws them: there is no worker running
  // while a person is typing a name.
  bool rehome(const std::string& id);

  // --- sources -------------------------------------------------------
  // Opens every source and assembles one disc set. Reports progress per
  // source and honours cancel(); a source that throws becomes Unreadable and
  // does not abort the rest.
  void open_sources(const std::vector<std::filesystem::path>& sources);
  const std::vector<disc::Disc>& discs() const;
  const std::vector<Source>& sources() const;

  // The manifest path resolves its discs by reference through resolve_discs,
  // which picks one disc out of an archive by volume label; open_sources takes
  // whole files and cannot express that. install::run therefore hands over an
  // already-opened set. The invariant is the same either way: after this,
  // discs() is what every later step works on.
  void adopt_discs(std::vector<disc::Disc> discs);

  // --- the install ---------------------------------------------------
  void prepare_prefix(const std::string& windows_version);
  void mount_discs();
  const std::vector<Meta::Disc>& drives() const;   // which letter each disc got

  // What the install page draws its drive row from, by value.
  //
  // Not drives() and not discs(): the wizard starts drawing the install step
  // on the frame it starts the worker, and the worker is inside mount_discs
  // clearing drives_ and push_backing into it. A reference the UI thread held
  // across that reallocation dangles in the middle of a frame, and the row it
  // was drawing is a use-after-free. So this is a copy, taken under the lock
  // mount_discs publishes under, and `ready` is the gate: until the mounts are
  // done there is no row to draw, only a sentence saying so.
  struct Mounted {
    bool ready = false;                 // mount_discs finished; the row is real
    std::vector<std::string> drives;    // one label per drive letter, D: first
    std::vector<std::string> discs;     // what a drive can be pointed at
  };
  Mounted mounted() const;
  void swap_disc(char letter, int disc_index);     // the in-GUI disc change

  void snapshot_before();

  // Blocks until the installer's whole process group has exited. Publishes
  // the X display through `on_display` as soon as Xwayland is up, so the
  // caller's UI thread can open a Stage on it. Runs on a worker thread.
  void run_setup(const std::filesystem::path& setup, bool headless,
                 std::function<void(const std::string&)> on_display);

  // The number the install page shows, as an atomic read of what the counting
  // thread last published - never a walk. A recursive walk of a drive_c an
  // installer is writing into, done on the UI thread once a second for the
  // length of an install, is a visible stall once a second on a large game.
  size_t files_written_so_far() const;
  // The counting thread. run_setup raises one for exactly as long as the
  // installer runs, and ~Build stops one that is somehow still going; they are
  // public so that a test can drive the counter without an installer.
  void start_counting();
  void stop_counting();

  // The other two methods, as alternatives to run_setup.
  void copy_from_disc(const std::string& subdir);
  void unzip_from_disc(const std::string& member, const std::string& subdir);

  // --- what it wrote -------------------------------------------------
  void diff_after();
  // What the installer wrote to C: outside `install_dir`, as a count and a
  // size. The build page reads it so the person about to press Build can see
  // what is going into the pack besides the game; write() carries it.
  //
  // Arithmetic over the diff diff_after already took, with no filesystem in it,
  // because the build page asks this once a frame.
  wine::SystemFiles outside(const std::filesystem::path& install_dir) const;
  // The same question for a method that ran no installer. Copy and unzip put
  // the game at the staging tree and write nothing whatever to C:, so there is
  // no diff to rank and diff_after would report, truthfully and uselessly,
  // that the installer wrote nothing. What landed is not a question here - it
  // is the tree, whole - and this states that in the shape the steps after it
  // already read.
  void survey_extracted();
  // Where the game is on this filesystem right now: the staging tree once a
  // copy or unzip has filled it, drive_c otherwise. Candidate::dir is relative
  // to whichever of the two the method used, and the size of an executable and
  // the verify list are both read from here.
  std::filesystem::path installed_root() const;
  // Deepest first, then by file count. Empty only if the installer wrote
  // nothing, which the caller must handle.
  const std::vector<Candidate>& candidates() const;

  Result write(const Draft& d);
  // Lays the body out as a set - games/<id>/, discs/<key>/ - hashes the game's
  // tree, packs the lot and writes the .kgpack. Both paths end here, so both
  // produce the layout docs/file-format.md describes and neither can drift from
  // the other.
  //
  // Every disc Meta::discs names is in discs/; system/ is there whenever the
  // installer wrote outside the game directory - the registry fragment names
  // those files and the fragment travels.
  //
  // The manifest path already has a Meta - fingerprints, input bindings,
  // winetricks - that a Draft cannot carry. It compiles its own and hands it
  // over; write(Draft) is draft_to_meta, the verify list read off the
  // installed directory, and then exactly this.
  Result write(Meta m);

  // Cooperative between steps, and not only cooperative during one: an
  // installer that is waiting for a person will never reach a check, so this
  // also terminates the process group run_setup launched. The RAII guards
  // inside run_in_compositor then tear the compositor down as usual.
  void cancel();
  bool cancelled() const;

 private:
  // The staging directory is named for the game, and two things already depend
  // on that: `kretro swap <id> <n>` finds a running install by it (see
  // staging_dir), and the frames captured while the installer runs have to
  // land in that game's journal. So the id is read back out of the path rather
  // than passed twice and allowed to disagree.
  std::string id() const;

  // write(Meta)'s steps, in the order it calls them (build_write.cpp). Each
  // carries the Meta forward; the paths they return are the next step's input.
  void place_game_tree(const Meta& m);
  void merge_registry_and_anchor(Meta& m) const;
  void check_verify(const Meta& m) const;
  std::vector<BodyDisc> collect_body_discs(const Meta& m, const std::vector<std::string>& only);
  std::filesystem::path collect_system_files(Meta& m);
  void collect_folds(const MergePlan& plan, const std::vector<ShelfSet>& sets, const Meta& m,
                     std::vector<BodyGame>& games, std::vector<BodyDisc>& discs);
  std::filesystem::path lay_out_and_hash(Meta& m, MergePlan& plan, const std::vector<BodyGame>& games,
                                         const std::vector<BodyDisc>& discs);
  std::filesystem::path pack_body(const std::filesystem::path& stage);
  Result write_set(MergePlan& plan, const std::vector<ShelfSet>& sets, const Meta& m,
                   const std::filesystem::path& body);

  rt::Env env_;
  std::filesystem::path work_, prefix_, home_, tree_dir_;
  Say say_;

  std::vector<Source> sources_;
  std::vector<disc::Disc> discs_;
  std::vector<Meta::Disc> drives_;
  // Where each disc's extracted tree ended up, parallel to discs_. mount_discs
  // makes one per drive letter; the copy and unzip methods mount nothing, so
  // write() extracts theirs when it comes to lay the body out.
  std::vector<std::filesystem::path> disc_trees_;

  std::vector<wine::RegValue> reg_before_;
  Tree c_before_;
  std::string fragment_;
  // Every file the installer added to or changed under drive_c, relative to it,
  // as diff_after found them. It is kept because the question "what did it
  // write that is not the game" cannot be answered until step 5, when a person
  // has said which directory the game is - and by then the diff is gone. The
  // sizes travel with the paths so that the build page can total them without
  // walking a prefix once a frame.
  std::vector<TreeEntry> c_written_;
  // The archive an unzip install actually depends on: its hash is what a
  // rebuild has to match. Recorded here because it is learned during the
  // extraction and consumed when the pack is written.
  std::optional<Anchor> anchor_;
  std::vector<Candidate> candidates_;

  std::atomic<bool> cancelled_{false};

  // The file counter. The number is published by a thread of its own rather
  // than computed by whoever asks for it, because the only thread that could
  // ask during an install is the one drawing the frame.
  std::thread counter_;
  std::atomic<bool> counting_stop_{false};
  std::atomic<size_t> files_written_{0};

  // Guards drives_ and discs_ against the UI thread reading them through
  // mounted() while this thread is still filling them in.
  mutable std::mutex mounts_mu_;
  bool mounts_ready_ = false;

  // The process group the installer runs in, published by run_setup while it
  // is running and zero otherwise. "Abandon this install" has to end
  // the installer rather than wait for it, and cancel() is called from the UI
  // thread while the worker is blocked inside run_in_compositor - so this is
  // the one thing cancel() can act on.
  std::atomic<pid_t> setup_pgid_{0};
};

}  // namespace kg::install
