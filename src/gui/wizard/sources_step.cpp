#include "wizard.h"

#include <algorithm>
#include <cmath>
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
  PageWindow page(step_title("sources"), "Esc back", fonts_.big());
  step_top();
  // The browser is laid out from what browsing_ was as the frame began, so a
  // press of the button part-way down changes the layout from the next frame
  // rather than half of this one.
  const bool browsing = browsing_;
  const bool listed = !classified_.empty();
  // The browser and the button that opens it sit between the body and the
  // footer rather than at the end of the body, where a long list of files
  // scrolled them out of sight. The browser is given a share of the height
  // of its own, so it is never a strip of path over no files.
  float below = 0, browse_h = 0;
  if (listed) below += ImGui::GetFrameHeightWithSpacing();
  if (browsing) {
    const float room = ImGui::GetContentRegionAvail().y - footer_block() - below;
    browse_h = std::round(std::clamp(room * 0.42f, px(150), px(260)));
    below += browse_h + ImGui::GetStyle().ItemSpacing.y;
  }
  begin_body(below);
  trouble_banner();

  if (!stale_.empty()) {
    begin_badge_block(BadgeKind::Warn);
    // A warning's headline in the warning's own colour; kWarn is a failure's.
    ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
    ImGui::TextWrapped("%s is a staging tree an install left behind - %s of it.",
                       stale_.filename().c_str(), human_size(stale_bytes_).c_str());
    ImGui::PopStyleColor();
    if (small_button("delete it")) {
      std::error_code ec;
      fs::remove_all(stale_, ec);
      stale_.clear();
    }
    ImGui::SameLine();
    if (small_button("leave it")) stale_.clear();
    end_badge_block();
    vgap(6);
  }

  if (!listed) {
    // The disc, the sentence, the kinds and the button to pick one are one
    // group, set a little above the middle of the body the way empty_state
    // sets its own, so the only thing to do on the page is where the eye is.
    const float empty_h = std::round(px(120));
    const float group = empty_h + ImGui::GetTextLineHeightWithSpacing() + std::round(px(12)) +
                        ImGui::GetFrameHeight();
    const float spare = ImGui::GetContentRegionAvail().y - group;
    if (spare > 0) ImGui::Dummy(ImVec2(0, std::round(spare * 0.35f)));
    // A disc, ringed, over the sentence: the page has nothing to list yet and
    // says what it is waiting for.
    empty_state(
        "Nothing here yet. Drop a disc image, an archive, a folder or a setup.exe on this "
        "window, or pick one below.",
        "\xe2\x97\x8e", empty_h);  // U+25CE, a disc
    const char* kinds = ".iso, .bin with its .cue, .img, .mdf, .zip, .7z, a folder, or a bare .exe";
    ImGui::PushFont(fonts_.small());
    const float kw = ImGui::CalcTextSize(kinds).x;
    const float aw = ImGui::GetContentRegionAvail().x;
    if (kw < aw) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::floor((aw - kw) * 0.5f));
      ImGui::TextDisabled("%s", kinds);
    } else {
      // Too long for the line: wrapped rather than cut off at the edge.
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("%s", kinds);
      ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    vgap(12);
    // The one thing to do on an empty page, under the sentence that offers
    // it, as the page's primary.
    const char* pick = browsing_ ? "close the browser" : "pick a file";
    const float pw = ImGui::CalcTextSize(pick).x + ImGui::CalcTextSize("[ ").x * 2 +
                     ImGui::GetStyle().FramePadding.x * 2;
    const float pa = ImGui::GetContentRegionAvail().x;
    if (pw < pa) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::floor((pa - pw) * 0.5f));
    if (primary_button(pick)) browsing_ = !browsing_;
    // The page opens here, not on the banner's "delete it" above.
    default_focus();
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
  // One table for the files and the discs found in them, so that the columns
  // are measured rather than placed: fixed offsets ran into each other as
  // soon as the font grew.
  if (listed) section("files");
  const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH |
                             ImGuiTableFlags_PadOuterX;
  if (listed && ImGui::BeginTable("sources", 5, tf)) {
    ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("status");
    ImGui::TableSetupColumn("size");
    // The start of the disc's fingerprint, which is what tells two pressings
    // with the same label apart; the whole of it is in the tooltip.
    ImGui::TableSetupColumn("fp");
    ImGui::TableSetupColumn("##actions");
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < 5; ++c) {
      ImGui::TableSetColumnIndex(c);
      const char* h = ImGui::TableGetColumnName(c);
      if (c == 2) right_aligned(h, kDim);
      else if (c != 4) ImGui::TextDisabled("%s", h);
    }
    for (size_t i = 0; i < classified_.size(); ++i) {
      const install::Source& s = classified_[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      const std::string name = s.path.filename().string();
      ImGui::TextUnformatted(elide(nullptr, name, ImGui::GetContentRegionAvail().x).c_str());
      if (s.kind == install::Source::Kind::Unreadable) {
        // One source that will not open does not stop the others: a collection
        // with a truncated download in it is still a collection.
        ImGui::Indent(px(16));
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::TextWrapped("%s", s.trouble.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent(px(16));
      }
      ImGui::TableSetColumnIndex(1);
      // Read, not yet read, or would not open. Before the probe the kind is
      // only what the name suggests, so it is not called OK yet.
      if (s.kind == install::Source::Kind::Unreadable) badge(BadgeKind::Fail);
      else if (probed_) badge(BadgeKind::Ok);
      else badge(BadgeKind::Info, "?");
      ImGui::SameLine(0, px(8));
      ImGui::TextDisabled("%s", kind_word(s.kind));
      ImGui::TableSetColumnIndex(4);
      if (i > 0) {
        if (small_button("up")) move_up = static_cast<int>(i);
      } else {
        // Room where "up" would be, so every "remove" lines up.
        ImGui::Dummy(ImVec2(small_button_width("up"), ImGui::GetTextLineHeight()));
      }
      ImGui::SameLine();
      if (small_button("remove")) remove_at = static_cast<int>(i);

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
          ImGui::PushID(static_cast<int>(d) + 1000);
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::Indent(px(16));
          ImGui::PushStyleColor(ImGuiCol_Text, kLine);
          ImGui::TextUnformatted("\xe2\x94\x94");  // U+2514, the branch to its file
          ImGui::PopStyleColor();
          ImGui::SameLine(0, px(6));
          ImGui::TextDisabled("disc %d", n);
          ImGui::SameLine(0, px(12));
          ImGui::TextUnformatted(disc.label.empty() ? "-" : disc.label.c_str());
          const bool known = d < known_as_.size() && !known_as_[d].empty();
          if (known) {
            ImGui::Indent(px(22));
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped("%s", known_as_[d].c_str());
            ImGui::PopStyleColor();
            ImGui::Unindent(px(22));
          }
          ImGui::Unindent(px(16));
          ImGui::TableSetColumnIndex(1);
          // The disc database's say: a disc it knows, one it knows but whose
          // bytes differ, or one it has never seen, which gets a dim blank
          // chip so the column reads down without a hole.
          if (known) {
            const bool bad = known_as_[d].find("bad dump") != std::string::npos;
            badge(bad ? BadgeKind::Warn : BadgeKind::Ok);
          } else {
            ImGui::TextDisabled("[ -- ]");
          }
          ImGui::TableSetColumnIndex(2);
          right_aligned(human_size(disc.info.size), kText);
          ImGui::TableSetColumnIndex(3);
          const std::string fp = to_hex(disc.info.prefix);
          ImGui::PushStyleColor(ImGuiCol_Text, kCyan);
          ImGui::TextUnformatted(fp.substr(0, 8).c_str());
          ImGui::PopStyleColor();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", fp.c_str());
          ImGui::PopID();
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
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
    vgap(6);
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

  end_body();
  // Between the body and the footer, in the column the body's words are in.
  centre_column(content_max_w(Content::Form));
  if (listed && small_button(browsing_ ? "close the browser" : "pick a file")) {
    browsing_ = !browsing_;
  }
  if (listed) default_focus();
  if (browsing) browser(browse_h);
  end_centre_column();

  bool ready = probed_ && build_ &&
               install::sources_are_enough(classified_, build_->discs().size());
  bool can_read = !draft_.sources.empty() && !probed_;
  // With files to go on with, the page opens on going on with them; the
  // default above stands while this one cannot be pressed.
  const bool go_on = step_footer(ready ? "Next: name it" : "Read these", ready || can_read, true);
  if (go_on) {
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
}
// A plain list over std::filesystem. There is no file dialog in this binary
// and there should not be one: a dialog is another toolkit, another theme and
// another thing that cannot be driven with a gamepad.
void Wizard::browser(float height) {
  FileListSpec spec;
  spec.child_id = "browse";
  // file_list takes design pixels and scales them itself.
  spec.height = height / ui_scale();
  // A folder is a source in its own right: a mounted CD, or a disc somebody
  // already extracted.
  spec.dir_button = "use this folder";
  FilePick pick = file_list(browse_dir_, spec);
  if (pick.kind != FilePick::None) add_source(pick.path);
}

}  // namespace kg::gui
