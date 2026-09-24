#include "wizard.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include "../../install/collection.h"
#include "../../install/keys.h"
#include "../../install/setup_ref.h"
#include "../../install/staging.h"
#include "../../util/paths.h"
#include "../job_modal.h"
#include "../palette.h"
#include "words.h"

namespace kg::gui {
namespace fs = std::filesystem;

using wizard_detail::set_buf;

Wizard::Wizard(const rt::Env& e, SDL_Renderer* ren, Fonts fonts)
    : env_(e), ren_(ren), big_(fonts.big) {}

Wizard::~Wizard() {
  // The worker holds a Build whose destructor removes gigabytes of staging
  // tree. Letting the thread outlive us would have it writing into a
  // half-destroyed Wizard, and letting it be detached would leave the tree.
  if (build_) build_->cancel();
  // Nav is a process-wide flag; leaving it off would make the shelf
  // unnavigable after the wizard closed.
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  job_.join();
}

void Wizard::run_job(const std::string& title, std::function<void()> job, Step next,
                     bool show_modal) {
  if (job_.running()) return;
  modal_ = show_modal;
  next_ = next;
  // A new job supersedes the last one's complaint: the banner is about what is
  // happening now, not about the attempt before it.
  trouble_.clear();
  job_.start(title, std::move(job));
}

void Wizard::pump_job() {
  if (!job_.running() || !job_.finished()) return;
  // The modal's own Close button does the joining - except when the job is
  // being abandoned, where there is nobody left to press it: the window is on
  // its way shut and the wizard is leaving the moment the worker stops.
  if (modal_ && !abandoning_) return;
  finish_job();
}

void Wizard::finish_job() {
  std::string failed = job_.join();
  if (step_ == Step::Installing) leave_installing();
  // Said on whatever page this lands on. The install step has no modal at all
  // and the modal steps close theirs on the way out, so a failure that is only
  // in the log is a failure nobody ever reads: without this the user lands
  // back on step 3 with the screen exactly as they left it and no word about
  // why.
  trouble_ = failed;
  if (trouble_.empty()) {
    step_ = next_;
    return;
  }
  // The one failure with a page of its own. An install that ran and wrote
  // nothing leaves candidates() empty, which is what step 5 draws "the
  // installer wrote nothing to C:" from - the usual cause is having launched a
  // DemoShield front end rather than the setup, and that page offers the list
  // of executables again. Every other failure stays on the step it happened
  // on, where the banner says what went wrong and the button that started it
  // is still there to press again.
  if (next_ == Step::Where) step_ = Step::Where;
}

void Wizard::begin() {
  draft_ = install::Draft{};
  step_ = Step::Sources;
  probed_ = false;
  build_.reset();
  classified_.clear();
  known_as_.clear();
  pending_.clear();
  missing_.clear();
  status_.clear();
  trouble_.clear();
  set_buf(id_buf_, sizeof(id_buf_), "");
  set_buf(name_buf_, sizeof(name_buf_), "");
  set_buf(serial_buf_, sizeof(serial_buf_), "");
  set_buf(args_buf_, sizeof(args_buf_), "");
  // subdir and member are the copy and unzip recipes, and they are buffers
  // like the rest: what_page writes draft_ back out of them on every frame it
  // draws, so one left over from a previous run would install the last game's
  // Data2.zip out of this game's disc.
  set_buf(subdir_buf_, sizeof(subdir_buf_), "");
  set_buf(member_buf_, sizeof(member_buf_), "");
  year_ = 0;
  setups_.clear();
  setups_scanned_ = false;
  setup_pick_ = 0;
  // A fresh wizard has nothing recognised yet. begin_from fills draft_.sources
  // without going through add_source, so this is the reset that covers it.
  presets_.clear();
  presets_computed_ = false;
  preset_settled_ = false;
  preset_index_ = 0;
  expect_root_set_ = false;
  browse_dir_ = install::iso_dir();

  // A killed process, or a machine that lost power, leaves its staging tree
  // under cache_dir() - several gigabytes that nothing else will ever remove,
  // because ~Build is the only thing that removes one. open_layers already
  // clears stale mounts on the way in; this is the same courtesy.
  stale_.clear();
  stale_bytes_ = 0;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(cache_dir(), ec)) {
    if (!de.is_directory(ec)) continue;
    if (de.path().filename().string().rfind("install-", 0) != 0) continue;
    stale_ = de.path();
    for (const auto& f : fs::recursive_directory_iterator(stale_, ec)) {
      if (f.is_regular_file(ec)) stale_bytes_ += f.file_size(ec);
    }
    break;
  }
}

