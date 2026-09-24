#include "scan.h"

#include <algorithm>
#include <cstdio>

#include "../../install/collection.h"
#include "../../install/discs.h"
#include "../../install/manifest.h"
#include "../../util/paths.h"

namespace kg::gui {
namespace fs = std::filesystem;

std::vector<Entry> scan(const rt::Env& e) {
  std::vector<Entry> out;
  std::error_code ec;
  ensure_state_dirs();

  for (const fs::directory_entry& de : fs::directory_iterator(games_dir(), ec)) {
    if (de.path().extension() != ".kgpack") continue;
    Entry en;
    try {
      Pack p = Pack::open(de.path());
      en.id = p.meta().id;
      en.name = p.meta().name;
      en.year = p.meta().year;
      en.tree_bytes = p.meta().tree.total_bytes();
      en.files = p.meta().tree.size();
      en.flat_body = !p.meta().rooted();
      for (const Meta::Disc& d : p.meta().discs) {
        if (d.embedded) continue;
        en.absent_discs.push_back(d.label.empty() ? d.ref : d.label);
      }
    } catch (const std::exception&) {
      continue;
    }
    en.pack = de.path();
    en.pack_bytes = fs::file_size(de.path(), ec);

    // Spelled out rather than through session::journal_dir and its
    // neighbours: this starts from saves_dir()/<id>, not game_saves_dir(id),
    // and the two need not be the same directory.
    fs::path j = saves_dir() / en.id / "journal";
    if (fs::exists(j / "title.png", ec)) en.title_png = j / "title.png";
    if (fs::exists(j / "last.png", ec)) en.last_png = j / "last.png";

    for (const session::Record& r : session::journal(en.id)) {
      en.sessions++;
      en.total_seconds += static_cast<double>(r.ended - r.started);
      if (r.ended > en.last_played) {
        en.last_played = r.ended;
        if (!r.note.empty()) en.last_note = r.note;
      }
    }
    out.push_back(std::move(en));
  }

  // Games that could be installed but are not, so the shelf also answers
  // "what else have I got discs for".
  for (const std::string& id : install::known_games(e)) {
    if (std::any_of(out.begin(), out.end(), [&](const Entry& x) { return x.id == id; })) continue;
    Entry en;
    en.id = id;
    en.installed = false;
    try {
      Meta m = install::load_manifest(install::find_manifest(e, id));
      en.name = m.name;
      en.year = m.year;
      // A disc reference carries a #LABEL or #N suffix naming which disc of
      // the archive it means, and no file on disk is called that: what has to
      // be there is the archive. Named whole, so the game's page can say what
      // is missing instead of "a disc".
      for (const std::string& d : m.recipe.discs) {
        std::string archive = install::parse_disc_ref(d).archive;
        if (!install::find_iso(archive).empty()) continue;
        if (std::find(en.missing_discs.begin(), en.missing_discs.end(), archive) ==
            en.missing_discs.end()) {
          en.missing_discs.push_back(archive);
        }
      }
      if (!en.missing_discs.empty()) {
        en.blocked = true;
        en.blocked_reason = "needs " + en.missing_discs[0];
      }
    } catch (const std::exception& ex) {
      en.name = id;
      en.blocked = true;
      en.blocked_reason = ex.what();
    }
    out.push_back(std::move(en));
  }

  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    // Installed first, then most recently played - so the game you are in the
    // middle of is the one under the cursor when the shelf opens.
    if (a.installed != b.installed) return a.installed;
    if (a.last_played != b.last_played) return a.last_played > b.last_played;
    return a.name < b.name;
  });
  return out;
}

}  // namespace kg::gui
