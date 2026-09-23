// An install, taken apart into the moments a person can be present for.
//
// install::run was one call of about 450 lines because nobody was in the room:
// it mounted, ran, diffed and packed without ever needing to ask. The wizard's
// whole point is the asking, so each thing the engine learns has to be
// available before the next thing happens - which is all this class is.
//
// The wizard's own arithmetic lives here as well, and deliberately not in
// src/gui: the test binaries link LIB_OBJ and cannot see a translation unit
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
#include <string>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <vector>

#include "../disc/disc.h"
#include "../pack/kgpack.h"
#include "../pack/tree.h"
#include "../rt/env.h"
#include "install.h"
#include "keys.h"
#include "registry.h"

namespace kg::install {

// Every long step reports through one of these. The CLI prints them; the
// wizard puts them in a strip under the stage.
using Say = std::function<void(const std::string&)>;

// What the wizard has decided so far. It is not a Meta: a Meta is what comes
// out at the end, with fingerprints and a tree in it, and half of that does not
// exist while a person is still choosing.
struct Draft {
  std::vector<std::filesystem::path> sources;
  // `serial` is stored in the key vault under `id` and goes into no pack;
  // keys.h:1-7 states that policy and draft_to_meta obeys it.
  std::string id, name, serial;
  uint32_t year = 0;

  // The four values are the four strings install.cpp's method branch accepts:
  // wine_setup, installer_exe, copy, unzip. A fifth would be a method the
  // engine cannot run.
  enum class Method { Installer, InstallerExe, Copy, Unzip } method = Method::Installer;
  // Installer: "<n>/<path on that disc>", n counting from 1 in the order
  // open_sources assembled the set - because "which exe" is not an answer
  // without "on which disc". InstallerExe: an absolute path to the file.
  std::filesystem::path setup;
  std::string member, subdir;        // unzip / copy

  std::string windows_version = "winxp";   // asked before the install, not after

  std::filesystem::path install_dir; // chosen from the candidates, under drive_c
  std::filesystem::path exe;         // chosen from that directory
  std::string args;
  // Filled on the way out of step 6 by verify_list, from the directory the
  // user confirmed at step 5 and the executable they picked at step 6. It is
  // what the pack carries: draft_to_meta copies it and Build::write keeps it,
  // computing a list of its own only for a draft that arrived without one.
  std::vector<std::string> verify;

  uint32_t width = 640, height = 480;
  bool dgvoodoo = false;
  bool embed_discs = true;
};

// An install in progress, one step at a time.
class Build {
 public:
  Build(const rt::Env& e, std::filesystem::path work, Say say);
  // Removes the staging tree unless keep_tree was set. This is the only thing
  // that deletes it; a cancelled or crashed install used to leave gigabytes
  // behind under cache_dir().
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
  struct Source {
    std::filesystem::path path;
    enum class Kind { DiscImage, Archive, Directory, BareExe, Unreadable } kind;
    std::string trouble;          // why, when Unreadable
  };
  // Classifies without opening anything. Cheap; runs as the user adds files.
  static Source classify(const std::filesystem::path& p);

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
  SystemFiles outside(const std::filesystem::path& install_dir) const;
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
  struct Candidate {
    std::filesystem::path dir;                       // relative to installed_root()
    size_t files = 0;
    uint64_t bytes = 0;
    std::vector<std::filesystem::path> executables;  // ranked
  };
  // Deepest first, then by file count. Empty only if the installer wrote
  // nothing, which the caller must handle.
  const std::vector<Candidate>& candidates() const;
  const std::string& registry_fragment() const;

