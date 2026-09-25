// A .kgpack: what it is - a capsule or a recipe, which game, how large, whose
// root - and taking it in. Reached from the shelf, from the Library screen,
// and by dropping a pack on the window.
#include <algorithm>
#include <cmath>
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
void ImportPage::import_browser(float height) {
  FileListSpec spec;
  spec.child_id = "import-browse";
  spec.height = height;
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
  set_page_trail("kretro \xe2\x80\xba import");
  PageWindow page("import", "Esc back", ctx_.fonts.big());

  if (drop_path_.empty()) {
    centre_column(content_max_w(Content::Form));
    step_heading("Import");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "Nothing to import yet. Pick a .kgpack below, or drop one on this window - a "
        "capsule carries the game itself, a recipe carries only the knowledge and rebuilds "
        "from your own disc.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    // With nothing chosen the browser is the page, so it takes the room
    // there is. The spec's height is in design pixels.
    import_browser(std::max(120.0f, ImGui::GetContentRegionAvail().y / ui_scale()));
    end_centre_column();
    return;
  }

  std::error_code ec;
  inspect_for_import(drop_path_);
  const Inspected& in = import_;
  // Everything under the header scrolls, so the file browser opening under a
  // long description never pushes the button off the bottom of the window.
  // The keys go through it as through the page. Its content is a column of a
  // readable width, so the facts, the warning and the button read as one
  // block.
  begin_scroll("import", ImVec2(0, 0));
  centre_column(content_max_w(Content::Form));
  step_heading("Import");

  // What happens to the file, and the way to another: the pack's card, and
  // the button that takes it in under it with "pick another file" beside it.
  // Reading it failed: the card says why, and only the other way is left.
  auto another = [&] {
    if (ghost_button(import_browsing_ ? "close the browser" : "pick another file", ImVec2(0, std::round(px(40))))) {
      import_browsing_ = !import_browsing_;
    }
  };
  auto browser = [&] {
    if (!import_browsing_) return;
    vgap(6);
    nav_section_begin("browser");
    import_browser(260);
    nav_section_end();
  };
  auto finish = [&] {
    end_centre_column();
    end_scroll();
  };

  nav_section_begin("card");
  card_begin("pack", "pack");
  {
    // The file, and its size straight after it; the name gives way to the
    // size.
    const std::string size = human_size(in.bytes);
    const float sw = ImGui::CalcTextSize(size.c_str()).x;
    const float room = ImGui::GetContentRegionAvail().x - sw - ImGui::GetStyle().ItemSpacing.x * 2;
    ImGui::TextColored(kCyan, "%s", elide(nullptr, drop_path_.filename().string(), room).c_str());
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
    ImGui::TextDisabled("%s", size.c_str());
  }

  if (!in.trouble.empty()) {
    ImGui::Spacing();
    badge_line(BadgeKind::Fail, "I cannot read that: " + in.trouble, kError);
    card_end();
    vgap(8);
    another();
    default_focus();
    nav_section_end();
    browser();
    finish();
    return;
  }
  const Meta& m = in.meta;
  const bool has_body = in.has_body;
  const Hash& root = in.root;

  vgap(4);
  if (kv_begin("what")) {
    std::string game = m.name.empty() ? m.id : m.name;
    if (m.year) game += " (" + std::to_string(m.year) + ")";
    kv("game", game);
    if (has_body) {
      std::string what = "a capsule: the game, its registry";
      size_t embedded = 0;
      for (const Meta::Disc& d : m.discs)
        if (d.embedded) ++embedded;
      what += embedded ? " and its " + std::to_string(embedded) +
                             (embedded == 1 ? " disc" : " discs")
                       : ", without its discs";
      kv("kind", what, kDim);
    } else {
      // Not "proves the result": clicking through an installer twice need not
      // produce the same bytes, so what the rebuild gets is compared with the
      // root below and the difference reported, which is a different promise.
      kv("kind", "a recipe: it rebuilds from your own disc, and says how the tree compares", kDim);
    }
    // The first sixteen digits of the hash the pack's tree is checked
    // against: enough to tell two packs apart, and said as such.
    kv("root hash", to_hex(root).substr(0, 16) + "\xe2\x80\xa6", kDim);
    kv_end();
  }

  // Whether it is installed already and how much it has been played, looked
  // up when the page comes up and when the collection is read again, rather
  // than a journal read on every frame.
  if (disk_.due(m.id + "\n" + std::to_string(ctx_.generation))) {
    installed_ = fs::exists(game_pack(m.id), ec);
    played_ = 0;
    if (installed_) {
      for (const session::Record& r : session::journal(m.id)) played_ += static_cast<double>(r.ended - r.started);
    }
  }
  const bool installed = installed_;
  if (installed) {
    const double seconds = played_;
    ImGui::Spacing();
    begin_badge_block(BadgeKind::Warn);
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s is already installed, with %s played against it.", m.id.c_str(),
                       human_time(seconds).c_str());
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "Importing over it replaces the pack. The saves and the snapshots stay where they "
        "are, because they are not in the pack.");
    ImGui::Checkbox("replace it", &import_replace_);
    end_badge_block();
  }

  if (!has_body) {
    // A recipe's discs are resolved by fingerprint before anything starts, so
    // a set that is short a disc says which one now rather than failing at
    // install time. Resolved once, in inspect_for_import: matching by
    // fingerprint hashes the head of every image in the collection, and this
    // page is redrawn sixty times a second.
    const install::Prefill& pre = in.prefill;
    for (const std::string& missing : pre.missing) {
      badge_line(BadgeKind::Fail, "this recipe wants " + missing + ", and your collection does not have it",
                 kWarn);
    }
    card_end();
    vgap(8);
    if (pre.missing.empty()) {
      const bool rebuild = play_button("Rebuild it from your disc", PlayKind::Install);
      default_focus();
      if (rebuild) {
        // By value: the next line puts the cached inspection out of date, and
        // `pre` is a reference into it.
        install::Prefill taken = pre;
        wizard_.begin_from_recipe(taken, root);
        drop_path_.clear();
        ctx_.go(Screen::Create);
      }
    } else {
      ImGui::BeginDisabled();
      play_button("Rebuild it from your disc", PlayKind::Install);
      ImGui::EndDisabled();
    }
    ImGui::SameLine(0, std::round(px(16)));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::round((ImGui::GetItemRectSize().y - px(40)) * 0.5f));
    another();
    nav_section_end();
    browser();
    finish();
    return;
  }

  card_end();
  vgap(8);
  bool blocked = installed && !import_replace_;
  if (blocked) ImGui::BeginDisabled();
  const bool take = play_button("Import it", PlayKind::Install);
  // The page opens on the button that takes the pack in, when it can.
  default_focus();
  if (take) {
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
  ImGui::SameLine(0, std::round(px(16)));
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::round((ImGui::GetItemRectSize().y - px(40)) * 0.5f));
  another();
  nav_section_end();
  browser();
  finish();
}

}  // namespace kg::gui::shelf