void Wizard::begin_from(const install::Prefill& p) {
  begin();
  draft_ = p.draft;
  missing_ = p.missing;
  // The serial the vault remembers, before the buffers are seeded from the
  // draft. A pack carries none, so this is the only way a rebuild - which
  // never passes the identity page that would look it up - reaches the install
  // step with the number to show beside the installer asking for it.
  install::prefill_serial(draft_, install::load_keys(install::keys_file()));
  // Every buffer this function is responsible for, rather than the four that
  // were easy to remember. A manifest whose recipe names Data2.zip and a
  // directory inside it arrives here with both in the draft, and step 3 writes
  // draft_.member and draft_.subdir back out of these buffers the first frame
  // it draws - so an unseeded buffer does not fail to prefill, it erases.
  set_buf(id_buf_, sizeof(id_buf_), draft_.id);
  set_buf(name_buf_, sizeof(name_buf_), draft_.name);
  set_buf(serial_buf_, sizeof(serial_buf_), draft_.serial);
  set_buf(args_buf_, sizeof(args_buf_), draft_.args);
  set_buf(subdir_buf_, sizeof(subdir_buf_), draft_.subdir);
  set_buf(member_buf_, sizeof(member_buf_), draft_.member);
  year_ = static_cast<int>(draft_.year);
  for (const fs::path& s : draft_.sources) {
    classified_.push_back(install::classify_source(s));
  }
}

void Wizard::begin_from_recipe(const install::Prefill& p, const Hash& expect_root) {
  begin_from(p);
  expect_root_set_ = true;
  expect_root_ = expect_root;
  // A recipe that named an installer it had no business naming does not get
  // to skip the question. draft_from_meta cleared the setup and kept what was
  // asked for; stop on step 3 and say so, rather than starting an install with
  // a blank setup or - worse - the path that was refused.
  if (!p.unsafe_setup.empty()) {
    status_ = "This recipe asks to run " + p.unsafe_setup +
              ", which is not a path on a disc it came with. Choose the installer yourself.";
    step_ = Step::What;
    return;
  }
  // Nothing left to ask: a recipe already knows its identity, its method and
  // what runs the game. What it cannot know is what the installer will ask.
  start_the_install();
}

void Wizard::add_source(const fs::path& p) {
  if (step_ != Step::Sources) return;
  // Not while a job is running. Everything below resets build_, and the worker
  // reading the discs is inside that Build: dropping a second disc on the
  // window while the first was being read destroyed the object mid-probe. It
  // is held instead, and taken up the moment the job is done.
  if (job_.running()) {
    if (std::find(pending_.begin(), pending_.end(), p) == pending_.end()) pending_.push_back(p);
    return;
  }
  if (std::find(draft_.sources.begin(), draft_.sources.end(), p) != draft_.sources.end()) return;
  draft_.sources.push_back(p);
  classified_.push_back(install::classify_source(p));
  probed_ = false;
  build_.reset();
  setups_.clear();
  setups_scanned_ = false;
  // A new source may be the disc that makes a manifest recognise the set, and
  // an offer already declined was declined about a different set.
  presets_computed_ = false;
  preset_settled_ = false;
  preset_index_ = 0;
}

void Wizard::take_pending_sources() {
  if (job_.running() || pending_.empty()) return;
  std::vector<fs::path> queued;
  queued.swap(pending_);
  for (const fs::path& p : queued) add_source(p);
}

void Wizard::ask_abandon() { confirm_abandon_ = true; }

