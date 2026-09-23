#include "wizard.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../config/config.h"
#include "../install/iso.h"
#include "../install/keys.h"
#include "../util/hash.h"
#include "../util/paths.h"
#include "app.h"
#include "library.h"

namespace kg::gui {
namespace fs = std::filesystem;

namespace {

// "three discs" reads better than "3 discs" in a sentence, and a disc set is
// never large enough for the words to run out.
std::string discs_in_words(size_t n) {
  static const char* w[] = {"no", "one", "two", "three", "four", "five", "six", "seven", "eight"};
  std::string s = n < 9 ? std::string(w[n]) : std::to_string(n);
  return s + (n == 1 ? " disc" : " discs");
}

const char* kind_word(install::Build::Source::Kind k) {
  switch (k) {
    case install::Build::Source::Kind::DiscImage: return "disc image";
    case install::Build::Source::Kind::Archive:   return "archive";
    case install::Build::Source::Kind::Directory: return "folder";
    case install::Build::Source::Kind::BareExe:   return "an installer, not a disc";
    case install::Build::Source::Kind::Unreadable: return "unreadable";
  }
  return "";
}

// How step 3 says each method out loud. The list of methods on offer is
// install::methods_for - what these sources can actually run - and this is the
// sentence for each one of them.
const char* method_word(install::Draft::Method m) {
  switch (m) {
    case install::Draft::Method::Installer:    return "run an installer on the disc";
    case install::Draft::Method::Copy:         return "copy the files off the disc";
    case install::Draft::Method::Unzip:        return "extract an archive from the disc";
    case install::Draft::Method::InstallerExe: return "run the installer you dropped";
  }
  return "";
}

// A candidate directory, said out loud. A copy or an unzip has exactly one
// candidate and its directory is the extracted tree itself, which has no name
// to print: where it landed is answered by the method rather than by a path.
std::string where_word(const fs::path& dir) {
  return dir.empty() ? std::string("what came off the disc") : dir.generic_string();
}

void set_buf(char* buf, size_t n, const std::string& v) {
  std::snprintf(buf, n, "%s", v.c_str());
}

// SafeDisc and SecuROM read raw sectors below the filesystem, and a
// directory-backed CD-ROM drive cannot answer that. Naming it on the build
// step is better than a pack that builds cleanly and a game that will not
// start, and better than refusing to build - the pack is still worth having.
std::vector<std::string> protection_notes(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    std::string what = install::protection_of(de.path().filename().string());
    if (what.empty()) continue;
    std::string line = std::string("this game carries ") + de.path().filename().string() +
                       ", which is " + what
                       + ". The pack will be built and the game may still refuse to start; "
                         "a directory-backed CD-ROM drive cannot answer a raw-sector read.";
    if (std::find(out.begin(), out.end(), line) == out.end()) out.push_back(line);
  }
  return out;
}

}  // namespace

Wizard::Wizard(const rt::Env& e, SDL_Renderer* ren) : env_(e), ren_(ren) {}

Wizard::~Wizard() {
  // The worker holds a Build whose destructor removes gigabytes of staging
  // tree. Letting the thread outlive us would have it writing into a
  // half-destroyed Wizard, and letting it be detached would leave the tree.
  if (build_) build_->cancel();
  // Nav is a process-wide flag; leaving it off would make the shelf
  // unnavigable after the wizard closed.
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  if (worker_.joinable()) worker_.join();
}

void Wizard::set_fonts(ImFont* body, ImFont* big) { body_ = body; big_ = big; }

void Wizard::log_line(const std::string& l) {
  std::lock_guard<std::mutex> lk(log_mutex_);
  log_.push_back(l);
}

std::string Wizard::failure() {
  std::lock_guard<std::mutex> lk(log_mutex_);
  return failed_;
}

void Wizard::run_job(const std::string& title, std::function<void()> job, Step next,
                     bool show_modal) {
  if (busy_) return;
  busy_ = true;
  modal_ = show_modal;
  busy_title_ = title;
  next_ = next;
  { std::lock_guard<std::mutex> lk(log_mutex_); failed_.clear(); }
  // A new job supersedes the last one's complaint: the banner is about what is
  // happening now, not about the attempt before it.
  trouble_.clear();
  done_ = false;
  { std::lock_guard<std::mutex> lk(log_mutex_); log_.clear(); }
  worker_ = std::thread([this, job] {
    try {
      job();
    } catch (const std::exception& ex) {
      {
        std::lock_guard<std::mutex> lk(log_mutex_);
        failed_ = ex.what();
        log_.push_back(std::string("failed: ") + ex.what());
      }
    }
    done_ = true;
  });
}

void Wizard::pump_job() {
  if (!busy_ || !done_) return;
  // The modal's own Close button does the joining - except when the job is
  // being abandoned, where there is nobody left to press it: the window is on
  // its way shut and the wizard is leaving the moment the worker stops.
  if (modal_ && !abandoning_) return;
  finish_job();
}

void Wizard::finish_job() {
  if (worker_.joinable()) worker_.join();
  busy_ = false;
  if (step_ == Step::Installing) leave_installing();
  done_ = false;
  // Said on whatever page this lands on. The install step has no modal at all
  // and the modal steps close theirs on the way out, so a failure that is only
  // in the log is a failure nobody ever reads: the wizard used to drop the
  // user back on step 3 with the screen exactly as they left it and no word
  // about why.
  trouble_ = failure();
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
    classified_.push_back(install::Build::classify(s));
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
  if (busy_) {
    if (std::find(pending_.begin(), pending_.end(), p) == pending_.end()) pending_.push_back(p);
    return;
  }
  if (std::find(draft_.sources.begin(), draft_.sources.end(), p) != draft_.sources.end()) return;
  draft_.sources.push_back(p);
  classified_.push_back(install::Build::classify(p));
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
  if (busy_ || pending_.empty()) return;
  std::vector<fs::path> queued;
  queued.swap(pending_);
  for (const fs::path& p : queued) add_source(p);
}

bool Wizard::take_library_request() {
  bool v = want_library_;
  want_library_ = false;
  return v;
}

void Wizard::ask_abandon() { confirm_abandon_ = true; }