  Result write(const Draft& d);
  // Lays the body out rooted - game/, system/, discs/<n>/, registry.reg - hashes
  // game/, packs the lot and writes the .kgpack. Both paths end here, so both
  // produce the layout Part 2.1 describes and neither can drift from the other.
  //
  // discs/ is there when every Meta::Disc says embedded and absent when any
  // says otherwise; system/ is there whenever the installer wrote outside the
  // game directory, and is not the discs' to leave out - the registry fragment
  // names those files and the fragment travels whatever the checkbox says.
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
  // cmd_swap_game), and the frames captured while the installer runs have to
  // land in that game's journal. So the id is read back out of the path rather
  // than passed twice and allowed to disagree.
  std::string id() const;

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

  std::vector<RegValue> reg_before_;
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
  Anchor anchor_;
  bool have_anchor_ = false;
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

  std::mutex display_mu_;
  std::string display_;
  bool headless_ = false;
  std::function<void(const std::string&)> on_display_;

  // The process group the installer runs in, published by run_setup while it
  // is running and zero otherwise. Part 5's "abandon this install" has to end
  // the installer rather than wait for it, and cancel() is called from the UI
  // thread while the worker is blocked inside run_in_compositor - so this is
  // the one thing cancel() can act on.
  std::atomic<pid_t> setup_pgid_{0};
};

// An id is four things, not one: state/games/<id>.kgpack, state/saves/<id>,
// state/prefixes/<id>, and <id>.toml among the local manifests. Checking
// only the pack is what lets a new game silently inherit an old game's prefix
// and, worse, its saves.
struct IdClash {
  bool pack = false;
  bool saves = false;
  bool prefix = false;
  bool manifest = false;
  bool any() const { return pack || saves || prefix || manifest; }
  // "a pack and its saves", for a sentence the user reads rather than a
  // checklist they have to decode.
  std::string sentence() const;
};
IdClash id_clash(const rt::Env& e, const std::string& id);

// The next id in the same family that clashes with nothing: "example-game-2",
// then "-3". Offered to the wizard's user as an edit to the field. Never
// applied for them - rebuilding the game you already have is a reasonable
// thing to want, and routing around it silently would take the choice away.
std::string next_free_id(const rt::Env& e, const std::string& id);

// Everything a manifest or a recipe already knows, as a Draft the wizard can
// open on. Disc references resolve by name first and then by fingerprint, the
// same two-step install::run does; a reference that resolves to nothing is
// left out of the sources and named in `missing`, so the page can say which
// disc is wanted rather than failing a step later.
struct Prefill {
  Draft draft;
  std::vector<std::string> missing;
  // Set when the pack named an installer it had no business naming - an
  // absolute path, or one climbing out with "..". The draft's setup is cleared
  // and this holds what was asked for, so the wizard can say why it is asking
  // rather than silently starting somewhere else.
  std::string unsafe_setup;
};
Prefill draft_from_meta(const rt::Env& e, const Meta& m);

// What the vault remembers, put into a draft that arrived without a serial.
// Returns true when it filled something in.
//
// A serial goes into no pack (keys.h says why), so a recipe knows everything
// about a game except the one thing its installer will ask for out loud. The
// wizard's identity page looks the key up while you are looking at that page -
// but a rebuild does not stop there, it goes straight to the install, and the
// step that shows the serial beside the installer had nothing to show for
// exactly the case that most needed it. Prefilling is not typing it in: the
// number is put on screen next to the box asking for it, and the person types.
bool prefill_serial(Draft& d, const std::vector<StoredKey>& keys);

// Where a running install stages itself: cache_dir()/install-<id>, with each
// disc at work/drive-<letter>. cmd_swap_game hardcodes both - that is how
// `kretro swap <id> <n>` finds an install it did not start - so this layout is
// part of the CLI's contract and not an implementation detail.
std::filesystem::path staging_dir(const std::string& id);

// "Example Game: The Sequel" -> "example-game-the-sequel".
std::string slug(std::string_view name);

// "SafeDisc" or "SecuROM" for a file only those protections put beside a game,
// and nothing for any other name; any case. The wizard names what it finds on
// its build step and the Bundles page puts it on its check list, and one list
// of signs keeps the two from disagreeing about the same game.
std::string protection_of(std::string_view filename);

// Every directory the diff added, deepest first then by file count. Counts are
// cumulative over the subtree, which is why a game directory and its parent
// show the same number.
std::vector<Build::Candidate> rank_candidates(const std::filesystem::path& drive_c,
                                              const Tree::Diff& d);

// The one candidate a copy or unzip install has: the extracted tree itself,
// counted, with its executables ranked the same way an installed directory's
// are. Its `dir` is empty, because the tree is the game's directory and there
// is nothing under it to choose between.
Build::Candidate survey_tree(const std::filesystem::path& tree);

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
// Which entry of a ranked executable list the draft already names, or the list
// size when it names none. The mirror of setup_index, and there for the same
// reason: a manifest that says Gamew.exe is saying which of two executables
// is the game, and ranking by size answers GAME.EXE - the DOS build.
size_t exe_index(const std::vector<std::filesystem::path>& exes,
                 const std::filesystem::path& want);

size_t setup_index(const std::vector<SetupChoice>& setups,
                   const std::filesystem::path& setup);

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
// the path inside, and staged_setup enforces it on the one road both the CLI
// and the wizard take to a setup.
bool setup_path_is_safe(const std::string& setup);

// The file run_setup is actually handed, given where this install staged
// itself. mount_discs puts disc n at work/drive-<'d'+n-1>, so this is that
// join and the inverse of what step 3 wrote. An installer_exe's setup is
// already a path on this filesystem - nothing is mounted for it - and comes
// back unchanged.
std::filesystem::path staged_setup(const std::filesystem::path& work, const Draft& d);

// The method a page can actually be editing, given what the sources can run.
//
// methods_for is the list step 3 offers, and a draft arrives with a method
// from somewhere else: a manifest, a preset, or the sources the user has since
// removed. A method that is not in the list is drawn as whichever entry the
// combo happens to land on while every edit below it and the Go button run the
// other one - so it is clamped to the offer instead, once, wherever the offer
// changes.
Draft::Method clamp_method(const std::vector<Draft::Method>& offered, Draft::Method want);

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

// The bare .exe among a set of sources, if there is one: a repacked installer
// somebody downloaded, with no disc behind it and none wanted. open_sources
// deliberately opens no disc for one (build.cpp's BareExe case), which is why
// this is read off the classification rather than out of the disc set - and
// why a source set can be complete with nothing in discs() at all.
std::filesystem::path bare_exe(const std::vector<Build::Source>& sources);

// Whether step 1 has enough to go on. A disc set is enough; so is that bare
// .exe, on its own, and refusing it is what made Draft::Method::InstallerExe
// unreachable from the wizard - the fourth method offered on step 3 and
// selectable from nowhere.
bool sources_are_enough(const std::vector<Build::Source>& sources, size_t discs);

// The methods these sources can actually run, in the order step 3 offers them.
// Three of the four need a disc and the fourth needs the bare .exe, so
// offering all four always is offering a failure three steps later. Never
// empty: with nothing readable at all it is the one method that wants a disc,
// which is the honest thing to be asking for.
std::vector<Draft::Method> methods_for(const std::vector<Build::Source>& sources,
                                       size_t discs);

// Executables in `dir`, ranked, relative to `dir`. `prefer_setup` is the disc
// side: it also looks one directory down and puts setup, then install, then
// autorun first. Uninstallers are last on both sides, whatever their size.
std::vector<std::filesystem::path> rank_executables(const std::filesystem::path& dir,
                                                    bool prefer_setup);

// Entries under a directory, counted recursively - files and directories both,
// because a count of entries under drive_c is what a person watching an
// installer sees moving. Zero for a directory that is not there.
size_t count_entries(const std::filesystem::path& dir);

// Where the installer wrote, under a staging directory: the C: of the throwaway
// prefix the install ran in.
//
// Seven places walk it, diff it, rank what is under it or move the game out of
// it, and install::run's manifest path is an eighth. All of them ask here, so
// that "where an install stages itself" is one sentence rather than eight
// spellings of the same three path components - which is what it was, and this
// function had no caller at all.
std::filesystem::path staging_drive_c(const std::filesystem::path& work);

// At most five entries: the executable, then the largest regular files at the
// top level. Every entry is a requirement detect_install_dir must satisfy
// simultaneously, so a hundred-entry list would make a rebuild fragile for no
// gain.
std::vector<std::string> verify_list(const std::filesystem::path& dir,
                                     const std::filesystem::path& exe);

// The list a draft's pack should carry. The one the draft already has, when it
// has one: step 6 reads it off the confirmed directory the moment the user
// picks the executable, which is the last moment the tree is exactly what they
// were looking at. Otherwise one read now, off `installed_root` joined with the
// draft's install_dir - which for a copy or unzip install is the tree itself,
// because there the tree is the game.
//
// One answer rather than two. Build::write used to take the draft's list
// through draft_to_meta and then compute another over the top of it, so the
// field existed, was filled, travelled, and was thrown away on arrival.
std::vector<std::string> verify_for(const Draft& d,
                                    const std::filesystem::path& installed_root);

// Resolves a possibly-nested, possibly-miscased path under a directory. A disc
// spells its own names however it likes, and a manifest cannot know.
std::filesystem::path resolve_path_ci_public(const std::filesystem::path& root,
                                             const std::string& want);

// The reference form the resolvers understand: "archive#LABEL".
std::string disc_ref_for(const disc::Disc& d);

// Draft plus the assembled discs -> the Meta that gets packed.
Meta draft_to_meta(const Draft& d, const std::vector<disc::Disc>& discs);

// A local manifest that recognises the discs already on the table.
//
// games/*.toml is never required and never consulted unless it matches
// something the user has already brought in. A manifest exists because
// somebody sat with a disc and found out what its installer is called, which of
// two executables is the game, and which Windows version its installer wants.
// That is knowledge worth offering. It is worth nothing as a gate, which is
// what it used to be: a disc nobody had written a manifest for could not be
// installed at all.
struct Preset {
  std::string id, name;
  std::filesystem::path manifest;
  size_t matched = 0;           // how many of its discs are here
  size_t of = 0;                // how many it names
  bool by_fingerprint = false;  // identity, rather than a name that agrees
};

// How much of one manifest the assembled set accounts for. `from` is only
// carried through so the caller can load it again if the offer is accepted.
Preset score_preset(const Meta& manifest, const std::filesystem::path& from,
                    const std::vector<disc::Disc>& discs);

// Every manifest that recognises this set, strongest first. An empty vector
// means "we know nothing about these discs", which is an ordinary answer and
// not an error.
std::vector<Preset> match_presets(const rt::Env& e, const std::vector<disc::Disc>& discs);

// Copies into a draft what a manifest knows: id, name, year, setup, method,
// member, subdir, exe, args, width, height, windows_version, dgvoodoo. Every
// one stays editable.
//
// The method is among them, and the two fields that go with it. It was left
// out once, on the grounds that the wizard already knows whether it is looking
// at a disc or a bare .exe and a manifest disagreeing would be a guess
// overriding a fact - but the method is not that fact. A manifest may say
// method="unzip", member="Data2.zip", subdir="Example Game 1.02", and a disc
// is exactly what it wants: the manifest exists to say that the compilation
// disc carries several games as zips and which of them this one is. Leaving it
// out prefilled that title as an installer disc naming nothing, which is not a
// thinner answer than the manifest's but a wrong one.
//
// What the wizard does know better is what is *possible*: step 3 offers only
// the methods these sources can run, and clamp_method puts a preset that names
// a method not on offer back onto one that is. That is where the fact wins,
// and it wins after the copy rather than instead of it.
void apply_preset(const Meta& manifest, Draft* d);

}  // namespace kg::install
