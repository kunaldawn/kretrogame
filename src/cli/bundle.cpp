// ---- kretro bundle ----------------------------------------------------------
//
// Building players without a window. The Bundles page is how an author builds
// one; this is the same build - bundle::build_from_draft, the call the page's
// button makes - for scripts, for tests, and for rebuilding a bundle the page
// remembers from a terminal.
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "../bundle/build.h"
#include "../bundle/builder.h"
#include "../bundle/meta.h"
#include "handlers.h"
#include "../rt/env.h"
#include "../session/saves_layout.h"
#include "../util/format.h"
#include "../util/paths.h"

namespace fs = std::filesystem;

namespace kg::cli {

namespace {

void build_and_say(const bundle::Draft& d, const bundle::BuildInputs& in, bundle::Built* built = nullptr) {
  bundle::Callbacks cb;
  int last = -1;
  std::string stage;
  cb.progress = [&](const bundle::Progress& p) {
    int pct = p.total ? static_cast<int>(p.done * 100 / p.total) : 100;
    if (p.stage != stage || pct / 10 != last / 10) {
      stage = std::string(p.stage);
      std::fprintf(stderr, "  %s %d%%\n", stage.c_str(), pct);
      last = pct;
    }
  };
  bundle::Built b = bundle::build_from_draft(d, in, cb);
  std::printf("%s\n  %s, %zu game%s, checked end to end\n", b.path.c_str(), fmt::bytes_iec(b.size).c_str(),
              d.games.size(), d.games.size() == 1 ? "" : "s");
  if (built) *built = b;
}

// kretro bundle build -o FILE|DIR [--id ID] [--title T] [--version V]
//                     [--base PLAYER-BASE] --rights GAME...
int bundle_build(const std::vector<std::string>& a) {
  bundle::Draft d;
  fs::path out, base;
  std::vector<std::string> games;
  for (size_t i = 0; i < a.size(); ++i) {
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= a.size()) throw std::runtime_error(std::string(what) + " needs a value");
      return a[++i];
    };
    if (a[i] == "-o") out = next("-o");
    else if (a[i] == "--id") { d.id = next("--id"); d.id_typed = true; }
    else if (a[i] == "--title") d.title = next("--title");
    else if (a[i] == "--version") d.version = next("--version");
    else if (a[i] == "--base") base = next("--base");
    else if (a[i] == "--rights") d.rights = true;
    else if (!a[i].empty() && a[i][0] == '-') throw std::runtime_error("bundle build does not know " + a[i]);
    else games.push_back(a[i]);
  }
  if (out.empty() || games.empty()) {
    std::fprintf(stderr, "kretro bundle build -o FILE|DIR [--id ID] [--title TITLE] [--version V]\n"
                         "                    [--base PLAYER-BASE] --rights GAME...\n");
    return 2;
  }
  if (!d.rights) {
    std::fprintf(stderr, "kretro: say --rights: \"I have the right to distribute these games.\" It is recorded "
                         "in the player, not checked; distributing them is on you.\n");
    return 2;
  }

  // Each game as the page first shows it: its name and year, its own gamepad
  // map, and the frame it was last played at as its cover.
  for (const std::string& id : games) {
    fs::path pk = game_pack(id);
    std::error_code ec;
    if (!fs::exists(pk, ec)) throw std::runtime_error(id + " is not installed");
    const fs::path frames = session::journal_dir(id);
    fs::path title = session::title_art_file(frames);
    bool cover = fs::exists(title, ec) && !fs::exists(session::title_marker_file(frames), ec);
    d.games.push_back(bundle::game_from_pack(bundle::read_pack_facts(pk), cover ? title : fs::path()));
  }
  if (d.title.empty()) d.title = d.games.size() == 1 ? d.games[0].name : (d.id.empty() ? "kretro bundle" : d.id);
  if (d.id.empty()) d.id = d.games.size() == 1 ? d.games[0].id : bundle::id_from_title(d.title);

  // -o names the file, or a folder to put <id>-<version>.run in.
  std::error_code ec;
  bundle::BuildInputs in = bundle::shelf_build_inputs(base);
  if (fs::is_directory(out, ec)) {
    d.out_dir = out.string();
  } else {
    d.out_dir = out.has_parent_path() ? out.parent_path().string() : ".";
    // Built at that name, not built as <id>-<version>.run and renamed: the
    // rename would first replace a file of that name in the folder - a player
    // shipped before - and would land over kretro or a shelf pack without
    // the build's check that the output is none of its inputs.
    in.out = out;
  }

