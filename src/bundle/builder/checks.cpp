#include "checks.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

#include "../../install/survey.h"
#include "../../util/safe_names.h"
#include "../../util/text.h"
#include "../toc.h"
#include "exe_probe.h"
#include "key_fragment.h"
#include "pack_facts.h"

namespace kg::bundle {
namespace fs = std::filesystem;

// ---- checks -----------------------------------------------------------------------------

namespace {

bool is_dgvoodoo_file(const std::string& name) {
  std::string n = to_lower(fs::path(name).filename().string());
  return n.find("dgvoodoo") != std::string::npos;
}

bool tree_has(const Meta& m, std::initializer_list<const char*> names) {
  for (const TreeEntry& e : m.tree.entries()) {
    std::string n = to_lower(fs::path(e.path).filename().string());
    for (const char* w : names) {
      if (n == w) return true;
    }
  }
  return false;
}

}  // namespace

std::vector<Check> run_checks(const Draft& d, const std::vector<GameFacts>& games) {
  std::vector<Check> out;
  for (size_t i = 0; i < d.games.size() && i < games.size(); ++i) {
    const DraftGame& g = d.games[i];
    const GameFacts& f = games[i];
    const std::string name = g.display_name();

    if (f.pack) {
      const Meta& m = f.pack->meta;
      // Protection: the wizard's signs, over the whole installed tree rather
      // than one directory, since a pack remembers every file it holds.
      std::vector<std::string> seen;
      for (const TreeEntry& e : m.tree.entries()) {
        std::string file = fs::path(e.path).filename().string();
        std::string what = install::protection_of(file);
        if (what.empty() || std::find(seen.begin(), seen.end(), what) != seen.end()) continue;
        seen.push_back(what);
        // Appended a piece at a time: one chain of + would build a temporary
        // for every step, once per protected file.
        std::string why = name + " carries ";
        why += file + ", which is ";
        why += what + ". The player gives it a CD-ROM drive made of files, which cannot answer "
                      "the raw-sector read ";
        why += what + " makes, so the game may refuse to start.";
        out.push_back({"protection:" + to_lower(what) + ":" + g.id, g.id, std::move(why), ""});
      }

      // The author's key.
      if (auto k = key_in_fragment(m.registry.fragment, f.vault_key)) {
        out.push_back({"key-in-pack:" + g.id, g.id,
                       "registry.reg in " + name + "'s pack still holds a key (" + k->hive + "\\" + k->key +
                           "\\" + k->name + "). It would be in every copy of this bundle, readable by "
                           "anyone who has one.",
                       "Install the game again with this kretro, which keeps keys out of the pack."});
      } else if (!f.vault_key.empty() && !g.embed_key) {
        out.push_back({"key-removed:" + g.id, g.id,
                       "Your key for " + name + " was kept out of registry.reg. If the game asks for "
                       "one when it starts, each player types their own.",
                       "Embed my key in this bundle, under " + name + "."});
      }
      if (g.embed_key) {
        out.push_back({"key-embedded:" + g.id, g.id,
                       "Your key for " + name + " will be embedded: it will be in every copy of this "
                       "bundle, and anyone who has one can read it.",
                       "Untick \"Embed my key in this bundle\"."});
      }

      // dgVoodoo.
      bool supplied = std::any_of(g.extra_dlls.begin(), g.extra_dlls.end(),
                                  [](const GameMeta::Dll& x) { return is_dgvoodoo_file(x.name); });
      bool in_tree = tree_has(m, {"dgvoodoo.conf", "dgvoodoocpl.exe"});
      if (supplied || in_tree) {
        out.push_back({"dgvoodoo:" + g.id, g.id,
                       "dgVoodoo is in this bundle for " + name + (in_tree ? ", inside the game's own files" : "") +
                           ". Its licence allows it with one game, as here, but not in a launcher for "
                           "general use; distributing it is on you, not on kretro.",
                       ""});
      } else if (m.runtime.dgvoodoo) {
        out.push_back({"dgvoodoo-missing:" + g.id, g.id,
                       name + " was set up to run under dgVoodoo, which the player does not carry. It "
                       "will run without it.",
                       "Add dgVoodoo's files for this game under Per game."});
      }

      // Glide.
      if (f.imports && glide_only(*f.imports)) {
        out.push_back({"glide:" + g.id, g.id,
                       name + "'s executable draws only through Glide, and the player has no Glide. It "
                       "needs a wrapper for it supplied with the game, or a Direct3D or OpenGL renderer "
                       "chosen in the game's own setup.",
                       ""});
      } else if (f.imports && f.imports->ok && tree_has(m, {"glide.dll", "glide2x.dll", "glide3x.dll"}) &&
                 !f.imports->imports("ddraw") && !f.imports->imports("opengl32") &&
                 !f.imports->imports("d3d8") && !f.imports->imports("d3d9")) {
        // A renderer loaded by name at run time imports nothing; the files are
        // the only evidence, and they are only evidence.
        out.push_back({"glide-maybe:" + g.id, g.id,
                       name + " carries Glide libraries and its executable imports no other renderer: "
                       "it may draw only through Glide, which the player does not have.",
                       ""});
      }
    }

    if (g.cover.empty()) {
      out.push_back({"cover:" + g.id, g.id,
                     name + " has no cover art: its tile shows its name on a colour.",
                     "Pick a cover under Per game."});
    }
  }
  return out;
}

std::vector<std::string> blockers(const Draft& d, const std::vector<GameFacts>& games) {
  std::vector<std::string> out;
  if (d.title.empty()) out.push_back("The bundle has no title.");
  if (!kg::id_is_safe(d.id)) {
    out.push_back("The bundle id '" + d.id + "' cannot be a directory name: letters, digits, '-', '_' and '.' only.");
  }
  if (!version_is_safe(d.version)) {
    out.push_back("The version '" + d.version + "' cannot be part of a file name: letters, digits, '-', '_' and '.' only.");
  }
  if (d.games.empty()) out.push_back("Choose at least one game.");
  std::vector<std::string> ids;
  for (size_t i = 0; i < d.games.size(); ++i) {
    const DraftGame& g = d.games[i];
    const std::string name = g.display_name();
    if (std::find(ids.begin(), ids.end(), g.id) != ids.end()) out.push_back(name + " is in the bundle twice.");
    ids.push_back(g.id);
    if (g.name.empty()) out.push_back(g.id + " has no name.");
    if (g.id.size() > kNameSize) out.push_back(g.id + " has an id longer than a bundle can name.");
    const GameFacts* f = i < games.size() ? &games[i] : nullptr;
    if (!f || !f->pack) {
      out.push_back(name + " is no longer on the shelf.");
    }
    for (const GameMeta::Dll& x : g.extra_dlls) {
      if (!kg::id_is_safe(x.name)) out.push_back(name + " has an extra file named '" + x.name + "', which cannot be placed.");
    }
    if (g.embed_key) {
      if (!f || f->vault_key.empty()) {
        out.push_back("You asked to embed your key for " + name + ", but the keys vault has none for it: "
                      "kretro key " + g.id + " <key> stores one.");
      }
      if (g.key_path.empty() || g.key_value.empty()) {
        out.push_back("Say where " + name + "'s key goes in the registry: a key path and a value name.");
      }
    }
  }
  if (d.out_dir.empty()) out.push_back("Choose a folder to write the bundle to.");
  return out;
}

bool ready_to_build(const Draft& d, const std::vector<Check>& checks, const std::vector<std::string>& blocking) {
  if (!blocking.empty() || !d.rights) return false;
  return std::all_of(checks.begin(), checks.end(), [&](const Check& c) { return d.acked(c.id); });
}

}  // namespace kg::bundle
