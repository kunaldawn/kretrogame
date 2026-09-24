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
  ImGui::TextUnformatted("Title");
  ImGui::SetNextItemWidth(-1);
  if (input_string("##title", draft_.title, 128)) {
    dirty_ = true;
    // The id follows the title until the author types one, and never once
    // the bundle is out: that is when changing it would lose people's saves.
    if (!draft_.id_typed && !draft_.published) draft_.id = id_from_title(draft_.title);
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Bundle id");
  ImGui::SetNextItemWidth(420);
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
    ImGui::TextDisabled(draft_.id_typed ? "Yours. It becomes fixed once you mark the bundle published."
                                        : "Made from the title. Type your own if you want another.");
    if (draft_.id_typed && ImGui::SmallButton("Make it from the title again")) {
      draft_.id_typed = false;
      draft_.id = id_from_title(draft_.title);
      dirty_ = true;
    }
  }
  if (!kg::id_is_safe(draft_.id)) warn_text("An id is letters, digits, '-', '_' and '.', starting with a letter or digit.");

  ImGui::Spacing();
  ImGui::TextUnformatted("Version");
  ImGui::SetNextItemWidth(200);
  if (input_string("##version", draft_.version, 33)) dirty_ = true;
  if (!version_is_safe(draft_.version)) warn_text("A version is letters, digits, '-', '_' and '.': it goes into the file name.");
  else ImGui::TextDisabled("The file will be %s", output_name(draft_).c_str());

  ImGui::Spacing();
  auto picture = [&](const char* what, std::string& bytes, std::string& from, Browse mode) {
    ImGui::PushID(what);
    ImGui::TextUnformatted(what);
    ImGui::SameLine(160);
    if (bytes.empty()) ImGui::TextDisabled("none");
    else ImGui::Text("%s  (%s)", picture_words(bytes).c_str(), fs::path(from).filename().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Choose PNG...")) browse_ = mode;
    if (!bytes.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Remove")) {
        bytes.clear();
        from.clear();
        dirty_ = true;
      }
    }
    ImGui::PopID();
  };
  picture("Banner", draft_.banner, draft_.banner_from, Browse::Banner);
  picture("Icon", draft_.icon, draft_.icon_from, Browse::Icon);

  ImGui::Spacing();
  if (!draft_.published) {
    ImGui::TextWrapped(
        "Once a build of this bundle has gone out to anybody, mark it published: its id is then fixed.");
    ImGui::BeginDisabled(draft_.last_built.empty());
    if (ImGui::Button("Mark as published")) {
      draft_.published = true;
      dirty_ = true;
    }
    ImGui::EndDisabled();
  }
}

}  // namespace kg::gui
