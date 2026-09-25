#include "wizard.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "../../install/game_id.h"
#include "../../install/keys.h"
#include "../../install/manifest.h"
#include "../../install/preset.h"
#include "../palette.h"
#include "../widgets.h"
#include "words.h"

namespace kg::gui {

using wizard_detail::set_buf;

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

  section("recognised");
  begin_badge_block(BadgeKind::Info);
  ImGui::TextWrapped("This looks like %s. Use what we know about it?", p.name.c_str());
  // On the sentence's own left edge: the badge block already hangs it there.
  if (p.by_fingerprint) {
    ImGui::TextDisabled("these are the discs that manifest was written against");
  } else if (p.of > 1) {
    ImGui::TextDisabled("%zu of its %zu discs are here, matched by label", p.matched, p.of);
  } else {
    ImGui::TextDisabled("matched by disc label, not by fingerprint");
  }
  end_badge_block();
  vgap(4);

  if (primary_button("yes, prefill")) {
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
    ImGui::SetNextItemWidth(field_width(280));
    if (ImGui::BeginCombo("##preset", presets_[preset_index_].name.c_str())) {
      for (size_t i = 0; i < presets_.size(); ++i) {
        bool sel = i == preset_index_;
        if (ImGui::Selectable(presets_[i].name.c_str(), sel)) preset_index_ = i;
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }
  vgap(10);
}

void Wizard::identity_page() {
  PageWindow page(step_title("name it"), "Esc back", fonts_.big());
  step_top();
  begin_body();

  // The page opens on the offer when there is one (the first thing on it),
  // on the name while there is none to go on with, and on going on once the
  // name is there: a text field is the default only while it is empty,
  // where typing into it is plainly the next thing to do.
  preset_offer();
  const bool offered = !presets_.empty() && !preset_settled_;

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

  section("identity");
  // Keys in a dim column of their own and the fields beside them, the way the
  // rest of the terminal shows a record.
  const bool fields = ImGui::BeginTable("fields", 2, ImGuiTableFlags_SizingFixedFit);
  if (fields) {
    ImGui::TableSetupColumn("key");
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
    form_row("name");
  }
  const bool name_empty = name_buf_[0] == '\0';
  ImGui::SetNextItemWidth(field_width(560));
  if (ImGui::InputText("##name", name_buf_, sizeof(name_buf_))) {
    // The id follows the name until the id is edited by hand, at which point
    // it stops following: renaming a game should not silently re-home its
    // saves under a different id.
    if (draft_.id == install::slug(draft_.name)) {
      set_buf(id_buf_, sizeof(id_buf_), install::slug(name_buf_));
    }
  }
  draft_.name = name_buf_;

  if (fields) form_row("id");
  ImGui::SetNextItemWidth(field_width(560));
  ImGui::PushStyleColor(ImGuiCol_Text, kCyan);
  ImGui::InputText("##id", id_buf_, sizeof(id_buf_));
  ImGui::PopStyleColor();
  draft_.id = id_buf_;
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextWrapped("the name of its pack, its saves and its Wine prefix");
  ImGui::PopStyleColor();

  if (fields) form_row("year");
  ImGui::SetNextItemWidth(field_width(160));
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
    // Under the fields rather than among them, where the table's key column
    // would squeeze a paragraph into a narrow strip.
    if (fields) ImGui::EndTable();
    vgap(6);
    // Wrapped to the fields' measure, which is where the eye already is,
    // rather than across the whole of a wide window.
    const float wrap = ImGui::GetCursorPosX() + field_width(640);
    begin_badge_block(BadgeKind::Warn);
    ImGui::PushTextWrapPos(wrap);
    // The headline in the warning's colour and the explanation in the text's,
    // as every warning in the wizard is drawn.
    ImGui::PushStyleColor(ImGuiCol_Text, kWarm);
    ImGui::TextWrapped("%s already names %s on this machine.", draft_.id.c_str(),
                       clash.sentence().c_str());
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "That may be exactly right - rebuilding the game you already have replaces its pack "
        "and keeps its saves. If this is a different game, give it a different id; the "
        "wizard will not quietly rename it for you, because overwriting a game is a "
        "decision rather than an accident.");
    ImGui::PopTextWrapPos();
    std::string alt = install::next_free_id(env_, draft_.id);
    if (small_button(("use " + alt + " instead").c_str())) {
      set_buf(id_buf_, sizeof(id_buf_), alt);
      draft_.id = alt;
    }
    end_badge_block();
  } else if (fields) {
    ImGui::EndTable();
  }

  vgap(6);
  section("serial");
  ImGui::TextDisabled("serial, if the installer will ask for one");
  ImGui::SetNextItemWidth(field_width(560));
  if (serial_buf_[0] == '\0' && !draft_.id.empty() && keys_seen_.due(draft_.id)) {
    std::string known = install::key_for(install::load_keys(install::keys_file()), draft_.id);
    if (!known.empty()) set_buf(serial_buf_, sizeof(serial_buf_), known);
  }
  ImGui::PushStyleColor(ImGuiCol_Text, kCyan);
  ImGui::InputText("##serial", serial_buf_, sizeof(serial_buf_));
  ImGui::PopStyleColor();
  draft_.serial = serial_buf_;
  ImGui::PushTextWrapPos(prose_wrap());
  ImGui::TextDisabled(
      "kept on this machine and shown back to you next to the installer that asks for it. "
      "It goes into no pack.");
  ImGui::PopTextWrapPos();

  end_body();
  const bool named = !draft_.id.empty() && !draft_.name.empty();
  if (step_footer("Next: what to run", named, named && !offered && !name_empty)) {
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
}

}  // namespace kg::gui