bool Wizard::back() {
  if (job_.running() && step_ == Step::Installing) { confirm_abandon_ = true; return true; }
  if (job_.running()) return true;   // a job is up; the modal owns the screen
  switch (step_) {
    case Step::Sources: return false;   // not ours: the caller drops to the shelf
    case Step::Identity: step_ = Step::Sources; return true;
    case Step::What: step_ = Step::Identity; return true;
    case Step::Installing: confirm_abandon_ = true; return true;
    case Step::Where: step_ = Step::What; return true;
    case Step::Runs: step_ = Step::Where; return true;
    case Step::Presentation: step_ = Step::Runs; return true;
    case Step::Build: step_ = Step::Presentation; return true;
    case Step::Done: return false;
  }
  return false;
}

WizardOutcome Wizard::draw() {
  switch (step_) {
    case Step::Sources: sources_page(); break;
    case Step::Identity: identity_page(); break;
    case Step::What: what_page(); break;
    case Step::Installing: installing_page(); break;
    case Step::Where: where_page(); break;
    case Step::Runs: runs_page(); break;
    case Step::Presentation: presentation_page(); break;
    case Step::Build: build_page(); break;
    case Step::Done: done_page(); break;
  }
  // The busy modal owns the screen for as long as it is up. Two popups opened
  // at the same level do not stack - the second closes the first - so raising
  // the abandon question from here while a modal job ran opened it and shut it
  // on the same frame, and the window could not be closed at all while discs
  // were being read or a pack was being built. The modal carries the question
  // itself instead, and this is what the other steps get.
  if (job_.running() && modal_) modal();
  else abandon_popup();
  pump_job();
  // Whatever was dropped while that job ran, now that build_ is nobody's but
  // ours again.
  take_pending_sources();
  if (abandoning_ && !job_.running()) {
    abandoning_ = false;
    build_.reset();
    status_ = "install abandoned";
    return closed();
  }
  if (step_ == Step::Done && finished_) return closed();
  // Still up. The Library request is taken only on a frame the wizard stays
  // up, so one raised on a frame it closed waits for the next frame it is up.
  WizardOutcome out;
  if (want_library_) {
    want_library_ = false;
    out.kind = WizardOutcome::WantsLibrary;
  }
  return out;
}

WizardOutcome Wizard::closed() const {
  WizardOutcome out;
  // The done page's "Play it" says which game as "play:<id>".
  if (status_.rfind("play:", 0) == 0) {
    out.kind = WizardOutcome::Play;
    out.game_id = status_.substr(5);
  } else {
    out.kind = WizardOutcome::Closed;
    out.status = status_;
  }
  return out;
}

void Wizard::modal() {
  JobModalHooks hooks;
  hooks.footer_while_running = [this] {
    if (confirm_abandon_) {
      // Closing the window during a job asks here rather than in a popup of
      // its own, because this one is already open and there is only ever one.
      // "stop" beside it means the same thing to the worker; the difference is
      // that this one also leaves.
      ImGui::TextWrapped(
          "Abandon this? What is running is stopped and its staging tree removed. Nothing "
          "goes into your library.");
      ImGui::Spacing();
      if (ImGui::Button("Abandon it")) {
        if (build_) build_->cancel();
        abandoning_ = true;
        confirm_abandon_ = false;
      }
      ImGui::SameLine();
      if (ImGui::Button("Keep going")) confirm_abandon_ = false;
    } else {
      ImGui::TextDisabled("working...");
      ImGui::SameLine();
      if (ImGui::SmallButton("stop")) { if (build_) build_->cancel(); }
    }
  };
  // A question the job answered by finishing. Left standing it would put
  // "Abandon?" up over a step that had already succeeded.
  hooks.on_finished_frame = [this] { confirm_abandon_ = false; };
  hooks.on_close = [this] { finish_job(); };
  draw_job_modal(job_, big_, hooks);
}

