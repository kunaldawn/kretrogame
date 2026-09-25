#include "wizard.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
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
#include "../widgets.h"
#include "words.h"

namespace kg::gui {
namespace fs = std::filesystem;

using wizard_detail::set_buf;

namespace {

// The steps by the short names the rail and the status bar's path give them,
// in Step's order.
const char* const kStepNames[] = {"sources", "name", "what", "install", "where",
                                  "runs", "looks", "build", "done"};

}  // namespace

Wizard::Wizard(const rt::Env& e, SDL_Renderer* ren, Fonts fonts)
    : env_(e), ren_(ren), fonts_(fonts) {}

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

bool Wizard::step_before(Step s, Step* before) {
  switch (s) {
    case Step::Identity: *before = Step::Sources; return true;
    case Step::What: *before = Step::Identity; return true;
    case Step::Where: *before = Step::What; return true;
    case Step::Runs: *before = Step::Where; return true;
    case Step::Presentation: *before = Step::Runs; return true;
    case Step::Build: *before = Step::Presentation; return true;
    case Step::Sources:
    case Step::Installing:
    case Step::Done: return false;
  }
  return false;
}

bool Wizard::back() {
  if (job_.running() && step_ == Step::Installing) { confirm_abandon_ = true; return true; }
  if (job_.running()) return true;   // a job is up; the modal owns the screen
  switch (step_) {
    case Step::Sources: return false;   // not ours: the caller drops to the shelf
    case Step::Installing: confirm_abandon_ = true; return true;
    case Step::Done: return false;
    default: break;
  }
  return step_before(step_, &step_);
}

bool Wizard::can_go_back_to(Step to) const {
  if (job_.running()) return false;
  Step s = step_;
  while (s != to) {
    if (!step_before(s, &s)) return false;
  }
  return true;
}

WizardOutcome Wizard::draw() {
  // The status bar's path would otherwise be the page title made into a
  // slug, and the titles here are questions.
  set_chrome_path(std::string("~/wizard/") + kStepNames[static_cast<int>(step_)]);
  // The header names the flow, and each step heads its own content with its
  // title; the install step, which gives the whole page to the installer,
  // says its title in the header instead.
  set_page_trail(step_ == Step::Installing ? "kretro \xe2\x80\xba add a game \xe2\x80\xba installing " + draft_.name
                                           : std::string("kretro \xe2\x80\xba add a game"));
  // The install step turns the keyboard's nav off for the installer; every
  // other step has it, however the install step was left.
  if (step_ != Step::Installing) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
      // The same pair as the abandon popup, marked the same way: the one that
      // throws work away in the error colour, the safe one as the primary,
      // which Escape and the pad's B answer.
      if (dialog_button("Abandon it", DialogButton::Danger)) {
        if (build_) build_->cancel();
        abandoning_ = true;
        confirm_abandon_ = false;
      }
      if (dialog_button("Keep going", DialogButton::Primary, true)) confirm_abandon_ = false;
    } else {
      // On the line of the button beside it, rather than at the top of it.
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("working...");
      ImGui::SameLine();
      if (dialog_button("stop")) {
        if (build_) build_->cancel();
      }
    }
  };
  // A question the job answered by finishing. Left standing it would put
  // "Abandon?" up over a step that had already succeeded.
  hooks.on_finished_frame = [this] { confirm_abandon_ = false; };
  hooks.on_close = [this] { finish_job(); };
  draw_job_modal(job_, fonts_.big(), hooks);
}

void Wizard::trouble_banner() {
  if (trouble_.empty()) return;
  badge_line(BadgeKind::Fail, trouble_, kWarn);
  vgap(4);
}

// Copy and unzip never visit "install", and it is lit as passed all the same:
// the stepper is a map of the wizard, not a log of this run.
void Wizard::step_top() {
  constexpr int n = 9;
  const int at = static_cast<int>(step_);
  nav_section_begin("stepper");
  const int picked = stepper(
      "steps", kStepNames, n, at, [at](int i) { return StepInfo{i < at ? StepState::Done : StepState::Todo, 0}; },
      [this](int i) { return can_go_back_to(static_cast<Step>(i)); }, step_ == Step::Installing);
  nav_section_end();
  // A step further back is Escape pressed until it is there, through the same
  // code, so it goes nowhere Escape would not.
  if (picked >= 0) {
    const Step to = static_cast<Step>(picked);
    for (int i = 0; i < n && step_ != to && back();) ++i;
  }
  vgap(6);
}

float Wizard::footer_block() const { return flow_footer_height() + ImGui::GetStyle().ItemSpacing.y; }

const char* Wizard::step_title(const std::string& title) {
  heading_ = title;
  return heading_.c_str();
}