bool Wizard::back() {
  if (busy_ && step_ == Step::Installing) { confirm_abandon_ = true; return true; }
  if (busy_) return true;   // a job is up; the modal owns the screen
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

bool Wizard::draw(SDL_Window* win) {
  (void)win;
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
  if (busy_ && modal_) modal();
  else abandon_popup();
  pump_job();
  // Whatever was dropped while that job ran, now that build_ is nobody's but
  // ours again.
  take_pending_sources();
  if (abandoning_ && !busy_) {
    abandoning_ = false;
    build_.reset();
    status_ = "install abandoned";
    return false;
  }
  return step_ != Step::Done || !finished_;
}

void Wizard::modal() {
  ImGui::OpenPopup("busy");
  ImVec2 c = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(720, 380));
  if (ImGui::BeginPopupModal("busy", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::PushFont(big_);
    ImGui::TextUnformatted(busy_title_.c_str());
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 260), true);
    {
      std::lock_guard<std::mutex> lk(log_mutex_);
      for (const std::string& l : log_) ImGui::TextWrapped("%s", l.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    if (done_) {
      // A question the job answered by finishing. Left standing it would put
      // "Abandon?" up over a step that had already succeeded.
      confirm_abandon_ = false;
      if (ImGui::Button("Close", ImVec2(-1, 44))) {
        finish_job();
        ImGui::CloseCurrentPopup();
      }
    } else if (confirm_abandon_) {
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
    ImGui::EndPopup();
  }
}

void Wizard::trouble_banner() {
  if (trouble_.empty()) return;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
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

// Each page arrives in its own task; until then they are the page chrome and
// nothing else, so the translation unit compiles and links from the start.
void Wizard::sources_page() {
  begin_page("sources", "Esc back", big_);
  trouble_banner();

  if (!stale_.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s is a staging tree an install left behind - %s of it.",
                       stale_.filename().c_str(), human_size(stale_bytes_).c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("delete it")) {
      std::error_code ec;
      fs::remove_all(stale_, ec);
      stale_.clear();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("leave it")) stale_.clear();
    ImGui::Spacing();
  }

  if (classified_.empty()) {
    ImGui::TextWrapped(
        "Nothing here yet. Drop a disc image, an archive, a folder or a setup.exe on this "
        "window, or pick one below.");
    ImGui::TextDisabled(".iso, .bin with its .cue, .img, .mdf, .zip, .7z, a folder, or a bare .exe");
    ImGui::Spacing();
  }

  // What opening them found, over the top of what their names suggested.
  //
  // classify() looks at an extension and nothing else; open_sources is where a
  // truncated download, a zip with no disc inside it and a .cue whose .bin is
  // missing say so, and it records the sentence for each. Nothing read that
  // list, so step 1 went on describing a file as an archive while the log
  // modal above it scrolled past the reason it was not one. Adopted by
  // position, which is the order open_sources fills it in, and only once the
  // probe has finished - the worker is inside that vector until then.
  if (probed_ && build_) {
    const std::vector<install::Build::Source>& opened = build_->sources();
    for (size_t i = 0; i < opened.size() && i < classified_.size(); ++i) {
      if (opened[i].path != classified_[i].path) break;
      classified_[i] = opened[i];
    }
  }

  int remove_at = -1, move_up = -1;
  for (size_t i = 0; i < classified_.size(); ++i) {
    const install::Build::Source& s = classified_[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::Spacing();
    ImGui::Text("%s", s.path.filename().c_str());
    ImGui::SameLine(420);
    ImGui::TextDisabled("%s", kind_word(s.kind));
    ImGui::SameLine(ImGui::GetWindowWidth() - 200);
    if (i > 0 && ImGui::SmallButton("up")) move_up = static_cast<int>(i);
    ImGui::SameLine();
    if (ImGui::SmallButton("remove")) remove_at = static_cast<int>(i);

    if (s.kind == install::Build::Source::Kind::Unreadable) {
      // One source that will not open does not stop the others: a collection
      // with a truncated download in it is still a collection.
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
      ImGui::TextWrapped("    %s", s.trouble.c_str());
      ImGui::PopStyleColor();
    }

    // Only once the probe has finished. This page keeps drawing underneath the
    // log modal while "Reading your discs" runs, and open_sources clears
    // discs_ and rebuilds it - iterating it from here in the meantime is the
    // same dangling reference the install step had, one screen earlier.
    if (build_ && probed_) {
      int n = 0;
      for (size_t d = 0; d < build_->discs().size(); ++d) {
        const disc::Disc& disc = build_->discs()[d];
        if (disc.source != s.path) continue;
        ++n;
        ImGui::Text("    disc %d", n);
        ImGui::SameLine(140);
        ImGui::TextUnformatted(disc.label.empty() ? "-" : disc.label.c_str());
        ImGui::SameLine(330);
        ImGui::TextDisabled("%s", human_size(disc.info.size).c_str());
        ImGui::SameLine(430);
        ImGui::TextDisabled("%s", to_hex(disc.info.prefix).substr(0, 6).c_str());
        if (d < known_as_.size() && !known_as_[d].empty()) {
          ImGui::SameLine(520);
          ImGui::TextDisabled("%s", known_as_[d].c_str());
        }
      }
    }
    ImGui::PopID();
  }
  if (move_up > 0) {
    // Reordering the files reorders the discs, because open_sources runs the
    // probe pipeline per source in the order given and concatenates. Which is
    // the case that needs it: three separate .bin/.cue pairs handed over in
    // whatever order the file browser sorted them.
    std::swap(draft_.sources[move_up], draft_.sources[move_up - 1]);
    std::swap(classified_[move_up], classified_[move_up - 1]);
    probed_ = false;
    build_.reset();
    setups_.clear();
    setups_scanned_ = false;
  }
  if (remove_at >= 0) {
    draft_.sources.erase(draft_.sources.begin() + remove_at);
    classified_.erase(classified_.begin() + remove_at);
    probed_ = false;
    build_.reset();
    setups_.clear();
    setups_scanned_ = false;
  }

  // An installer somebody downloaded is a source in its own right and has no
  // disc behind it by definition. Step 1 has to let it through or the fourth
  // method is a branch nothing can reach.
  fs::path bare = install::bare_exe(classified_);

  if (probed_ && build_) {
    ImGui::Spacing();
    if (!build_->discs().empty()) {
      ImGui::TextWrapped("%s, one game.  Reorder the files with the arrows if the numbering "
                         "is wrong.", discs_in_words(build_->discs().size()).c_str());
    } else if (!bare.empty()) {
      ImGui::TextWrapped(
          "No disc, and none wanted: %s is an installer carrying its own game. It runs from "
          "where it is, with nothing mounted for it.",
          bare.filename().c_str());
    } else {
      ImGui::TextWrapped(
          "No disc came out of any of these. \"This zip holds no disc\" is a fact about your "
          "collection rather than about this install, and the Library screen is where facts "
          "about your collection live.");
      if (ImGui::Button("Open the Library")) want_library_ = true;
    }
  }

  ImGui::Spacing();
  if (ImGui::SmallButton(browsing_ ? "close the browser" : "pick a file")) browsing_ = !browsing_;
  if (browsing_) browser();

  ImGui::Spacing();
  ImGui::PushFont(big_);
  bool ready = probed_ && build_ &&
               install::sources_are_enough(classified_, build_->discs().size());
  bool can_read = !draft_.sources.empty() && !probed_;
  if (!ready && !can_read) ImGui::BeginDisabled();
  if (ImGui::Button(ready ? "Next: name it" : "Read these", ImVec2(-1, 60))) {
    if (ready) {
      if (build_->discs().empty()) {
        // Nothing but the .exe. The method is not a preference here, it is
        // what was dropped; and the path is absolute because draft_to_meta
        // records it as one, so a rebuild finds the installer wherever it was
        // downloaded to.
        std::error_code aec;
        draft_.method = install::Draft::Method::InstallerExe;
        draft_.setup = fs::absolute(bare, aec);
      }
      step_ = Step::Identity;
    } else {
      // Probing several gigabytes is minutes, not seconds, so it goes where
      // every long job in this program goes.
      //
      // "new" until step 2 names the game: the discs have to be opened before
      // there is anything to name them after. Leaving the tree called that is
      // what put the installer's frames in saves/new/ and hid the whole install
      // from `kretro swap`, so step 2 renames it - Build::rehome - the moment
      // the id is settled.
      work_ = install::staging_dir(draft_.id.empty() ? std::string("new") : draft_.id);
      build_ = std::make_unique<install::Build>(
          env_, work_, [this](const std::string& l) { log_line(l); });
      std::vector<fs::path> srcs = draft_.sources;
      run_job("Reading your discs", [this, srcs] {
        build_->open_sources(srcs);
        std::vector<iso::Known> db = iso::load_database(env_);
        std::vector<std::string> named;
        for (const disc::Disc& d : build_->discs()) {
          iso::Match m = iso::identify(db, d.info);
          std::string s = m.entry ? m.entry->name : "";
          if (m.suspect_bad_dump) s += "  (bytes differ - a bad dump?)";
          named.push_back(s);
        }
        std::lock_guard<std::mutex> lk(log_mutex_);
        known_as_ = named;
        probed_ = true;
      }, Step::Sources);
    }
  }
  if (!ready && !can_read) ImGui::EndDisabled();
  ImGui::PopFont();
  ImGui::End();
}
// The one place accumulated knowledge is allowed to speak.
//
// It is a line above the fields rather than a step of its own, because a step
// implies a decision that has to be made and this one does not: saying no
// leaves the user exactly where they already were, typing a name into a box.
// Nothing downstream asks whether a preset was taken.
void Wizard::preset_offer() {
  if (!presets_computed_) {
    presets_computed_ = true;
    if (build_) presets_ = install::match_presets(env_, build_->discs());
  }
  if (presets_.empty() || preset_settled_) return;
  if (preset_index_ >= presets_.size()) preset_index_ = 0;
  const install::Preset& p = presets_[preset_index_];

  ImGui::Spacing();
  ImGui::TextWrapped("This looks like %s. Use what we know about it?", p.name.c_str());
  if (p.by_fingerprint) {
    ImGui::TextDisabled("  these are the discs that manifest was written against");
  } else if (p.of > 1) {
    ImGui::TextDisabled("  %zu of its %zu discs are here, matched by label", p.matched, p.of);
  } else {
    ImGui::TextDisabled("  matched by disc label, not by fingerprint");
  }

  if (ImGui::Button("yes, prefill")) {
    try {
      install::apply_preset(install::load_manifest(p.manifest), &draft_);
      // The fields below draw from the text buffers and write draft_ back on
      // every change, so a preset that wrote only draft_ would be undone by the
      // first keystroke. These are the four the offer can fill.
      set_buf(id_buf_, sizeof(id_buf_), draft_.id);
      set_buf(name_buf_, sizeof(name_buf_), draft_.name);
      set_buf(args_buf_, sizeof(args_buf_), draft_.args);
      // And the two that pick a build. what_page writes draft_.subdir and
      // draft_.member back out of these buffers on its first frame, so leaving
      // them empty does not fail to prefill the preset - it erases it.
      set_buf(subdir_buf_, sizeof(subdir_buf_), draft_.subdir);
      set_buf(member_buf_, sizeof(member_buf_), draft_.member);
      year_ = static_cast<int>(draft_.year);
      // And the setup, which is why half of these manifests were written down
      // in the first place: step 3 ranks a list of every executable on the
      // disc and would otherwise answer the question again, differently.
      preselect_setup();
    } catch (const std::exception& ex) {
      // A manifest that will not load is this machine's problem, said out loud
      // rather than swallowed - and the fields below are still typeable.
      status_ = ex.what();
    }
    preset_settled_ = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("no, I will fill it in")) preset_settled_ = true;

  // A compilation's zip may be five discs with five manifests each naming one
  // of them, so more than one right answer is the normal case rather than a
  // corner.
  if (presets_.size() > 1) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::BeginCombo("##preset", presets_[preset_index_].name.c_str())) {
      for (size_t i = 0; i < presets_.size(); ++i) {
        bool sel = i == preset_index_;
        if (ImGui::Selectable(presets_[i].name.c_str(), sel)) preset_index_ = i;
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }
  ImGui::Spacing();
}

void Wizard::identity_page() {
  begin_page("name it", "Esc back", big_);

  preset_offer();

  // The disc says what it is called, more or less. A volume label is shouty
  // and abbreviated, so it is a first answer rather than an answer.
  if (name_buf_[0] == '\0' && build_ && !build_->discs().empty()) {
    std::string best = build_->discs()[0].label;
    for (size_t i = 0; i < build_->discs().size() && i < known_as_.size(); ++i) {
      if (!known_as_[i].empty()) { best = known_as_[i]; break; }
    }
    set_buf(name_buf_, sizeof(name_buf_), best);
    set_buf(id_buf_, sizeof(id_buf_), install::slug(best));
  }

  ImGui::TextDisabled("name");
  ImGui::SetNextItemWidth(560);
  if (ImGui::InputText("##name", name_buf_, sizeof(name_buf_))) {
    // The id follows the name until the id is edited by hand, at which point
    // it stops following: renaming a game should not silently re-home its
    // saves under a different id.
    if (draft_.id == install::slug(draft_.name)) {
      set_buf(id_buf_, sizeof(id_buf_), install::slug(name_buf_));
    }
  }
  draft_.name = name_buf_;

  ImGui::TextDisabled("id");
  ImGui::SetNextItemWidth(560);
  ImGui::InputText("##id", id_buf_, sizeof(id_buf_));
  draft_.id = id_buf_;
  ImGui::TextDisabled("the name of its pack, its saves and its Wine prefix");

  ImGui::TextDisabled("year");
  ImGui::SetNextItemWidth(160);
  if (ImGui::InputInt("##year", &year_)) {
    if (year_ < 0) year_ = 0;
  }
  if (year_ == 0 && build_ && !build_->discs().empty()) {
    // The primary volume descriptor records when the disc was mastered, which
    // is the game's year often enough to be worth offering and never worth
    // trusting.
    const std::string& c = build_->discs()[0].info.created;
    if (c.size() >= 4) {
      int y = std::atoi(c.substr(0, 4).c_str());
      if (y >= 1980 && y <= 2010) year_ = y;
    }
  }
  draft_.year = static_cast<uint32_t>(year_ < 0 ? 0 : year_);

  install::IdClash clash = install::id_clash(env_, draft_.id);
  if (clash.any()) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s already names %s on this machine.", draft_.id.c_str(),
                       clash.sentence().c_str());
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "That may be exactly right - rebuilding the game you already have replaces its pack "
        "and keeps its saves. If this is a different game, give it a different id; the "
        "wizard will not quietly rename it for you, because overwriting a game is a "
        "decision rather than an accident.");
    std::string alt = install::next_free_id(env_, draft_.id);
    if (ImGui::SmallButton(("use " + alt + " instead").c_str())) {
      set_buf(id_buf_, sizeof(id_buf_), alt);
      draft_.id = alt;
    }
  }

  ImGui::Spacing();
  ImGui::TextDisabled("serial, if the installer will ask for one");
  ImGui::SetNextItemWidth(560);
  if (serial_buf_[0] == '\0' && !draft_.id.empty()) {
    std::string known = install::key_for(install::load_keys(install::keys_file()), draft_.id);
    if (!known.empty()) set_buf(serial_buf_, sizeof(serial_buf_), known);
  }
  ImGui::InputText("##serial", serial_buf_, sizeof(serial_buf_));
  draft_.serial = serial_buf_;
  ImGui::TextDisabled(
      "kept on this machine and shown back to you next to the installer that asks for it. "
      "It goes into no pack.");

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (draft_.id.empty() || draft_.name.empty()) ImGui::BeginDisabled();
  if (ImGui::Button("Next: what to run", ImVec2(-1, 60))) {
    if (!draft_.serial.empty()) {
      std::vector<install::StoredKey> keys = install::load_keys(install::keys_file());
      install::put_key(keys, draft_.id, draft_.serial, "typed into the wizard");
      install::save_keys(install::keys_file(), keys);
    }
    // The staging tree is named after the id, which is the layout
    // `kretro swap <id> <n>` addresses; it can only be named once the id is,
    // and step 1 had to open the discs before this page could offer a name for
    // them. So the tree follows the id here - and the installer's journal with
    // it, into saves/<id>/ rather than saves/new/.
    if (build_) {
      trouble_.clear();
      if (!build_->rehome(draft_.id)) {
        trouble_ = "the staging tree kept the name it was made with. The install goes on, "
                   "but `kretro swap` will not find it and its frames are filed under that "
                   "name rather than " + draft_.id + ".";
      }
      work_ = build_->work_dir();
    }
    step_ = Step::What;
  }
  if (draft_.id.empty() || draft_.name.empty()) ImGui::EndDisabled();
  ImGui::PopFont();
  ImGui::End();
}
void Wizard::preselect_setup() {
  size_t i = install::setup_index(setups_, draft_.setup);
  if (i < setups_.size()) setup_pick_ = i;
}