void Wizard::trouble_banner() {
  if (trouble_.empty()) return;
  ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
  ImGui::TextWrapped("%s", trouble_.c_str());
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

void Wizard::abandon_popup() {
  if (confirm_abandon_) {
    ImGui::OpenPopup("Abandon?");
    confirm_abandon_ = false;
  }
  if (!ImGui::BeginPopupModal("Abandon?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::Text("Abandon this install?");
  ImGui::TextWrapped(
      "The installer is stopped and its staging tree removed. Nothing goes into your "
      "library, and you start again from the disc.");
  ImGui::Spacing();
  if (ImGui::Button("Abandon it")) {
    if (build_) build_->cancel();
    abandoning_ = true;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Keep going")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// Everything between "go" and the first frame of the installer, on the worker:
// a wineboot, a disc extraction per disc, and a walk of drive_c and the
// registry. snapshot_before gets a progress line of its own rather than
// appearing to hang.
void Wizard::start_the_install() {
  setups_.clear();
  setups_scanned_ = false;
  files_ = 0;
  drive_pick_.clear();
  drive_trouble_.clear();
  stage_.reset();
  stage_dead_ = false;
  stage_trouble_.clear();
  { std::lock_guard<std::mutex> lk(display_mutex_); display_.clear(); }
  if (!build_) {
    work_ = install::staging_dir(draft_.id);
    build_ = std::make_unique<install::Build>(env_, work_,
                                              [this](const std::string& l) { job_.log(l); });
  }
  // One spelling of where this install is staging itself. The Build is the
  // one that knows - a rehome on step 2 moved it - and the mount points the
  // setup path is joined onto below are under it.
  work_ = build_->work_dir();
  install::Draft d = draft_;

  // Copy and unzip are not an install anybody attends. Nothing is mounted, no
  // prefix is prepared, no compositor is raised and there is no dialog to
  // click Next in: the game is already installed, on the disc, and the work is
  // moving it. So they are an ordinary job with the ordinary log modal, and
  // they go from step 3 straight to step 5. Games that are already installed on
  // their disc, or zipped on it, arrive this way; the installer path cannot
  // add them at all.
  if (d.method == install::Draft::Method::Copy || d.method == install::Draft::Method::Unzip) {
    std::string title = (d.method == install::Draft::Method::Copy ? "Copying " : "Unpacking ") +
                        draft_.name;
    run_job(title, [this, d] {
      if (build_->discs().empty()) build_->open_sources(d.sources);
      if (d.method == install::Draft::Method::Copy) {
        build_->copy_from_disc(d.subdir);
      } else {
        build_->unzip_from_disc(d.member, d.subdir);
      }
      // What step 5 asks is answered by the method rather than by a diff: no
      // installer ran, nothing was written to C:, and what landed is the tree.
      build_->survey_extracted();
    }, Step::Where);
    return;
  }

  step_ = Step::Installing;
  // No modal here: a true ImGui modal would cover the stage, and the stage is
  // the whole point of this step. Progress goes into the strip under the
  // drives instead.
  run_job("Installing " + draft_.name, [this, d] {
    if (build_->discs().empty()) build_->open_sources(d.sources);
    job_.log("preparing the prefix as " + d.windows_version);
    build_->prepare_prefix(d.windows_version);
    job_.log("mounting the discs");
    build_->mount_discs();
    job_.log("reading what is on C: before the installer runs");
    build_->snapshot_before();
    // run_setup runs what it is given from the directory it is in, so it wants
    // a path on this filesystem. A Draft's setup is already one for
    // InstallerExe; for Installer it is "<n>/<path on that disc>", and the
    // discs are mounted at work/drive-d, work/drive-e, ... in the order
    // open_sources assembled the set. staged_setup is that join, and it knows
    // that only a leading run of digits is a disc number. Reading everything
    // before the first slash as one would turn a setup at Game3/Setup.exe
    // into a Setup.exe at the root of a disc that has none.
    const fs::path setup = install::staged_setup(work_, d);
    // Headless Weston: nobody sees a second window, and the frames come to us
    // instead. The display arrives from inside run_in_compositor the moment
    // Xwayland is confirmed up, which is the whole of the coupling.
    build_->run_setup(setup, /*headless=*/true, [this](const std::string& disp) {
      std::lock_guard<std::mutex> lk(display_mutex_);
      display_ = disp;
    });
    job_.log("working out what it wrote");
    build_->diff_after();
  }, Step::Where, /*show_modal=*/false);
}
void Wizard::leave_installing() {
  stage_.reset();
  stage_dead_ = false;
  { std::lock_guard<std::mutex> lk(display_mutex_); display_.clear(); }
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
}

}  // namespace kg::gui