void Wizard::begin_body(float below) {
  nav_section_begin("content");
  // The scrollbar's grab in the accent's dim green rather than the rules'
  // grey, so a body with more below the fold says so. Read as the child
  // begins, so it is popped straight away.
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, kAccentDim);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, alpha(kAccent, 0.7f));
  // Flattened, so the pad and the keyboard move between the body and the
  // footer's button as though they were one window, as they were.
  ImGui::BeginChild("##body", ImVec2(0, -(footer_block() + below)), ImGuiChildFlags_NavFlattened);
  ImGui::PopStyleColor(2);
  // Centred as though the scrollbar were not there, so the column lines up
  // with anything the step keeps between the body and the footer.
  body_indent_ = ImGui::GetScrollMaxY() > 0 ? ImGui::GetStyle().ScrollbarSize : 0.0f;
  if (body_indent_ > 0) ImGui::Indent(body_indent_);
  centre_column(content_max_w(Content::Form));
  step_heading(heading_);
  // The step opens on its first control; a step that says otherwise says so
  // further down.
  default_focus_next();
}

void Wizard::end_body() {
  end_centre_column();
  if (body_indent_ > 0) ImGui::Unindent(body_indent_);
  // A body cut off at its bottom edge cuts a line of text through its middle,
  // which reads as a drawing fault. Fading the last of it into the page and
  // saying there is more makes the cut look meant, and says to scroll.
  const float left = ImGui::GetScrollMaxY() - ImGui::GetScrollY();
  if (left > 0.5f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float x0 = wp.x;
    const float x1 = wp.x + ImGui::GetWindowContentRegionMax().x;
    const float y1 = wp.y + ImGui::GetWindowSize().y;
    const float y0 = y1 - std::round(px(40));
    dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), u32(kBg0, 0.0f), u32(kBg0, 0.0f),
                                u32(kBg0), u32(kBg0));
    ImFont* f = fonts_.small();
    const float fs = font_px(f);
    // U+2193, in the small font's arrows.
    const char* more = f->FindGlyphNoFallback(0x2193) ? "\xe2\x86\x93 more" : "more";
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, more);
    dl->AddText(f, fs, ImVec2(std::round(x1 - ts.x - px(4)), std::round(y1 - ts.y - px(2))),
                u32(kDim), more);
  }
  ImGui::EndChild();
  vgap(4);
}

bool Wizard::step_footer(const char* label, bool enabled, bool is_default,
                         const std::function<void(float)>& extra) {
  nav_section_end();
  nav_section_begin("footer");
  // The footer's top, as flow_footer finds it: the foot of the page.
  const float top = std::max(ImGui::GetCursorScreenPos().y,
                             ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y - flow_footer_height());
  FlowFooter f;
  // Back does what Escape does here: a step back, or on the first step
  // leaving the wizard, which is the shelf's to do. On the last step Escape
  // goes nowhere a step back, so there is no Back.
  f.back = step_ == Step::Sources ? "Cancel" : step_ == Step::Done ? nullptr
                                                                   : "Back";
  f.primary = label;
  f.primary_enabled = enabled;
  f.primary_default = is_default;
  const FlowAction act = flow_footer(f);
  if (extra) extra(top);
  nav_section_end();
  if (act == FlowAction::Back) {
    if (!back()) press_escape();
  }
  return act == FlowAction::Primary;
}

float Wizard::prose_wrap() {
  const float measure = ImGui::CalcTextSize("0").x * 90.0f;
  return ImGui::GetCursorPosX() + std::min(measure, ImGui::GetContentRegionAvail().x);
}

void Wizard::abandon_popup() {
  if (confirm_abandon_) {
    ImGui::OpenPopup("Abandon?");
    confirm_abandon_ = false;
  }
  // A width of our own and a height that follows the text, headed by the
  // question.
  if (!dialog_begin("Abandon?", 620, "Abandon?")) return;
  badge(BadgeKind::Warn);
  ImGui::SameLine(0, badge_gap());
  ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
  ImGui::TextUnformatted("Abandon this install?");
  ImGui::PopStyleColor();
  vgap(2);
  ImGui::TextWrapped(
      "The installer is stopped and its staging tree removed. Nothing goes into your "
      "library, and you start again from the disc.");
  dialog_footer();
  // The one that throws the install away in the error colour, and staying
  // put as the primary: the two must not look like equals.
  if (dialog_button("Abandon it", DialogButton::Danger)) {
    if (build_) build_->cancel();
    abandoning_ = true;
    ImGui::CloseCurrentPopup();
  }
  // Going on is what Escape and the pad's B answer, and where the focus
  // starts: throwing an install away takes a deliberate press.
  if (dialog_button("Keep going", DialogButton::Primary, true)) ImGui::CloseCurrentPopup();
  dialog_end();
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
