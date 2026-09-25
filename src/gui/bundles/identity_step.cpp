#include <cfloat>
#include <cmath>
#include <string>

#include "../../util/safe_names.h"
#include "../widgets.h"
#include "bundles_page.h"
#include "choices.h"

namespace kg::gui {
namespace fs = std::filesystem;
using namespace kg::bundle;
using bundles_detail::picture_words;

// ---- 1. identity ------------------------------------------------------------------------

void Bundles::identity_step() {
  section("identity");
  if (form_begin("identity", {"Title", "Bundle id", "Version", "Banner", "Icon"})) {
    form_row("Title");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (input_string("##title", draft_.title, 128)) {
      dirty_ = true;
      // The id follows the title until the author types one, and never once
      // the bundle is out: that is when changing it would lose people's saves.
      if (!draft_.id_typed && !draft_.published) draft_.id = id_from_title(draft_.title);
    }

    form_row("Bundle id");
    ImGui::SetNextItemWidth(field_width(420));
    ImGui::BeginDisabled(draft_.published);
    if (input_string("##id", draft_.id, 101)) {
      draft_.id_typed = true;
      dirty_ = true;
    }
    ImGui::EndDisabled();
    if (draft_.published) {
      ImGui::TextWrapped(
          "Fixed: this bundle has been published, and every player keeps its saves under this id. A new id "
          "would make a new bundle, whose players could not find the old one's saves.");
    } else {
      colored_text(kDim, draft_.id_typed ? "Yours. It becomes fixed once you mark the bundle published."
                                         : "Made from the title. Type your own if you want another.");
      if (draft_.id_typed && small_button("Make it from the title again")) {
        draft_.id_typed = false;
        draft_.id = id_from_title(draft_.title);
        dirty_ = true;
      }
    }
    if (!kg::id_is_safe(draft_.id)) {
      badge_line(BadgeKind::Warn, "An id is letters, digits, '-', '_' and '.', starting with a letter or digit.");
    }

    form_row("Version");
    ImGui::SetNextItemWidth(field_width(200));
    if (input_string("##version", draft_.version, 33)) dirty_ = true;
    if (!version_is_safe(draft_.version)) {
      badge_line(BadgeKind::Warn, "A version is letters, digits, '-', '_' and '.': it goes into the file name.");
    } else {
      ImGui::TextDisabled("The file will be");
      ImGui::SameLine();
      elided_text(output_name(draft_), kCyan);
    }

    // The buttons first and the picture's words after them, cut to the room
    // left, so the buttons stay where they are however long the file name.
    // Full-height buttons, as tall as the fields above them, so the form's
    // rows keep one rhythm.
    auto picture = [&](const char* what, std::string& bytes, std::string& from, Browse mode) {
      form_row(what);
      ImGui::PushID(what);
      if (ImGui::Button("Choose PNG...")) browse_ = mode;
      // The step opens on the banner's button, the first thing on it that is
      // not a field to type in, unless a file list is open, which opens on
      // itself.
      if (mode == Browse::Banner && browse_ == Browse::None) default_focus();
      if (!bytes.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
          bytes.clear();
          from.clear();
          dirty_ = true;
        }
      }
      ImGui::SameLine(0, std::round(px(12)));
      if (bytes.empty()) {
        ImGui::TextDisabled("none");
      } else {
        elided_text(picture_words(bytes) + "  (" + fs::path(from).filename().string() + ")");
      }
      ImGui::PopID();
    };
    picture("Banner", draft_.banner, draft_.banner_from, Browse::Banner);
    picture("Icon", draft_.icon, draft_.icon_from, Browse::Icon);
    ImGui::EndTable();
  }

  if (!draft_.published) {
    section("publish");
    ImGui::TextWrapped(
        "Once a build of this bundle has gone out to anybody, mark it published: its id is then fixed.");
    ImGui::Spacing();
    ImGui::BeginDisabled(draft_.last_built.empty());
    if (ImGui::Button("Mark as published")) {
      draft_.published = true;
      dirty_ = true;
    }
    ImGui::EndDisabled();
  }
}

}  // namespace kg::gui
