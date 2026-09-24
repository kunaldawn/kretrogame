// A .kgpack: what it is - a capsule or a recipe, which game, how large, whose
// root - and taking it in. Reached from the shelf, from the Library screen,
// and by dropping a pack on the window.
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

#include "../../install/share.h"
#include "../../session/journal.h"
#include "../../util/hash.h"
#include "../../util/paths.h"
#include "../../util/text.h"
#include "../file_list.h"
#include "../format.h"
#include "../palette.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {
namespace fs = std::filesystem;

void ImportPage::open_file(const fs::path& p) {
  drop_path_ = p;
  ctx_.go(Screen::Import);
}

void ImportPage::inspect_for_import(const fs::path& f) {
  if (import_.of == f) return;
  import_ = Inspected{};
  import_.of = f;
  std::error_code ec;
  import_.bytes = fs::file_size(f, ec);
  try {
    Pack p = Pack::open(f);
    import_.meta = p.meta();
    import_.has_body = p.has_body();
    import_.root = p.header().blake3_root;
    // A capsule carries its discs, so there is nothing to go looking for.
    if (!import_.has_body) import_.prefill = install::draft_from_meta(import_.meta);
  } catch (const std::exception& ex) {
    import_.trouble = ex.what();
  }
}

// A list of directories and .kgpack files, the same plain walk over
// std::filesystem the wizard's step 1 uses. Dropping a file on the window
// was the only way into this page, and a drop is not something a person
// sitting in front of a shelf with a gamepad can do - nor anyone whose file
// manager is a terminal. A dialog is out for the same reason it is out of
// the wizard: another toolkit, another theme, and nothing a pad can drive.
void ImportPage::import_browser() {
  FileListSpec spec;
  spec.child_id = "import-browse";
  spec.height = 260;
  // Only .kgpack, because this page can read nothing else: a disc belongs to
  // the Library screen, and listing anything else here would be offering
  // sentences that say no.
  spec.show_file = [](const fs::path& p) { return to_lower(p.extension().string()) == ".kgpack"; };
  spec.empty_text = "no .kgpack here, and nowhere to go";
  FilePick pick = file_list(import_dir_, spec);
  if (pick.kind == FilePick::File) {
    drop_path_ = pick.path;
    import_replace_ = false;
    import_browsing_ = false;
  }
}

// A modal that could say only "Import X?" could not say what X is: a capsule
// or a recipe, which game, how large, whose root. So it is a page.
void ImportPage::draw() {
  PageWindow page("import", "Esc back", ctx_.fonts.big);

  if (drop_path_.empty()) {
    ImGui::TextWrapped(
        "Nothing to import yet. Pick a .kgpack below, or drop one on this window - a "
        "capsule carries the game itself, a recipe carries only the knowledge and rebuilds "
        "from your own disc.");
    ImGui::Spacing();
    import_browser();
    return;
  }

  std::error_code ec;
  inspect_for_import(drop_path_);
  const Inspected& in = import_;
  ImGui::Text("%s", drop_path_.filename().c_str());
  ImGui::SameLine(ImGui::GetWindowWidth() - 200);
  ImGui::TextDisabled("%s", human_size(in.bytes).c_str());
  ImGui::Spacing();

  // A different file, from the same page: whatever is on screen, including a
  // file that turned out not to be a pack at all.
  if (ImGui::SmallButton(import_browsing_ ? "close the browser" : "pick another file")) {
    import_browsing_ = !import_browsing_;
  }
  if (import_browsing_) import_browser();
  ImGui::Spacing();

  if (!in.trouble.empty()) {
    ImGui::TextWrapped("I cannot read that: %s", in.trouble.c_str());
    return;
  }
  const Meta& m = in.meta;
  const bool has_body = in.has_body;
  const Hash& root = in.root;

  ImGui::Indent(24);
  ImGui::Text("%s", m.name.empty() ? m.id.c_str() : m.name.c_str());
  ImGui::SameLine();
  if (m.year) ImGui::TextDisabled("%u", m.year);
  if (has_body) {
    std::string what = "a capsule: the game, its registry";
    size_t embedded = 0;
    for (const Meta::Disc& d : m.discs) if (d.embedded) ++embedded;
    what += embedded ? " and its " + std::to_string(embedded) +
                           (embedded == 1 ? " disc" : " discs")
                     : ", without its discs";
    ImGui::TextDisabled("%s", what.c_str());
  } else {
    // Not "proves the result": clicking through an installer twice need not
    // produce the same bytes, so what the rebuild gets is compared with the
    // root below and the difference reported, which is a different promise.
    ImGui::TextDisabled(
        "a recipe: it rebuilds from your own disc, and says how the tree compares");
  }
  ImGui::TextDisabled("root  %s", to_hex(root).substr(0, 16).c_str());
  ImGui::Unindent(24);
  ImGui::Spacing();

  bool installed = fs::exists(game_pack(m.id), ec);
  if (installed) {
    double seconds = 0;
    for (const session::Record& r : session::journal(m.id)) {
      seconds += static_cast<double>(r.ended - r.started);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s is already installed, with %s played against it.", m.id.c_str(),
                       human_time(seconds).c_str());
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "Importing over it replaces the pack. The saves and the snapshots stay where they "
        "are, because they are not in the pack.");
    ImGui::Checkbox("replace it", &import_replace_);
  }

  if (!has_body) {
    // A recipe's discs are resolved by fingerprint before anything starts, so
    // a set that is short a disc says which one now rather than failing at
    // install time. Resolved once, in inspect_for_import: matching by
    // fingerprint hashes the head of every image in the collection, and this
    // page is redrawn sixty times a second.
    const install::Prefill& pre = in.prefill;
    for (const std::string& missing : pre.missing) {
      ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      ImGui::TextWrapped("this recipe wants %s, and your collection does not have it",
                         missing.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::PushFont(ctx_.fonts.big);
    if (pre.missing.empty()) {
      if (ImGui::Button("Rebuild it from your disc", ImVec2(-1, 60))) {
        // By value: the next line puts the cached inspection out of date, and
        // `pre` is a reference into it.
        install::Prefill taken = pre;
        wizard_.begin_from_recipe(taken, root);
        drop_path_.clear();
        ctx_.go(Screen::Create);
      }
    } else {
      ImGui::BeginDisabled();
      ImGui::Button("Rebuild it from your disc", ImVec2(-1, 60));
      ImGui::EndDisabled();
    }
    ImGui::PopFont();
    return;
  }

  ImGui::Spacing();
  ImGui::PushFont(ctx_.fonts.big);
  bool blocked = installed && !import_replace_;
  if (blocked) ImGui::BeginDisabled();
  if (ImGui::Button("Import it", ImVec2(-1, 60))) {
    fs::path f = drop_path_;
    bool replace = import_replace_;
    ctx_.run_job("Importing " + f.filename().string(), [this, f, replace]() {
      install::ImportResult r =
          install::import_pack(ctx_.env, f, replace, [this](const std::string& l) { ctx_.job.log(l); });
      ctx_.job.log(r.name + " is in your library");
    });
    drop_path_.clear();
    ctx_.go(Screen::Shelf);
  }
  if (blocked) ImGui::EndDisabled();
  ImGui::PopFont();
}

}  // namespace kg::gui::shelf