  const bundle::DraftFacts facts = bundle::gather_draft_facts(d, false);
  const std::vector<bundle::GameFacts>& gf = facts.games;
  std::vector<std::string> stop = bundle::blockers(d, gf);
  if (!stop.empty()) {
    for (const std::string& s : stop) std::fprintf(stderr, "kretro: %s\n", s.c_str());
    return 1;
  }
  // What the page would ask the author to acknowledge, said here instead:
  // --rights is the one acknowledgement a script makes.
  const std::vector<bundle::Check> checks = bundle::run_checks(d, gf);
  for (const bundle::Check& c : checks) std::fprintf(stderr, "  note: %s\n", c.text.c_str());
  bundle::Built b;
  build_and_say(d, in, &b);
  // Remembered, as a build from the page is, so `bundle list` shows it and
  // `bundle rebuild <id>` makes the next one - and the Bundles page opens it.
  const std::string when = fmt::local_minute(std::time(nullptr));
  try {
    if (!bundle::remember_built(bundle::bundles_dir(), d, checks, b.path.string(), b.size, when)) {
      std::fprintf(stderr, "  a bundle '%s' is already remembered, with what was set on it; this build is "
                           "not remembered over it (kretro bundle rebuild %s builds that one)\n",
                   d.id.c_str(), d.id.c_str());
    }
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "  built, and not remembered: %s\n", ex.what());
  }
  return 0;
}

int bundle_list() {
  std::vector<std::string> unreadable;
  std::vector<bundle::Draft> all = bundle::load_drafts(bundle::bundles_dir(), &unreadable);
  if (all.empty() && unreadable.empty()) {
    std::printf("No bundles yet. The Bundles page on the shelf makes one; kretro bundle build makes one here.\n");
    return 0;
  }
  for (const bundle::Draft& d : all) {
    std::string games;
    for (const bundle::DraftGame& g : d.games) games += (games.empty() ? "" : ", ") + g.id;
    std::printf("%-24s %s %s  (%s)\n", d.id.c_str(), d.title.c_str(), d.version.c_str(), games.c_str());
    if (!d.last_built.empty()) {
      std::printf("%-24s last built %s: %s, %s\n", "", d.last_built_at.c_str(), d.last_built.c_str(),
                  fmt::bytes_iec(d.last_size).c_str());
    }
  }
  for (const std::string& u : unreadable) std::fprintf(stderr, "  cannot read %s\n", u.c_str());
  return 0;
}

// kretro bundle rebuild <id> [--base PLAYER-BASE]: what the page's Build button
// does for a remembered bundle, held to the same conditions.
int bundle_rebuild(const std::vector<std::string>& a) {
  std::string id;
  fs::path base;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] == "--base" && i + 1 < a.size()) base = a[++i];
    else if (!a[i].empty() && a[i][0] == '-') throw std::runtime_error("bundle rebuild does not know " + a[i]);
    else id = a[i];
  }
  if (id.empty()) {
    std::fprintf(stderr, "kretro bundle rebuild <id> [--base PLAYER-BASE]\n");
    return 2;
  }
  std::optional<bundle::Draft> found;
  for (const bundle::Draft& d : bundle::load_drafts(bundle::bundles_dir())) {
    if (d.id == id) found = d;
  }
  if (!found) throw std::runtime_error("no bundle is remembered as '" + id + "'; kretro bundle list says which are");
  bundle::Draft d = *found;
  const bundle::DraftFacts facts = bundle::gather_draft_facts(d, true);
  const std::vector<bundle::GameFacts>& gf = facts.games;
  std::vector<bundle::Check> checks = bundle::run_checks(d, gf);
  std::vector<std::string> stop = bundle::blockers(d, gf);
  if (!bundle::ready_to_build(d, checks, stop)) {
    for (const std::string& s : stop) std::fprintf(stderr, "kretro: %s\n", s.c_str());
    for (const bundle::Check& c : checks) {
      if (!d.acked(c.id)) std::fprintf(stderr, "kretro: not acknowledged on the Bundles page: %s\n", c.text.c_str());
    }
    if (!d.rights) std::fprintf(stderr, "kretro: \"I have the right to distribute these games\" is not ticked.\n");
    return 1;
  }
  build_and_say(d, bundle::shelf_build_inputs(base));
  {
    const std::string when = fmt::local_minute(std::time(nullptr));
    fs::path p = fs::path(d.out_dir) / bundle::output_name(d);
    std::error_code ec;
    bundle::stamp_built(bundle::bundles_dir(), d.id, p.string(), fs::file_size(p, ec), when);
  }
  return 0;
}

}  // namespace

int cmd_bundle(const rt::Env& /*e*/, std::vector<std::string>& a) {
  const std::string sub = a.empty() ? "" : a[0];
  std::vector<std::string> rest(a.begin() + (a.empty() ? 0 : 1), a.end());
  if (sub == "build") return bundle_build(rest);
  if (sub == "list") return bundle_list();
  if (sub == "rebuild") return bundle_rebuild(rest);
  std::fprintf(stderr,
               "kretro bundle build -o FILE|DIR [--id ID] [--title T] [--version V] [--base FILE] --rights GAME...\n"
               "kretro bundle list\n"
               "kretro bundle rebuild <id> [--base FILE]\n");
  return 2;
}

// The name it had while it was a hidden test hook; scripts still use it.
int cmd_bundle_build(const rt::Env& /*e*/, std::vector<std::string>& a) { return bundle_build(a); }

}  // namespace kg::cli
