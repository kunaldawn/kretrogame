#include "wizard.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

#include "../../disc/members.h"
#include "../../install/setup_ref.h"
#include "../../util/text.h"
#include "../widgets.h"
#include "words.h"

namespace kg::gui {
namespace fs = std::filesystem;

using wizard_detail::method_word;

void Wizard::preselect_setup() {
  size_t i = install::setup_index(setups_, draft_.setup);
  if (i < setups_.size()) setup_pick_ = i;
}

void Wizard::what_page() {
  PageWindow page("what runs the install?", "Esc back", big_);
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
        std::string low = to_lower(entry);
        if (low.size() < 4 || low.compare(low.size() - 4, 4, ".exe") != 0) continue;
        if (std::count(low.begin(), low.end(), '/') > 1) continue;
        setups_.push_back({d, fs::path(entry)});
      }
    }
    std::sort(setups_.begin(), setups_.end(), [](const auto& a, const auto& b) {
      auto rank = [](const fs::path& p) {
        std::string n = to_lower(p.filename().string());
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
  methods.reserve(ways.size());
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
}

}  // namespace kg::gui
