// One game's timeline: every snapshot, newest first, with what the session
// that left it did, and going back to one.
#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "../../session/journal.h"
#include "../../session/saves.h"
#include "../format.h"
#include "../palette.h"
#include "../widgets.h"
#include "imgui.h"
#include "pages.h"

namespace kg::gui::shelf {

// The overlay already captured every write and the harvester already
// photographed every session. Putting them on one axis is a rewind button for
// games that never had one.
void TimelinePage::draw() {
  if (ctx_.entries.empty()) { ctx_.go(Screen::Shelf); return; }
  const Entry& en = ctx_.entries[ctx_.selected < ctx_.entries.size() ? ctx_.selected : 0];
  // The band says the game's name, so the header only says where it is.
  set_page_trail("kretro \xe2\x80\xba " + en.name + " \xe2\x80\xba timeline");
  PageWindow page((en.name + " - timeline").c_str(), "Esc back", ctx_.fonts.big());

  // Read when the page comes up, after a session and after a restore, not
  // on every frame.
  if (disk_.due(en.id + "\n" + std::to_string(ctx_.generation))) {
    gens_ = session::generations(en.id);
    recs_ = session::journal(en.id);
  }
  const std::vector<std::string>& gens = gens_;
  const std::vector<session::Record>& recs = recs_;

  // One region scrolls the page, the band with it, and the focus moving
  // from card to card brings each into view.
  begin_page_scroll("snapshots");
  // A low band of the game's picture under its name, as its page has a tall
  // one: this is still the game's page, looked at another way.
  const std::filesystem::path& shot = en.last_png.empty() ? en.title_png : en.last_png;
  HeroSpec hs;
  hs.backdrop = ctx_.texture(shot) ? ctx_.textures.backdrop_file(shot) : nullptr;
  hs.id = en.id;
  hs.title = en.name;
  hs.kicker = "timeline";
  if (!gens.empty()) hs.kicker += " \xc2\xb7 " + std::to_string(gens.size()) + (gens.size() == 1 ? " snapshot" : " snapshots");
  hs.height = 140;
  hs.art_right = false;
  hero(hs);
  vgap(8);

  if (gens.empty()) {
    empty_state(
        "Nothing to go back to yet. Every time you play, whatever the game writes is kept "
        "as a snapshot, and they appear here newest first - when it was, how long you "
        "played, what it wrote - with the last picture taken of the game beside the "
        "newest of them.");
    end_page_scroll();
    return;
  }

  // A history, the way `git log --graph` draws one: a card per snapshot on a
  // rail down the left, the newest at the top with the last picture taken of
  // the game, and what each session did on its card. The Restore buttons are
  // one column down the right, so Up and Down go from card to card.
  nav_section_begin("cards");
  // No wider than reads well, so a button stays near what it restores.
  const float column = std::min(ImGui::GetContentRegionAvail().x, content_max_w(Content::Reading));
  for (size_t i = gens.size(); i-- > 0;) {
    const std::string& g = gens[i];
    ImGui::PushID(g.c_str());
    const session::Record* rec = nullptr;
    for (const session::Record& r : recs) if (r.generation == g) rec = &r;
    SnapshotCard card;
    card.gen = g;
    card.id = en.id;
    card.width = column;
    card.first = i + 1 == gens.size();
    card.last = i == 0;
    if (card.first) card.art = ctx_.texture(en.last_png);
    if (rec) {
      card.when = ago(rec->ended);
      card.played = human_time(static_cast<double>(rec->ended - rec->started));
      card.files = std::to_string(rec->files_written) + " files written";
      card.note = rec->note;
    }
    if (snapshot_card(card)) {
      restore_gen_ = g;
      confirm_restore_ = true;
    }
    // The page opens on the newest snapshot's Restore.
    if (card.first) default_focus();
    ImGui::PopID();
  }
  nav_section_end();
  end_page_scroll();

  if (confirm_restore_) {
    ImGui::OpenPopup("Restore?");
    confirm_restore_ = false;
  }
  // Headed with the question as its title; the popup's name is only its ID.
  if (dialog_begin("Restore?", 560, "Restore snapshot " + restore_gen_ + "?")) {
    ImGui::TextWrapped(
        "Everything the game has written since then is replaced by what it had "
        "written by then. The current state is kept as a new snapshot first, so "
        "this is reversible.");
    dialog_footer();
    // What is there now is replaced, so the button says it in the colour
    // that means care, and the green is kept for going ahead.
    if (dialog_button("Restore", DialogButton::Danger)) {
      try {
        session::restore(en.id, restore_gen_);
        ctx_.status = "restored " + restore_gen_;
      } catch (const std::exception& ex) {
        ctx_.status = std::string("could not restore: ") + ex.what();
      }
      // A restore keeps what was there as a snapshot of its own.
      disk_.invalidate();
      ImGui::CloseCurrentPopup();
    }
    // Cancel is what Escape and the pad's B answer, and where the focus
    // starts.
    if (dialog_button("Cancel", DialogButton::Secondary, true)) ImGui::CloseCurrentPopup();
    dialog_end();
  }
}

}  // namespace kg::gui::shelf
