#include "wizard.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../../disc/database.h"
#include "../../install/staging.h"
#include "../../util/hash.h"
#include "../file_list.h"
#include "../format.h"
#include "../palette.h"
#include "../widgets.h"
#include "words.h"

namespace kg::gui {
namespace fs = std::filesystem;

using wizard_detail::discs_in_words;
using wizard_detail::kind_word;

// Step 1: the discs, archives, folders and installers the game comes from,
// and what opening them found.
void Wizard::sources_page() {
  PageWindow page("sources", "Esc back", big_);
  trouble_banner();

  if (!stale_.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
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
  // classify_source() looks at an extension and nothing else; open_sources is where a
  // truncated download, a zip with no disc inside it and a .cue whose .bin is
  // missing say so, and it records the sentence for each. Nothing read that
  // list, so step 1 went on describing a file as an archive while the log
  // modal above it scrolled past the reason it was not one. Adopted by
  // position, which is the order open_sources fills it in, and only once the
  // probe has finished - the worker is inside that vector until then.
  if (probed_ && build_) {
    const std::vector<install::Source>& opened = build_->sources();
    for (size_t i = 0; i < opened.size() && i < classified_.size(); ++i) {
      if (opened[i].path != classified_[i].path) break;
      classified_[i] = opened[i];
    }
  }

  int remove_at = -1, move_up = -1;
  for (size_t i = 0; i < classified_.size(); ++i) {
    const install::Source& s = classified_[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::Spacing();
    ImGui::Text("%s", s.path.filename().c_str());
    ImGui::SameLine(420);
    ImGui::TextDisabled("%s", kind_word(s.kind));
    ImGui::SameLine(ImGui::GetWindowWidth() - 200);
    if (i > 0 && ImGui::SmallButton("up")) move_up = static_cast<int>(i);
    ImGui::SameLine();
    if (ImGui::SmallButton("remove")) remove_at = static_cast<int>(i);

    if (s.kind == install::Source::Kind::Unreadable) {
      // One source that will not open does not stop the others: a collection
      // with a truncated download in it is still a collection.
      ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
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
          env_, work_, [this](const std::string& l) { job_.log(l); });
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
        job_.locked([&] {
          known_as_ = named;
          probed_ = true;
        });
      }, Step::Sources);
    }
  }
  if (!ready && !can_read) ImGui::EndDisabled();
  ImGui::PopFont();
}
// A plain list over std::filesystem. There is no file dialog in this binary
// and there should not be one: a dialog is another toolkit, another theme and
// another thing that cannot be driven with a gamepad.
void Wizard::browser() {
  FileListSpec spec;
  spec.child_id = "browse";
  spec.height = 260;
  // A folder is a source in its own right: a mounted CD, or a disc somebody
  // already extracted.
  spec.dir_button = "use this folder";
  FilePick pick = file_list(browse_dir_, spec);
  if (pick.kind != FilePick::None) add_source(pick.path);
}

}  // namespace kg::gui