void Wizard::what_page() {
  begin_page("what runs the install?", "Esc back", big_);
  trouble_banner();

  // Everything at a disc root and one directory down, ranked. The ranking is a
  // hint and nothing more: "the disc root is a DemoShield launcher and the
  // real setup is a directory further in" is exactly the judgement no ranking
  // can make, and such discs exist.
  if (!setups_scanned_ && build_) {
    setups_scanned_ = true;
    for (size_t d = 0; d < build_->discs().size(); ++d) {
      std::vector<std::string> listing = iso::list(env_, build_->discs()[d].iso);
      for (const std::string& entry : listing) {
        std::string low = entry;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.size() < 4 || low.compare(low.size() - 4, 4, ".exe") != 0) continue;
        if (std::count(low.begin(), low.end(), '/') > 1) continue;
        setups_.push_back({d, fs::path(entry)});
      }
    }
    std::sort(setups_.begin(), setups_.end(), [](const auto& a, const auto& b) {
      auto rank = [](const fs::path& p) {
        std::string n = p.filename().string();
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n.rfind("unins", 0) == 0) return 9;
        if (n.rfind("setup", 0) == 0) return 0;
        if (n.rfind("install", 0) == 0) return 1;
        if (n.rfind("autorun", 0) == 0) return 2;
        return 3;
      };
      if (rank(a.second) != rank(b.second)) return rank(a.second) < rank(b.second);
      return a.second.string() < b.second.string();
    });
    // A draft that already names its setup - a manifest, a recipe, a preset
    // taken on step 2 - named it before this list existed. The ranking is a
    // hint and that name is a fact, so the list opens on it.
    preselect_setup();
  }

  // A bare .exe is a source in its own right - a repacked installer somebody
  // downloaded, with no disc behind it - and step 1 already classified it as
  // one. It is the fourth method, and it is offered only when there is one,
  // because "the installer came without a disc" is a fact about what was
  // dropped rather than a preference.
  using Method = install::Draft::Method;
  std::error_code ec;
  fs::path bare = install::bare_exe(classified_);
  if (bare.empty() && draft_.method == Method::InstallerExe) bare = draft_.setup;
  size_t n_discs = build_ ? build_->discs().size() : 0;
  // Nothing but a bare exe: there is one sensible answer and step 1 knows it.
  if (!bare.empty() && draft_.method == Method::Installer && n_discs == 0) {
    draft_.method = Method::InstallerExe;
  }

  std::vector<Method> ways = install::methods_for(classified_, n_discs);
  // The combo below shows an entry of this list, and everything under it -
  // the setup list, the two text fields, the Go button - runs draft_.method.
  // A draft that arrives naming a method these sources cannot run (a manifest,
  // a preset, a source since removed) would have the two disagree: the page
  // saying "copy the files off the disc" while the button runs an installer.
  draft_.method = install::clamp_method(ways, draft_.method);
  std::vector<const char*> methods;
  for (Method w : ways) methods.push_back(method_word(w));
  int method = 0;
  for (size_t i = 0; i < ways.size(); ++i) {
    if (ways[i] == draft_.method) method = static_cast<int>(i);
  }
  ImGui::SetNextItemWidth(420);
  if (ImGui::Combo("##method", &method, methods.data(), static_cast<int>(methods.size()))) {
    draft_.method = ways[static_cast<size_t>(method)];
  }
  ImGui::Spacing();

  if (draft_.method == Method::Installer) {
    ImGui::BeginChild("setups", ImVec2(0, 240), true);
    for (size_t i = 0; i < setups_.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      bool on = (i == setup_pick_);
      char row[512];
      std::snprintf(row, sizeof(row), "disc %zu   %s", setups_[i].first + 1,
                    setups_[i].second.c_str());
      if (ImGui::RadioButton(row, on)) setup_pick_ = i;
      ImGui::PopID();
    }
    if (setups_.empty()) {
      ImGui::TextWrapped(
          "No executable at the root of any of these discs, or one directory in. If the "
          "disc is the game already, copy the files instead.");
    }
    ImGui::EndChild();
    if (setup_pick_ < setups_.size()) {
      // "<n>/<path on that disc>", n counting from 1 in the order open_sources
      // assembled the set. Which executable is not an answer without which
      // disc, and draft_to_meta parses exactly this form back apart into
      // recipe.setup_ref and recipe.setup - drop the number and the rebuild
      // reaches for disc 1 whatever disc this came off.
      draft_.setup = fs::path(std::to_string(setups_[setup_pick_].first + 1)) /
                     setups_[setup_pick_].second;
    }
  } else if (draft_.method == Method::Copy) {
    ImGui::TextDisabled("which directory on the disc is the game? Blank means the whole disc.");
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("##subdir", subdir_buf_, sizeof(subdir_buf_));
    draft_.subdir = subdir_buf_;
    ImGui::TextDisabled("Some discs keep the game in one directory, such as PC, beside the autorun.");
  } else if (draft_.method == Method::Unzip) {
    ImGui::TextDisabled("which archive on the disc holds the game?");
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("##member", member_buf_, sizeof(member_buf_));
    draft_.member = member_buf_;
    ImGui::TextDisabled("and which directory inside it, if it is not the archive's root");
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("##usubdir", subdir_buf_, sizeof(subdir_buf_));
    draft_.subdir = subdir_buf_;
  } else {
    ImGui::TextUnformatted(bare.c_str());
    // Absolute, because the installer runs from where it is: nothing is
    // mounted for it and there is no disc to resolve it against. draft_to_meta
    // records the same path in the pack as a note of what this was built from,
    // which is all a path off somebody else's machine can honestly be.
    draft_.setup = fs::absolute(bare, ec);
    ImGui::TextDisabled(
        "an installer with no disc behind it. It runs from where it is; nothing is mounted "
        "for it, and the pack's recipe records this path so a rebuild can find it again.");
  }

  // windows_version is asked here rather than with the other presentation
  // settings, because prepare_prefix applies it *before* the installer runs
  // and the installer is exactly where it matters. Some repacked installers
  // refuse with "cannot be installed on Windows 9x/ME", then refuse again for
  // XP, and want Vista or later. Asking after the install would be asking after
  // the only moment the answer could have helped.
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text("what Windows it thinks it is");
  const char* versions[] = {"win95", "win98", "winme", "win2k", "winxp", "vista", "win7", "win10"};
  int wv = 4;
  for (int i = 0; i < 8; ++i) if (draft_.windows_version == versions[i]) wv = i;
  ImGui::SetNextItemWidth(200);
  if (ImGui::Combo("##winver", &wv, versions, 8)) draft_.windows_version = versions[wv];
  ImGui::TextDisabled(
      "If the installer complains about the Windows version, come back here and change it.");

  ImGui::Spacing();
  ImGui::PushFont(big_);
  // Copy and unzip raise no installer and no window: there is nothing to
  // attend, so the button says what will actually happen rather than promising
  // a screen that never comes.
  const char* go = draft_.method == Method::Copy    ? "Copy it off the disc"
                   : draft_.method == Method::Unzip ? "Take it off the disc"
                                                    : "Next: install it";
  if (ImGui::Button(go, ImVec2(-1, 60))) start_the_install();
  ImGui::PopFont();
  ImGui::End();
}
void Wizard::installing_page() {
  begin_page(("installing " + draft_.name).c_str(), "Esc abandon   F11 fullscreen", big_);

  // The stage is opened from the UI thread the moment the worker publishes a
  // display, and opens its own XOpenDisplay: an Xlib connection is not shared
  // between threads here. Each side has one, which is the simplest arrangement
  // that is correct.
  if (!stage_ && stage_trouble_.empty()) {
    std::string disp;
    { std::lock_guard<std::mutex> lk(display_mutex_); disp = display_; }
    if (!disp.empty()) {
      try {
        stage_ = std::make_unique<Stage>(ren_, disp);
      } catch (const std::exception& ex) {
        stage_trouble_ = ex.what();
      }
    }
  }

  // ImGui's keyboard nav is enabled globally, and it would eat the arrows and
  // Tab that drive an InstallShield dialog before XTest ever saw them. It goes
  // back on in leave_installing().
  ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

  float w = ImGui::GetContentRegionAvail().x;
  float h = ImGui::GetContentRegionAvail().y - 200.0f;
  if (h < 120.0f) h = 120.0f;
  if (stage_) {
    // False means only "the last frame is the newest one I have". The X
    // display outlives setup.exe by construction, so this could never have
    // been the signal that the install finished.
    if (!stage_->refresh() && !stage_dead_) {
      stage_dead_ = true;
      log_line("the installer's display has gone; showing the last frame");
    }
    stage_->draw(ImVec2(w, h));
  } else {
    ImGui::Dummy(ImVec2(w, h));
    ImGui::TextDisabled("%s", stage_trouble_.empty()
                                  ? "waiting for the installer's window..."
                                  : stage_trouble_.c_str());
  }

  ImGui::Spacing();
  // A copy, taken under the worker's own lock. This step is the one with no
  // modal over it, which is exactly why it is the one place the read races:
  // the page draws every frame while the thread that assigns to failed_ is
  // still running.
  const std::string oops = failure();
  if (!oops.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s", oops.c_str());
    ImGui::PopStyleColor();
  }
  // A copy, taken once a frame, and never a reference into the Build.
  //
  // This page starts drawing on the frame the worker starts, and the worker
  // spends its first minutes inside mount_discs clearing drives_ and
  // push_backing into it. A `const std::vector&` held here across that
  // reallocation is a dangling reference in the middle of a frame - the
  // install crashing the shelf just as the first disc came up. `ready` is the
  // gate: until the mounts are done there is nothing to draw but a sentence.
  const install::Build::Mounted m =
      build_ ? build_->mounted() : install::Build::Mounted{};
  if (!m.ready) {
    ImGui::TextDisabled("%s", m.discs.empty() ? "reading the discs..."
                                              : "putting the discs in their drives...");
  } else {
    if (drive_pick_.size() != m.drives.size()) {
      drive_pick_.resize(m.drives.size());
      for (size_t i = 0; i < drive_pick_.size(); ++i) drive_pick_[i] = i;
    }
    for (size_t i = 0; i < m.drives.size(); ++i) {
      if (i) ImGui::SameLine(0, 24);
      ImGui::PushID(static_cast<int>(i));
      char letter = static_cast<char>('D' + i);
      const size_t in_it = drive_pick_[i] < m.discs.size() ? drive_pick_[i] : i;
      std::string preview = std::string(1, letter) + ":  " +
                            (in_it < m.discs.size() ? m.discs[in_it] : m.drives[i]);
      ImGui::SetNextItemWidth(240);
      if (ImGui::BeginCombo("##drive", preview.c_str())) {
        for (size_t d = 0; d < m.discs.size(); ++d) {
          if (ImGui::Selectable(m.discs[d].c_str(), d == in_it)) {
            // One symlink, the same change cmd_swap_game makes: Wine resolves
            // dosdevices on every open, so the disc changes with nothing to
            // restart and nothing moved out from under an open file. Some
            // installers do insist on one drive, which is the only reason this
            // menu exists.
            //
            // And it throws - a disc that is not mounted is said so rather
            // than silently ignored. Thrown from here it would unwind through
            // an open BeginCombo, an open Begin and out of gui::run, killing
            // the window and the running installer with it. Nothing that can
            // throw may leave a draw call.
            try {
              build_->swap_disc(static_cast<char>('d' + i), static_cast<int>(d));
              drive_pick_[i] = d;
              drive_trouble_.clear();
            } catch (const std::exception& ex) {
              drive_trouble_ = ex.what();
            }
          }
        }
        ImGui::EndCombo();
      }
      ImGui::PopID();
    }
    ImGui::TextDisabled("%s", m.drives.empty()
        ? "no disc: this installer carries its own game and runs from where it is"
        : m.drives.size() > 1
            ? "all the discs are in drives; it should not ask you to swap"
            : "the disc is in D:");
  }
  if (!drive_trouble_.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s", drive_trouble_.c_str());
    ImGui::PopStyleColor();
  }

  if (!draft_.serial.empty()) {
    ImGui::Spacing();
    ImGui::Text("serial   %s", draft_.serial.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("copy")) SDL_SetClipboardText(draft_.serial.c_str());
    ImGui::TextDisabled("shown so you can read it while you type it. Nothing types it for you.");
  }

  ImGui::Spacing();
  // An atomic read of a number a thread of Build's own publishes once a
  // second. It used to be the walk itself, on this thread, in the middle of
  // this frame - which on a large install is a stall you can see, once a
  // second, for the whole of it.
  if (build_) files_ = build_->files_written_so_far();
  ImGui::Text("%zu files written to C:", files_);
  {
    std::lock_guard<std::mutex> lk(log_mutex_);
    if (!log_.empty()) ImGui::TextDisabled("%s", log_.back().c_str());
  }
  ImGui::TextDisabled(
      "Escape asks whether to abandon this install. It does not reach the installer - which "
      "matters, because Escape is also how you back out of an InstallShield dialog.");
  ImGui::End();
}
void Wizard::where_page() {
  begin_page("where did it install?", "Esc back", big_);
  // What the install failed with, above the page that explains what an install
  // with nothing to show for it means. This is the step a failed install lands
  // on, and until it did, the page below was unreachable.
  trouble_banner();

  // A Build there always is by the time anything reaches this step - it is
  // what ran the install - but this page is also the one a failure arrives at,
  // and a page that dereferences its way out of the program is a worse answer
  // than an empty list.
  const std::vector<install::Build::Candidate> nothing;
  const std::vector<install::Build::Candidate>& c = build_ ? build_->candidates() : nothing;
  if (c.empty()) {
    // The old engine threw here (install.cpp:650-653). A page is better: the
    // usual cause is having launched a DemoShield front end rather than the
    // setup, and that is fixable one step back.
    bool off_disc = draft_.method == install::Draft::Method::Copy ||
                    draft_.method == install::Draft::Method::Unzip;
    ImGui::TextWrapped(
        "%s", off_disc
                  ? "Nothing came off the disc. It was stopped, or what step 3 named is not "
                    "on this disc under that name."
                  : "The installer wrote nothing to C:. It did not run, or it was cancelled, "
                    "or what ran was a front end rather than the setup itself.");
    ImGui::Spacing();
    if (ImGui::Button(off_disc ? "Back to what to take off the disc"
                               : "Back to the list of executables")) {
      step_ = Step::What;
    }
    ImGui::End();
    return;
  }

  if (candidate_pick_ >= c.size()) candidate_pick_ = 0;
  for (size_t i = 0; i < c.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    char row[512];
    std::snprintf(row, sizeof(row), "%-44s %8zu files   %s", where_word(c[i].dir).c_str(),
                  c[i].files, human_size(c[i].bytes).c_str());
    if (ImGui::RadioButton(row, i == candidate_pick_)) candidate_pick_ = i;
    ImGui::PopID();
  }
  ImGui::Spacing();
  // One candidate with no directory is a copy or an unzip: no installer ran,
  // nothing was written to C:, and there was no diff to rank. What came off
  // the disc is the game, whole, and there is nothing here to choose.
  if (c.size() == 1 && c[0].dir.empty()) {
    ImGui::TextWrapped(
        "Nothing ran an installer, so there is nothing to choose: this is what came off the "
        "disc, and the pack holds it as it is.");
  } else {
    ImGui::TextWrapped(
        "The first one is almost certainly it. Pick another if the game is somewhere else.");
    ImGui::TextDisabled(
        "This is the whole of what source.verify used to do, done by looking rather than by "
        "being told.");
  }

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (ImGui::Button("Next: what runs it", ImVec2(-1, 60))) {
    draft_.install_dir = c[candidate_pick_].dir;
    // The same courtesy step 3 does for the setup. A preset or a recipe that
    // names the exe is answering the one question ranking cannot: a game's
    // expansions may all run the base game's exe, and a manifest may name
    // Gamew.exe because the larger GAME.EXE beside it is the DOS build.
    exe_pick_ = install::exe_index(c[candidate_pick_].executables, draft_.exe);
    if (exe_pick_ >= c[candidate_pick_].executables.size()) exe_pick_ = 0;
    step_ = Step::Runs;
  }
  ImGui::PopFont();
  ImGui::End();
}
void Wizard::runs_page() {
  begin_page("what runs the game?", "Esc back", big_);

  const install::Build::Candidate& c = build_->candidates()[candidate_pick_];
  if (c.executables.empty()) {
    ImGui::TextWrapped("Nothing in %s looks like a program. Try another directory.",
                       where_word(c.dir).c_str());
    ImGui::Spacing();
    if (ImGui::Button("Back to the directories")) step_ = Step::Where;
    ImGui::End();
    return;
  }

  std::error_code ec;
  for (size_t i = 0; i < c.executables.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    // installed_root(), not drive_c: a copy or unzip install never wrote to C:
    // and its tree is the staging tree, so this is where the file whose size
    // is about to be printed actually is.
    fs::path full = build_->installed_root() / c.dir / c.executables[i];
    char row[512];
    std::snprintf(row, sizeof(row), "%-40s %10s", c.executables[i].c_str(),
                  human_size(fs::file_size(full, ec)).c_str());
    if (ImGui::RadioButton(row, i == exe_pick_)) exe_pick_ = i;
    ImGui::PopID();
  }
  draft_.exe = c.executables[exe_pick_];

  ImGui::Spacing();
  ImGui::TextDisabled("arguments");
  ImGui::SetNextItemWidth(420);
  ImGui::InputText("##args", args_buf_, sizeof(args_buf_));
  draft_.args = args_buf_;
  // Its own line with its own hint, rather than tucked into an advanced
  // section, because this field is sometimes the entire difference between
  // two games.
  ImGui::TextWrapped(
      "Some games are the same program twice: expansions that run the base game's exe "
      "differ only here.");

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (ImGui::Button("Next: how it looks", ImVec2(-1, 60))) {
    // Written into the pack now, while the tree is still on disk and still
    // exactly what the user just looked at. After the build there is nothing
    // left to look at: the staging tree is gone.
    draft_.verify = install::verify_list(
        build_->installed_root() / draft_.install_dir, draft_.exe);
    step_ = Step::Presentation;
  }
  ImGui::PopFont();
  ImGui::End();
}
void Wizard::presentation_page() {
  begin_page("how it looks", "Esc back", big_);

  int w = static_cast<int>(draft_.width), h = static_cast<int>(draft_.height);
  ImGui::SetNextItemWidth(140);
  if (ImGui::InputInt("width", &w) && w > 0) draft_.width = static_cast<uint32_t>(w);
  ImGui::SetNextItemWidth(140);
  if (ImGui::InputInt("height", &h) && h > 0) draft_.height = static_cast<uint32_t>(h);
  ImGui::TextDisabled(
      "the resolution the game renders at, not the size of the window. How that reaches "
      "your panel is a setting, on the Settings screen, and it applies to every game.");

  ImGui::Spacing();
  ImGui::Checkbox("dgVoodoo", &draft_.dgvoodoo);
  ImGui::TextDisabled("a Direct3D wrapper, for games whose own renderer no longer works");

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (ImGui::Button("Next: build the pack", ImVec2(-1, 60))) step_ = Step::Build;
  ImGui::PopFont();
  ImGui::End();
}
void Wizard::build_page() {
  begin_page("build the pack", "Esc back", big_);
  trouble_banner();

  const install::Build::Candidate& c = build_->candidates()[candidate_pick_];
  uint64_t disc_bytes = 0, audio_bytes = 0;
  std::error_code ec;
  for (const disc::Disc& d : build_->discs()) {
    disc_bytes += d.info.size;
    for (const disc::AudioTrack& t : d.audio) audio_bytes += fs::file_size(t.file, ec);
  }

  // What the installer wrote to C: that is not the game: the DLLs and the
  // shared runtime it scattered around Windows. They travel whatever else does,
  // because the registry fragment names them and the fragment travels; the row
  // is here so that is visible rather than a surprise in the size.
  install::SystemFiles sys = build_->outside(c.dir);

  ImGui::Text("  game tree     %14s", human_size(c.bytes).c_str());
  ImGui::Text("  system files  %14s", human_size(sys.bytes).c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("%zu outside the game folder", sys.files);
  ImGui::Text("  discs         %14s", human_size(disc_bytes).c_str());
  ImGui::SameLine();
  ImGui::Checkbox("include the discs", &draft_.embed_discs);
  ImGui::Text("  cd audio      %14s", human_size(audio_bytes).c_str());
  ImGui::Separator();
  uint64_t total = c.bytes + sys.bytes + (draft_.embed_discs ? disc_bytes + audio_bytes : 0);
  ImGui::Text("  before dedup  %14s", human_size(total).c_str());
  ImGui::TextWrapped(
      "The installed files are copies of files on the discs, and mkdwarfs stores a byte "
      "once, so the pack will be smaller than this - usually much smaller.");
  if (!draft_.embed_discs) {
    ImGui::TextDisabled(
        "Without them, the game's page will say \"needs the original disc\", the same way "
        "the library already says it for a disc it cannot find.");
  }

  for (const std::string& note : protection_notes(build_->installed_root() / c.dir)) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s", note.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (ImGui::Button("Build it", ImVec2(-1, 60))) {
    draft_.install_dir = c.dir;
    install::Draft d = draft_;
    run_job("Building " + draft_.name, [this, d] {
      result_ = build_->write(d);
      root_matched_ = !expect_root_set_ || result_.root == expect_root_;
    }, Step::Done);
  }
  ImGui::PopFont();

  // The sources came from anywhere on the disk, and a recipe's disc references
  // resolve only against the top level of iso_dir(). Adding each source's
  // folder to the library paths is the same mechanism as dropping a folder on
  // the shelf, and it is what makes these discs findable again.
  config::Config cfg = config::load(config::config_file());
  bool dirty = false;
  for (const fs::path& s : draft_.sources) {
    std::string parent = s.parent_path().string();
    if (std::find(cfg.library_paths.begin(), cfg.library_paths.end(), parent) ==
        cfg.library_paths.end()) {
      cfg.library_paths.push_back(parent);
      dirty = true;
    }
  }
  if (dirty) config::save(config::config_file(), cfg);
  ImGui::End();
}
void Wizard::done_page() {
  begin_page((draft_.name + " is in your library").c_str(), "Esc back", big_);

  ImGui::TextDisabled("%zu files, %s, packed to %s", result_.entries,
                      human_size(result_.tree_bytes).c_str(),
                      human_size(result_.pack_bytes).c_str());
  ImGui::TextDisabled("one file: %s", result_.pack.filename().c_str());
  if (expect_root_set_) {
    // Advisory now: with no answer file, two people clicking through the same
    // InstallShield need not produce the same bytes, so a mismatch is a fact
    // to report rather than a reason to throw the install away.
    ImGui::Spacing();
    ImGui::TextWrapped("%s", root_matched_ ? "the tree matches the recipe"
                                           : "the tree differs from the recipe");
  }

  ImGui::Spacing();
  ImGui::PushFont(big_);
  if (ImGui::Button("Play it", ImVec2(-1, 60))) {
    status_ = "play:" + draft_.id;
    finished_ = true;
  }
  ImGui::PopFont();
  ImGui::Spacing();
  // There is no separate export step in the format - install.h:1-6 says the
  // pack is simultaneously the installed game and the shareable capsule - so
  // this is a copy, and the button says so.
  if (ImGui::Button("Copy the .kgpack to your home directory")) {
    std::error_code ec;
    const char* home = std::getenv("HOME");
    fs::path out = fs::path(home ? home : ".") / result_.pack.filename();
    fs::copy_file(result_.pack, out, fs::copy_options::overwrite_existing, ec);
    status_ = ec ? ("could not copy it: " + ec.message()) : ("copied to " + out.string());
  }
  ImGui::SameLine();
  if (ImGui::Button("Back to the shelf")) {
    status_ = draft_.name + " is in your library";
    finished_ = true;
  }
  ImGui::End();
}
// A plain list over std::filesystem. There is no file dialog in this binary
// and there should not be one: a dialog is another toolkit, another theme and
// another thing that cannot be driven with a gamepad.
void Wizard::browser() {
  std::error_code ec;
  ImGui::BeginChild("browse", ImVec2(0, 260), true);
  ImGui::TextDisabled("%s", browse_dir_.c_str());
  if (ImGui::Selectable("..")) browse_dir_ = browse_dir_.parent_path();
  std::vector<fs::path> dirs, files;
  for (const fs::directory_entry& de : fs::directory_iterator(browse_dir_, ec)) {
    if (de.is_directory(ec)) dirs.push_back(de.path());
    else if (de.is_regular_file(ec)) files.push_back(de.path());
  }
  std::sort(dirs.begin(), dirs.end());
  std::sort(files.begin(), files.end());
  for (const fs::path& d : dirs) {
    ImGui::PushID(d.c_str());
    if (ImGui::Selectable((d.filename().string() + "/").c_str())) browse_dir_ = d;
    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    // A folder is a source in its own right: a mounted CD, or a disc somebody
    // already extracted.
    if (ImGui::SmallButton("use this folder")) add_source(d);
    ImGui::PopID();
  }
  for (const fs::path& f : files) {
    ImGui::PushID(f.c_str());
    if (ImGui::Selectable(f.filename().c_str())) add_source(f);
    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    ImGui::TextDisabled("%s", human_size(fs::file_size(f, ec)).c_str());
    ImGui::PopID();
  }
  ImGui::EndChild();
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
                                              [this](const std::string& l) { log_line(l); });
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
  // their disc, or zipped on it, arrive this way, and until now none of them
  // could be added at all.
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
    log_line("preparing the prefix as " + d.windows_version);
    build_->prepare_prefix(d.windows_version);
    log_line("mounting the discs");
    build_->mount_discs();
    log_line("reading what is on C: before the installer runs");
    build_->snapshot_before();
    // run_setup runs what it is given from the directory it is in, so it wants
    // a path on this filesystem. A Draft's setup is already one for
    // InstallerExe; for Installer it is "<n>/<path on that disc>", and the
    // discs are mounted at work/drive-d, work/drive-e, ... in the order
    // open_sources assembled the set. staged_setup is that join, and it knows
    // that only a leading run of digits is a disc number - this used to read
    // everything before the first slash as one, which turned a setup at
    // Game3/Setup.exe into a Setup.exe at the root of a disc that has none.
    const fs::path setup = install::staged_setup(work_, d);
    // Headless Weston: nobody sees a second window, and the frames come to us
    // instead. The display arrives from inside run_in_compositor the moment
    // Xwayland is confirmed up, which is the whole of the coupling.
    build_->run_setup(setup, /*headless=*/true, [this](const std::string& disp) {
      std::lock_guard<std::mutex> lk(display_mutex_);
      display_ = disp;
    });
    log_line("working out what it wrote");
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
