// kretro - old games, one binary.
//
// By the time main runs we are already under the runtime's own loader, against
// the runtime's own libraries. Nothing below links anything from the host.
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "disc/disc.h"
#include "disc/drive.h"
#include "disc/serial.h"
#include <SDL2/SDL.h>
#include <unistd.h>

#include "bundle/build.h"
#include "bundle/builder.h"
#include "bundle/meta.h"
#include "config/config.h"
#include "config/scaling.h"
#include "gpu/probe.h"
#include "gui/app.h"
#include "gui/input.h"
#include "gui/stage.h"
#include "install/install.h"
#include "install/discs.h"
#include "install/keys.h"
#include "install/iso.h"
#include "install/share.h"
#include "pack/kgpack.h"
#include "player/doctor.h"
#include "rt/env.h"
#include "session/session.h"
#include "util/paths.h"

namespace fs = std::filesystem;
using namespace kg;

namespace {

int usage() {
  std::fputs(
      "kretro - old games, one binary\n"
      "\n"
      "  kretro                       the shelf\n"
      "  kretro create                install a game from your own discs\n"
      "  kretro doctor [--save FILE]  what this machine can and cannot do; --save\n"
      "                               writes it without your home directory or name\n"
      "  kretro info                  runtime, state and library paths\n"
      "  kretro games                 the manifests, and whether their discs are here\n"
      "  kretro identify <iso>        what a disc image is\n"
      "  kretro key <id> [serial]     show or store a game's serial\n"
      "  kretro display <id>          where this game lands on this screen\n"
      "  kretro swap <id> <n>         put disc n in the drive, mid-install\n"
      "  kretro scan [dir] [--write-db]  identify every disc in a directory\n"
      "  kretro contents <archive[#LABEL]>   what is on a disc\n"
      "  kretro install <id> [--headless] [--force] [--keep-tree] [--no-discs]\n"
      "                               open the wizard on a game we have a manifest for;\n"
      "                               --headless installs a copy or unzip game with no\n"
      "                               display, for scripts. An installer you have to click\n"
      "                               through has no headless form and refuses.\n"
      "                               --no-discs leaves the discs out of the pack.\n"
      "  kretro list                  your collection\n"
      "  kretro verify <id>           check a pack's tree root and its body\n"
      "  kretro uninstall <id>\n"
      "  kretro play <id> [--fullscreen] [--integer|--fit|--native] [--scale N] [--dry-run]\n"
      "                    [--note \"...\"]\n"
      "  kretro show <id>             open the shelf on one game\n"
      "  kretro export <id> [--recipe|--capsule] [-o FILE]\n"
      "                               a game for another kretro: the whole pack, or\n"
      "                               the recipe to rebuild it from the same disc\n"
      "  kretro export-saves <id>     just what the game wrote\n"
      "  kretro import <file>         a capsule, or a recipe to rebuild\n"
      "  kretro bundle build -o FILE|DIR [--id ID] [--title T] [--version V]\n"
      "                    [--base PLAYER-BASE] --rights GAME...\n"
      "                               a player for these games, the file you ship;\n"
      "                               the Bundles page on the shelf is the same build\n"
      "  kretro bundle list           the bundles the Bundles page remembers\n"
      "  kretro bundle rebuild <id>   build one of them again, as its Build button would\n"
      "  kretro import-saves <id> <file>  put somebody's saves back\n"
      "  kretro compare <id>          photograph each renderer so you can pick\n"
      "  kretro journal <id>          when you last played, and where you were\n"
      "  kretro saves <id>            the session timeline\n"
      "  kretro restore <id> <gen>    go back to a snapshot\n"
      "  kretro wine [args...]        run the bundled Wine\n"
      "  kretro exec <prog> [args]    run a bundled program\n"
      "  kretro input --display D --pid N [--game <id>] [--no-pause]\n"
      "                               gamepad translation for a running game\n"
      "\n",
      stderr);
  return 2;
}

std::string human(uint64_t n) {
  const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(n);
  int i = 0;
  while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
  char b[64];
  std::snprintf(b, sizeof(b), i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
  return b;
}

void say(const std::string& s) { std::fprintf(stderr, "  %s\n", s.c_str()); }

std::string human_time(double seconds) {
  long s = static_cast<long>(seconds);
  char b[64];
  if (s < 60) std::snprintf(b, sizeof(b), "%lds", s);
  else if (s < 3600) std::snprintf(b, sizeof(b), "%ldm %lds", s / 60, s % 60);
  else std::snprintf(b, sizeof(b), "%ldh %ldm", s / 3600, (s % 3600) / 60);
  return b;
}

// "8 months ago" is the thing a person actually needs to see after a long
// absence; a timestamp is not.
std::string ago(std::time_t then) {
  if (then <= 0) return "never";
  double d = difftime(std::time(nullptr), then);
  const struct { double secs; const char* unit; } steps[] = {
      {60, "second"}, {3600, "minute"}, {86400, "hour"},
      {86400 * 30.0, "day"}, {86400 * 365.0, "month"}, {1e18, "year"}};
  double div[] = {1, 60, 3600, 86400, 86400 * 30.0, 86400 * 365.0};
  for (int i = 0; i < 6; ++i) {
    if (d < steps[i].secs) {
      long n = static_cast<long>(d / div[i]);
      char b[64];
      std::snprintf(b, sizeof(b), "%ld %s%s ago", n, steps[i].unit, n == 1 ? "" : "s");
      return b;
    }
  }
  return "long ago";
}

int cmd_journal(const std::string& id) {
  std::vector<session::Record> j = session::journal(id);
  if (j.empty()) {
    std::printf("No sessions recorded for %s yet.\n", id.c_str());
    return 0;
  }
  double total = 0;
  for (const session::Record& r : j) total += static_cast<double>(r.ended - r.started);
  std::printf("%s\n", id.c_str());
  std::printf("  %zu session%s, %s in total\n", j.size(), j.size() == 1 ? "" : "s",
              human_time(total).c_str());
  std::printf("  last played %s\n\n", ago(j.front().ended).c_str());
  for (const session::Record& r : j) {
    char when[64];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&r.started));
    std::printf("  %s  %-9s  %zu file%s written%s\n", when,
                human_time(static_cast<double>(r.ended - r.started)).c_str(), r.files_written,
                r.files_written == 1 ? "" : "s",
                r.generation.empty() ? "" : ("  -> " + r.generation).c_str());
    if (!r.note.empty()) std::printf("      \"%s\"\n", r.note.c_str());
  }
  return 0;
}

int cmd_saves(const std::string& id) {
  std::vector<std::string> g = session::generations(id);
  if (g.empty()) {
    std::printf("No snapshots for %s yet. They are made when a session writes something.\n", id.c_str());
    return 0;
  }
  std::printf("%s: %zu snapshot%s\n\n", id.c_str(), g.size(), g.size() == 1 ? "" : "s");
  for (const std::string& s : g) std::printf("  %s\n", s.c_str());
  std::printf("\n  kretro restore %s <snapshot>\n", id.c_str());
  return 0;
}

int cmd_info(const rt::Env& e) {
  std::printf("runtime       %s\n", e.valid() ? e.root.c_str() : "(none - running outside the bootstrap)");
  if (e.valid() && fs::exists(e.root / "RUNTIME")) {
    std::ifstream f(e.root / "RUNTIME");
    std::string line;
    while (std::getline(f, line)) std::printf("  %s\n", line.c_str());
  }
  std::printf("state         %s\n", state_dir().c_str());
  std::printf("  games       %s\n", games_dir().c_str());
  std::printf("  saves       %s\n", saves_dir().c_str());
  std::printf("  prefixes    %s\n", prefixes_dir().c_str());
  std::printf("discs         %s\n", install::iso_dir().c_str());
  if (e.valid()) {
    fs::path w = rt::find_wine(e.root);
    std::printf("wine          %s\n", w.empty() ? "not found in this runtime" : w.c_str());
  }
  return 0;
}

// The GPU report, the host's capabilities and the runtime's own parts, as one
// report with a line to act on for each thing wrong. --save writes the same
// text with the home directory and the user name taken out, for pasting into
// a bug report somebody else will read.
int cmd_doctor(const std::vector<std::string>& a) {
  namespace doc = player::doctor;
  // Anything but nothing or "--save FILE" is a mistake, and saying so beats
  // printing the report and letting "--save" alone look as if it had saved.
  if (!a.empty() && !(a.size() == 2 && a[0] == "--save")) return usage();
  ensure_state_dirs();
  gpu::Report r = gpu::probe();
  gpu::materialize(r);
  rt::Env e = rt::make(&r);

  doc::Inputs in = doc::gather(e, r);

  doc::Section routing{"driver routing", {}};
  if (r.env.empty()) routing.lines.push_back({"loaders", "their own defaults"});
  for (const auto& [k, v] : r.env) routing.lines.push_back({k, v});
  in.extra.push_back(routing);

  doc::Section rts{"runtime", {}};
  if (!e.valid()) {
    rts.lines.push_back({"root", "none - kretro was started without its bootstrap"});
    in.caps.problems.push_back({gpu::Problem::Severity::Blocking, "kretro was started without its runtime",
                                "Run the kretro file itself, not the program inside it"});
  } else {
    rts.lines.push_back({"root", e.root.string()});
    struct { const char* label; const char* name; } needed[] = {
        {"wine", "wine"}, {"weston", "weston"}, {"xwayland", "Xwayland"}, {"7z", "7z"},
        {"vulkaninfo", "vulkaninfo"},
    };
    for (const auto& n : needed) {
      fs::path p = std::string(n.name) == "wine" ? rt::find_wine(e.root) : rt::which(e, n.name);
      rts.lines.push_back({n.label, p.empty() ? "MISSING" : p.string()});
    }
  }
  in.extra.push_back(rts);

  // The newest session's compositor log: when a game dies at once, this is
  // where Weston and Xwayland said why.
  fs::path newest;
  fs::file_time_type when{};
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(home_dir(), ec)) {
    fs::path log = de.path() / "weston.log";
    if (!fs::exists(log, ec)) continue;
    fs::file_time_type t = fs::last_write_time(log, ec);
    if (newest.empty() || t > when) {
      newest = log;
      when = t;
    }
  }
  if (!newest.empty()) in.log_tail = doc::tail_lines(newest, 40);

  doc::Report rep = doc::collect(in);
  std::string text = doc::render(rep);
  std::fputs(text.c_str(), stdout);

  if (a.size() == 2 && a[0] == "--save") {
    std::ofstream out(a[1]);
    out << doc::redact(text);
    if (!out) {
      std::fprintf(stderr, "kretro: could not write %s\n", a[1].c_str());
      return 1;
    }
    std::printf("\nsaved to %s, without your home directory or user name\n", a[1].c_str());
  }
  // Only what stops a game from starting fails the command; a warning such as
  // "no hidraw access" is true of most machines and would make it always fail.
  return rep.blocking() ? 1 : 0;
}

int cmd_games(const rt::Env& e) {
  std::vector<std::string> ids = install::known_games(e);
  if (ids.empty()) {
    std::printf("No manifests found. Looked in:\n");
    for (const fs::path& d : install::manifest_dirs(e)) std::printf("  %s\n", d.c_str());
    return 1;
  }
  std::printf("%-18s %-34s %-6s %s\n", "ID", "NAME", "YEAR", "STATUS");
  for (const std::string& id : ids) {
    fs::path mp = install::find_manifest(e, id);
    std::string status;
    std::string name = id;
    uint32_t year = 0;
    try {
      Meta m = install::load_manifest(mp);
      name = m.name;
      year = m.year;
      std::error_code ec;
      if (fs::exists(games_dir() / (id + ".kgpack"), ec)) {
        status = "installed (" + human(fs::file_size(games_dir() / (id + ".kgpack"), ec)) + ")";
      } else {
        // A disc reference carries a #LABEL or #N suffix naming which disc of
        // the archive it means; readiness is about the archive.
        std::string missing;
        for (const std::string& d : m.recipe.discs) {
          install::DiscRef r = install::parse_disc_ref(d);
          if (install::find_iso(r.archive).empty()) { missing = r.archive; break; }
        }
        // Not "blocked": the wizard takes the files from you, from wherever
        // they are, so a disc that is not in iso_dir() is a thing this listing
        // cannot see rather than a thing that cannot be installed.
        status = missing.empty() ? "ready to install" : "needs " + missing;
      }
    } catch (const std::exception& ex) {
      status = std::string("bad manifest: ") + ex.what();
    }
    std::printf("%-18s %-34s %-6u %s\n", id.c_str(), name.c_str(), year, status.c_str());
  }
  return 0;
}

int cmd_identify(const rt::Env& e, const fs::path& p) {
  std::error_code ec;
  if (!fs::exists(p, ec)) {
    std::fprintf(stderr, "kretro: no such file: %s\n", p.c_str());
    return 1;
  }
  iso::Info i = iso::scan(p, /*full=*/false);
  std::printf("file          %s\n", p.filename().c_str());
  std::printf("size          %s\n", human(i.size).c_str());
  std::printf("iso 9660      %s\n", i.valid_iso9660 ? "yes" : "no - this may not be a disc image");
  if (i.valid_iso9660) {
    std::printf("volume        %s\n", i.volume_id.c_str());
    if (!i.publisher.empty()) std::printf("publisher     %s\n", i.publisher.c_str());
    if (!i.created.empty()) std::printf("created       %s\n", i.created.c_str());
  }
  std::printf("fingerprint   %s (first %s)\n", to_hex(i.prefix).substr(0, 32).c_str(),
              human(iso::kPrefixBytes).c_str());

  std::vector<iso::Known> db = iso::load_database(e);
  iso::Match match = iso::identify(db, i);
  if (match.exact) {
    std::printf("known as      %s\n", match.entry->name.c_str());
  } else if (match.suspect_bad_dump) {
    std::printf("known as      %s\n", match.entry->name.c_str());
    std::printf("\nWARNING: this disc has the right name and size but different contents.\n"
                "         That is what a bad dump or a failing drive looks like. Installing\n"
                "         from it may put a corrupt file somewhere you will not meet for\n"
                "         another forty hours.\n");
  } else if (!db.empty()) {
    std::printf("known as      not in the database of %zu discs\n", db.size());
  }

  // Which manifests name this disc, so the answer is "you can install X from
  // this" rather than a hash nobody can use.
  std::vector<std::string> can;
  for (const std::string& id : install::known_games(e)) {
    try {
      Meta m = install::load_manifest(install::find_manifest(e, id));
      for (const std::string& d : m.recipe.discs) {
        std::string a = d, b = p.filename().string();
        for (char& c : a) c = static_cast<char>(tolower(c));
        for (char& c : b) c = static_cast<char>(tolower(c));
        if (a == b) { can.push_back(m.name + " (" + id + ")"); break; }
      }
    } catch (const std::exception&) {}
  }
  std::printf("\n");
  if (can.empty()) {
    std::printf("No manifest names this disc.\n");
  } else {
    std::printf("This disc can install:\n");
    for (const std::string& c : can) std::printf("  %s\n", c.c_str());
  }
  return 0;
}

// `kretro install <id>` is a way into the wizard now rather than a way around
// it. The manifest is looked up here, on the terminal where the id was typed,
// so a typo is answered by the line under it rather than by a window that opens
// and then complains.
//
// Except where there is nobody to be in the room. A copy or unzip install asks
// no questions: the manifest holds every answer an installer would have wanted,
// and install::run - the same engine import_pack rebuilds a recipe with - can
// produce the pack with no display at all. So the wizard is what a person gets,
// and --headless is what a script gets; a machine with no Wayland or X11
// session takes the headless road without being asked, because the alternative
// there is a window that cannot open.
//
// wine_setup and installer_exe refuse. Clicking through somebody's 1998 setup -
// its directory, its questions, its serial - is the whole point of the wizard,
// and a script claiming to do it would be lying about what happened.
int cmd_install(const rt::Env& e, const std::vector<std::string>& a) {
  std::string id;
  bool headless = false;
  install::Options opt;
  for (const std::string& s : a) {
    if (s == "--headless") {
      headless = true;
    } else if (s == "--force") {
      opt.force = true;
    } else if (s == "--keep-tree") {
      opt.keep_tree = true;
    } else if (s == "--no-discs") {
      opt.embed_discs = false;
    } else if (!s.empty() && s[0] == '-') {
      std::fprintf(stderr, "kretro: unknown option %s\n", s.c_str());
      return 2;
    } else {
      id = s;
    }
  }
  if (id.empty()) return usage();
  fs::path toml = install::find_manifest(e, id);
  if (toml.empty()) {
    std::fprintf(stderr, "kretro: no manifest for '%s'. Try: kretro games\n", id.c_str());
    return 1;
  }

  const bool screen = std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY");
  if (!headless && screen) {
    gui::Startup entry;
    entry.create = true;
    entry.preset = id;
    return gui::run(e, entry);
  }

  Meta m = install::load_manifest(toml);
  const std::string& method = m.recipe.method;
  if (method != "copy" && method != "unzip") {
    std::fprintf(stderr,
                 "kretro: %s installs by running its own installer, and that needs a person.\n"
                 "  Its method is %s: somebody has to answer %s under Wine - where to put\n"
                 "  the game, which pieces to install, the serial off the box. Nothing here\n"
                 "  can do that for you, so this refuses rather than pretend.\n"
                 "  Run it on a desktop and the wizard will walk you through: kretro install %s\n",
                 m.name.c_str(), method.c_str(),
                 m.recipe.setup.empty() ? "the installer" : m.recipe.setup.c_str(), id.c_str());
    return 1;
  }
  if (!headless) {
    say("no display, and " + m.name + " needs nobody at an installer - installing here");
  }

  std::fprintf(stderr, "%s\n", m.name.c_str());
  install::Result r = install::run(e, m, opt, say);
  std::printf("\n%s\n", r.pack.c_str());
  std::printf("  %s in %zu files, packed to %s\n", human(r.tree_bytes).c_str(), r.entries,
              human(r.pack_bytes).c_str());
  std::printf("  root %s\n", to_hex(r.root).c_str());
  if (!opt.embed_discs) {
    std::printf("  without its disc: playing needs the original in %s\n",
                install::iso_dir().string().c_str());
  }
  std::printf("  kretro play %s\n", m.id.c_str());
  return 0;
}

int cmd_list() {
  ensure_state_dirs();
  std::error_code ec;
  int n = 0;
  std::vector<fs::path> packs;
  for (const fs::directory_entry& de : fs::directory_iterator(games_dir(), ec)) {
    if (de.path().extension() == ".kgpack") packs.push_back(de.path());
  }
  std::sort(packs.begin(), packs.end());
  if (packs.empty()) {
    std::printf("No games yet.\n\n  kretro games            what you can install\n"
                "  kretro install <id>     install one\n");
    return 0;
  }
  std::printf("%-18s %-32s %-6s %-10s %s\n", "ID", "NAME", "YEAR", "SIZE", "ROOT");
  for (const fs::path& p : packs) {
    try {
      Pack pk = Pack::open(p);
      std::printf("%-18s %-32s %-6u %-10s %s\n", pk.meta().id.c_str(), pk.meta().name.c_str(),
                  pk.meta().year, human(fs::file_size(p, ec)).c_str(),
                  to_hex(pk.header().blake3_root).substr(0, 12).c_str());
      ++n;
    } catch (const std::exception& ex) {
      std::printf("%-18s %s\n", p.filename().c_str(), ex.what());
    }
  }
  return n ? 0 : 1;
}

int cmd_verify(const std::string& id) {
  fs::path p = games_dir() / (id + ".kgpack");
  std::error_code ec;
  if (!fs::exists(p, ec)) {
    std::fprintf(stderr, "kretro: %s is not installed\n", id.c_str());
    return 1;
  }
  Pack pk = Pack::open(p);
  Pack::Verification v = pk.verify();
  std::printf("%-14s %s\n", "tree root", v.root_matches ? "ok" : "MISMATCH");
  std::printf("%-14s %s\n", "body", v.body_matches ? "ok" : "CORRUPT");
  std::printf("%-14s %zu files, %s\n", "contents", pk.meta().tree.size(),
              human(pk.meta().tree.total_bytes()).c_str());
  if (!v.ok) std::fprintf(stderr, "\n%s\n", v.detail.c_str());
  return v.ok ? 0 : 1;
}

int cmd_uninstall(const std::string& id) {
  fs::path p = games_dir() / (id + ".kgpack");
  std::error_code ec;
  if (!fs::exists(p, ec)) {
    std::fprintf(stderr, "kretro: %s is not installed\n", id.c_str());
    return 1;
  }
  // The game is one file, so this really is all of it. Saves are deliberately
  // somewhere else and are left alone.
  fs::remove(p, ec);
  std::printf("removed %s\n", p.c_str());
  fs::path saves = saves_dir() / id;
  if (fs::exists(saves, ec)) std::printf("saves kept at %s\n", saves.c_str());
  return 0;
}


static int cmd_scan(const rt::Env& e, const std::vector<std::string>& args) {
  fs::path dir = "iso";
  bool write_db = false;
  for (const std::string& a : args) {
    if (a == "--write-db") write_db = true;
    else dir = a;
  }
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    std::fprintf(stderr, "not a directory: %s\n", dir.string().c_str());
    return 1;
  }

  std::vector<fs::path> files;
  for (const auto& de : fs::directory_iterator(dir, ec)) {
    if (de.is_regular_file(ec)) files.push_back(de.path());
  }
  std::sort(files.begin(), files.end());

  fs::path work = fs::temp_directory_path() / "kretro-scan";
  fs::remove_all(work, ec);

  std::vector<iso::Known> rows;
  for (const fs::path& f : files) {
    std::vector<disc::Candidate> cands;
    try {
      cands = disc::probe(e, f);
    } catch (const std::exception& ex) {
      std::printf("%-56s  unreadable: %s\n", f.filename().string().c_str(), ex.what());
      continue;
    }
    if (cands.empty()) {
      std::printf("%-56s  no disc image inside\n", f.filename().string().c_str());
      continue;
    }
    std::printf("%s\n", f.filename().string().c_str());
    std::fflush(stdout);

    std::vector<disc::Disc> opened;
    int n = 0;
    for (const disc::Candidate& c : cands) {
      ++n;
      fs::path w = work / (f.stem().string() + "-" + std::to_string(n));
      try {
        opened.push_back(disc::open(e, c, w, false));
      } catch (const std::exception& ex) {
        std::printf("    %-30s failed: %s\n", c.name.c_str(), ex.what());
      }
      fs::remove_all(w, ec);
    }

    // Three rips of one disc in one archive are one disc and two alternates,
    // and the database must not claim otherwise.
    disc::DiscSet set = disc::assemble(disc::set_id_from(f.stem().string()),
                                       f.stem().string(), std::move(opened));
    for (size_t i = 0; i < set.discs.size(); ++i) {
      const disc::Disc& d = set.discs[i];
      std::printf("    disc %zu  %-24s %12llu bytes\n", i + 1, d.label.c_str(),
                  static_cast<unsigned long long>(d.info.size));
      iso::Known k;
      k.prefix = d.info.prefix;
      k.size = d.info.size;
      k.volume_id = d.label;
      k.set_id = set.set_id;
      k.disc_no = static_cast<int>(i + 1);
      k.name = set.name;
      rows.push_back(k);
    }
    for (const disc::Disc& d : set.alternates) {
      std::printf("    alternate dump of %s\n", d.label.c_str());
    }
    for (const disc::Companion& comp : disc::scan_companions(f)) {
      std::printf("    also: %s (%s)\n", comp.path.filename().string().c_str(),
                  comp.kind.c_str());
    }
    std::fflush(stdout);
  }
  fs::remove_all(work, ec);

  if (write_db) {
    // db/ is local, gitignored data, so a fresh checkout has no directory to
    // write into until this makes one.
    fs::path out = "db/discs.txt";
    fs::create_directories(out.parent_path(), ec);
    std::ofstream o(out, std::ios::trunc);
    o << "# Known discs. Offline; there is no lookup service and never will be.\n"
      << "#\n"
      << "# Naming a release before installing catches the wrong disc, and the\n"
      << "# prefix hash catches a bad dump before a forty-hour playthrough finds\n"
      << "# the corrupt file. The prefix is BLAKE3 of the first 64 MiB of the\n"
      << "# normalised data track, truncated to sixteen bytes, so a .bin and its\n"
      << "# .iso conversion agree.\n"
      << "#\n"
      << "# prefix_blake3\tsize\tvolume_id\tset_id\tdisc_no\tname\n";
    for (const iso::Known& k : rows) {
      o << to_hex(k.prefix).substr(0, 32) << "\t" << k.size << "\t" << k.volume_id << "\t"
        << k.set_id << "\t" << k.disc_no << "\t" << k.name << "\n";
    }
    std::printf("\nwrote %s (%zu discs)\n", out.string().c_str(), rows.size());
  }
  return 0;
}


// What is this screen? Asked once, here, because this file already links SDL
// for the shelf and the session layer deliberately does not.
void panel_size(uint32_t* w, uint32_t* h) {
  *w = 0;
  *h = 0;
  bool inited = SDL_WasInit(SDL_INIT_VIDEO) != 0;
  if (!inited && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return;
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    *w = static_cast<uint32_t>(dm.w);
    *h = static_cast<uint32_t>(dm.h);
  }
  if (!inited) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int cmd_display(const std::string& id) {
  uint32_t pw = 0, ph = 0;
  panel_size(&pw, &ph);
  fs::path pack = games_dir() / (id + ".kgpack");
  std::error_code ec;
  if (!fs::exists(pack, ec)) {
    std::fprintf(stderr, "kretro: %s is not installed\n", id.c_str());
    return 1;
  }
  Meta m = Pack::open(pack).meta();
  config::Config cfg = config::load(config::config_file());
  config::Display d = config::for_game(cfg, id);
  config::Geometry g = config::compute_geometry(m.run.width, m.run.height, pw, ph, d);

  std::printf("%s\n", m.name.c_str());
  std::printf("  the game renders at   %ux%u\n", g.logical_w, g.logical_h);
  if (pw && ph) std::printf("  this screen is        %ux%u\n", pw, ph);
  else std::printf("  this screen is        unknown (no display)\n");
  std::printf("  scaling               %s", config::name_of(d.mode));
  if (d.mode == config::ScaleMode::Integer && d.scale) std::printf(", asked for %ux", d.scale);
  std::printf("\n");
  std::printf("  you will see          %ux%u at %ux%s\n", g.logical_w * g.scale,
              g.logical_h * g.scale, g.scale, g.fullscreen ? ", fullscreen" : "");
  return 0;
}

// ---- kretro bundle ----------------------------------------------------------
//
// Building players without a window. The Bundles page is how an author builds
// one; this is the same build - bundle::build_from_draft, the call the page's
// button makes - for scripts, for tests, and for rebuilding a bundle the page
// remembers from a terminal.

std::string env_or_empty(const char* k) {
  const char* v = std::getenv(k);
  return v ? v : "";
}

bundle::BuildInputs bundle_inputs(const fs::path& base) {
  return bundle::BuildInputs{env_or_empty("KRETRO_SELF"), games_dir(), install::keys_file(),
                             env_or_empty("KRETRO_DWARFS"), cache_dir(), base};
}

// What the page knows about each game of a draft: its pack, and the vault's
// key for it. The executables are read too when `imports` is set, which is
// what the Glide checks need; it takes a dwarfsextract per game.
std::vector<bundle::PackFacts> draft_facts(const bundle::Draft& d, std::vector<bundle::GameFacts>& out,
                                           bool imports) {
  std::vector<bundle::PackFacts> facts(d.games.size());
  std::vector<install::StoredKey> keys = install::load_keys(install::keys_file());
  out.assign(d.games.size(), {});
  for (size_t i = 0; i < d.games.size(); ++i) {
    std::error_code ec;
    fs::path pk = games_dir() / (d.games[i].id + ".kgpack");
    if (!fs::exists(pk, ec)) continue;
    try {
      facts[i] = bundle::read_pack_facts(pk);
    } catch (const std::exception&) {
      continue;
    }
    out[i].pack = &facts[i];
    out[i].vault_key = install::key_for(keys, d.games[i].id);
    if (imports) {
      out[i].imports = bundle::read_exe_imports(facts[i], env_or_empty("KRETRO_DWARFS"),
                                                cache_dir() / "bundle-exe");
    }
  }
  return facts;
}

int build_and_say(const bundle::Draft& d, const bundle::BuildInputs& in, bundle::Built* built = nullptr) {
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
  std::printf("%s\n  %s, %zu game%s, checked end to end\n", b.path.c_str(), human(b.size).c_str(),
              d.games.size(), d.games.size() == 1 ? "" : "s");
  if (built) *built = b;
  return 0;
}

// kretro bundle build -o FILE|DIR [--id ID] [--title T] [--version V]
//                     [--base PLAYER-BASE] --rights GAME...
int cmd_bundle_build(const std::vector<std::string>& a) {
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
    fs::path pk = games_dir() / (id + ".kgpack");
    std::error_code ec;
    if (!fs::exists(pk, ec)) throw std::runtime_error(id + " is not installed");
    fs::path title = game_saves_dir(id) / "journal" / "title.png";
    bool cover = fs::exists(title, ec) && !fs::exists(game_saves_dir(id) / "journal" / "title.provisional", ec);
    d.games.push_back(bundle::game_from_pack(bundle::read_pack_facts(pk), cover ? title : fs::path()));
  }
  if (d.title.empty()) d.title = d.games.size() == 1 ? d.games[0].name : (d.id.empty() ? "kretro bundle" : d.id);
  if (d.id.empty()) d.id = d.games.size() == 1 ? d.games[0].id : bundle::id_from_title(d.title);

  // -o names the file, or a folder to put <id>-<version>.run in.
  std::error_code ec;
  bundle::BuildInputs in = bundle_inputs(base);
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

  std::vector<bundle::GameFacts> gf;
  std::vector<bundle::PackFacts> facts = draft_facts(d, gf, false);
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
  int rc = build_and_say(d, in, &b);
  if (rc != 0) return rc;
  // Remembered, as a build from the page is, so `bundle list` shows it and
  // `bundle rebuild <id>` makes the next one - and the Bundles page opens it.
  char when[32] = {};
  std::time_t now = std::time(nullptr);
  std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&now));
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

int cmd_bundle_list() {
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
                  human(d.last_size).c_str());
    }
  }
  for (const std::string& u : unreadable) std::fprintf(stderr, "  cannot read %s\n", u.c_str());
  return 0;
}

// kretro bundle rebuild <id> [--base PLAYER-BASE]: what the page's Build button
// does for a remembered bundle, held to the same conditions.
int cmd_bundle_rebuild(const std::vector<std::string>& a) {
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
  std::vector<bundle::GameFacts> gf;
  std::vector<bundle::PackFacts> facts = draft_facts(d, gf, true);
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
  int rc = build_and_say(d, bundle_inputs(base));
  if (rc == 0) {
    char when[32] = {};
    std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&now));
    fs::path p = fs::path(d.out_dir) / bundle::output_name(d);
    std::error_code ec;
    bundle::stamp_built(bundle::bundles_dir(), d.id, p.string(), fs::file_size(p, ec), when);
  }
  return rc;
}

int cmd_bundle(const std::vector<std::string>& a) {
  const std::string sub = a.empty() ? "" : a[0];
  std::vector<std::string> rest(a.begin() + (a.empty() ? 0 : 1), a.end());
  if (sub == "build") return cmd_bundle_build(rest);
  if (sub == "list") return cmd_bundle_list();
  if (sub == "rebuild") return cmd_bundle_rebuild(rest);
  std::fprintf(stderr,
               "kretro bundle build -o FILE|DIR [--id ID] [--title T] [--version V] [--base FILE] --rights GAME...\n"
               "kretro bundle list\n"
               "kretro bundle rebuild <id> [--base FILE]\n");
  return 2;
}

int cmd_key(const std::vector<std::string>& a) {
  auto keys = install::load_keys(install::keys_file());
  if (a.size() == 1) {
    std::string v = install::key_for(keys, a[0]);
    if (v.empty()) {
      std::printf("no serial stored for %s\n", a[0].c_str());
      return 1;
    }
    std::printf("%s\n", v.c_str());
    return 0;
  }
  std::string note = a.size() > 2 ? a[2] : "";
  install::put_key(keys, a[0], a[1], note);
  install::save_keys(install::keys_file(), keys);
  std::printf("stored a serial for %s in %s\n", a[0].c_str(),
              install::keys_file().string().c_str());
  return 0;
}


// Short form: `kretro swap <id> <n>` while an install is running. An installer
// that insists on one drive gets the disc it asked for, and because a Wine
// drive is a symlink resolved on every open, it sees the change immediately.
int cmd_swap_game(const std::string& id, int n) {
  fs::path work = cache_dir() / ("install-" + id);
  fs::path prefix = work / "prefix";
  std::error_code ec;
  if (!fs::exists(prefix, ec)) {
    std::fprintf(stderr, "kretro: no install of %s is running\n", id.c_str());
    return 1;
  }
  if (n < 1 || n > 8) { std::fprintf(stderr, "kretro: disc %d?\n", n); return 1; }
  fs::path tree = work / (std::string("drive-") + static_cast<char>('d' + n - 1));
  if (!fs::exists(tree, ec)) {
    std::fprintf(stderr, "kretro: %s has no disc %d mounted\n", id.c_str(), n);
    return 1;
  }
  std::string label;
  { std::ifstream f(tree / ".windows-label"); std::getline(f, label); }
  disc::repoint(prefix, 'd', tree);
  std::printf("D: is now disc %d (%s). The installer should carry on.\n", n, label.c_str());
  return 0;
}

int cmd_contents(const rt::Env& e, const std::string& ref_s) {
  install::DiscRef ref = install::parse_disc_ref(ref_s);
  fs::path file = install::find_iso(ref.archive);
  if (file.empty()) {
    std::fprintf(stderr, "no such file in the collection: %s\n", ref.archive.c_str());
    return 1;
  }
  std::error_code ec;
  fs::path work = fs::temp_directory_path() / "kretro-contents";
  fs::remove_all(work, ec);

  // Opened one at a time and stopped as soon as the reference matches: an
  // archive of five discs should not cost five normalisations to list one.
  std::vector<disc::Candidate> cands = disc::probe(e, file);
  std::vector<disc::Disc> opened;
  std::vector<std::string> seen;
  bool found = false;
  for (size_t i = 0; i < cands.size() && !found; ++i) {
    disc::Disc one = disc::open(e, cands[i], work / std::to_string(i), false);
    seen.push_back(one.label);
    std::vector<disc::Disc> probe_set;
    probe_set.push_back(one);
    install::DiscRef by_first;
    by_first.label = ref.label;
    by_first.index = ref.index > 0 ? 1 : 0;
    bool match = ref.label.empty() ? (ref.index == 0 || static_cast<size_t>(ref.index) == i + 1)
                                   : install::pick_disc(probe_set, by_first) == 0;
    if (match) { opened.push_back(std::move(one)); found = true; }
  }
  if (!found) {
    std::fprintf(stderr, "no disc matching %s. It holds:\n", ref_s.c_str());
    for (const std::string& l : seen) std::fprintf(stderr, "    %s\n", l.c_str());
    fs::remove_all(work, ec);
    return 1;
  }

  const disc::Disc& d = opened[0];
  std::printf("%s  %s  %llu bytes\n", file.filename().string().c_str(), d.label.c_str(),
              static_cast<unsigned long long>(d.info.size));
  for (const std::string& p2 : iso::list(e, d.iso)) std::printf("  %s\n", p2.c_str());
  fs::remove_all(work, ec);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> a(argv + 1, argv + argc);

  // No arguments means the shelf. Running the binary is the whole interface;
  // the subcommands are there for when you would rather type.
  if (a.empty()) {
    try {
      gpu::Report gl = gpu::probe();
      gpu::materialize(gl);
      rt::Env e = rt::make(&gl);
      return gui::run(e);
    } catch (const std::exception& ex) {
      std::fprintf(stderr, "kretro: %s\n", ex.what());
      return 1;
    }
  }
  std::string cmd = a[0];
  a.erase(a.begin());

  try {
    // The GPU probe is only needed by things that draw; extraction and packing
    // do not, and it costs a directory walk.
    bool needs_gpu = (cmd == "wine" || cmd == "exec" || cmd == "play" || cmd == "install" ||
                      cmd == "create");
    gpu::Report gl;
    if (needs_gpu) {
      gl = gpu::probe();
      gpu::materialize(gl);
    }
    rt::Env e = rt::make(needs_gpu ? &gl : nullptr);

    if (cmd == "info") return cmd_info(e);
    if (cmd == "create" && a.empty()) {
      gui::Startup entry;
      entry.create = true;
      return gui::run(e, entry);
    }
    if (cmd == "doctor") return cmd_doctor(a);
    if (cmd == "games") return cmd_games(e);
    if (cmd == "list") return cmd_list();
    if (cmd == "identify" && a.size() == 1) return cmd_identify(e, a[0]);
    if (cmd == "scan") return cmd_scan(e, a);
    if (cmd == "contents" && a.size() == 1) return cmd_contents(e, a[0]);
    if (cmd == "key" && !a.empty() && a.size() <= 3) return cmd_key(a);
    if (cmd == "display" && a.size() == 1) return cmd_display(a[0]);
    if (cmd == "stage-probe") {
      return gui::stage_probe(e, a.empty() ? 20 : std::atoi(a[0].c_str()));
    }
    if (cmd == "swap" && a.size() == 2) return cmd_swap_game(a[0], std::atoi(a[1].c_str()));
    if (cmd == "install") return cmd_install(e, a);
    if (cmd == "verify" && a.size() == 1) return cmd_verify(a[0]);
    if (cmd == "bundle") return cmd_bundle(a);
    // The name it had while it was a hidden test hook; scripts still use it.
    if (cmd == "bundle-build") return cmd_bundle_build(a);
    if (cmd == "uninstall" && a.size() == 1) return cmd_uninstall(a[0]);

    if (cmd == "play") {
      session::Options so;
      panel_size(&so.panel_w, &so.panel_h);
      std::string id;
      for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == "--fullscreen") so.fullscreen = true;
        else if (a[i] == "--windowed") so.fullscreen = false;
        else if (a[i] == "--dgvoodoo") so.dgvoodoo = true;
        else if (a[i] == "--no-journal") so.no_journal = true;
        // Everything up to the game - lock, mounts, prefix, backend - and then
        // the plan instead of the game: how a machine with no display, or a
        // test, finds out whether this game would get its writable layer.
        else if (a[i] == "--dry-run") so.dry_run = true;
        else if (a[i] == "--note" && i + 1 < a.size()) so.note = a[++i];
        // These override the setting for this game for one session, without
        // writing anything: useful for finding out what you like.
        else if (a[i] == "--integer") { so.display.mode = config::ScaleMode::Integer; so.display_set = true; }
        else if (a[i] == "--fit") { so.display.mode = config::ScaleMode::Fit; so.display_set = true; }
        else if (a[i] == "--native") { so.display.mode = config::ScaleMode::Native; so.display_set = true; }
        else if (a[i] == "--scale" && i + 1 < a.size()) {
          so.display.scale = static_cast<uint32_t>(std::atoi(a[++i].c_str()));
          so.display_set = true;
        }
        else if (!a[i].empty() && a[i][0] == '-') { std::fprintf(stderr, "kretro: unknown option %s\n", a[i].c_str()); return 2; }
        else id = a[i];
      }
      if (id.empty()) return usage();
      session::Outcome o = session::play(e, id, so);
      if (so.dry_run) {
        std::printf("%s is ready to play. Not started (--dry-run):\n", id.c_str());
        size_t w = 0;
        for (const auto& [k, v] : o.plan) w = std::max(w, k.size());
        for (const auto& [k, v] : o.plan) std::printf("  %-*s  %s\n", static_cast<int>(w), k.c_str(), v.c_str());
        return 0;
      }
      std::printf("\nplayed for %s\n", human_time(o.seconds).c_str());
      size_t wrote = o.diff.added.size() + o.diff.changed.size();
      if (wrote) std::printf("the game wrote %zu file%s\n", wrote, wrote == 1 ? "" : "s");
      if (!o.generation.empty()) std::printf("snapshot %s\n", o.generation.filename().c_str());
      return o.status;
    }

    if (cmd == "export" && !a.empty()) {
      std::string id = a[0];
      bool recipe = true;
      fs::path out;
      for (size_t i = 1; i < a.size(); ++i) {
        if (a[i] == "--recipe") recipe = true;
        else if (a[i] == "--capsule") recipe = false;
        else if (a[i] == "--standalone") {
          // Gone: it copied all of kretro, authoring tools and all, to carry
          // one game. A one-game player is the same one file for somebody
          // else to run, at half the size, and it is what gets shipped now.
          std::fprintf(stderr,
                       "kretro: export --standalone is gone. A player is what you send now:\n"
                       "  kretro bundle build -o %s.run --id %s --rights %s\n"
                       "or the Bundles page on the shelf.\n",
                       id.c_str(), id.c_str(), id.c_str());
          return 2;
        }
        else if (a[i] == "-o" && i + 1 < a.size()) out = a[++i];
      }
      if (recipe) {
        fs::path f = install::export_recipe(id, out);
        std::printf("%s\n", f.c_str());
        std::printf("  %s - the recipe, not the game. Anyone with the same disc rebuilds it\n",
                    human(fs::file_size(f)).c_str());
        std::printf("  by clicking through the installer themselves, and the tree they get is\n");
        std::printf("  compared with this one and the difference reported.\n");
      } else {
        fs::path src = games_dir() / (id + ".kgpack");
        fs::path dst = out.empty() ? fs::path(id + ".kgpack") : out;
        std::error_code ec;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) { std::fprintf(stderr, "kretro: %s\n", ec.message().c_str()); return 1; }
        std::printf("%s\n  %s - the whole game in one file\n", dst.c_str(),
                    human(fs::file_size(dst)).c_str());
      }
      return 0;
    }

    if (cmd == "export-saves" && !a.empty()) {
      fs::path f = install::export_saves(a[0], a.size() > 1 ? fs::path(a[1]) : fs::path(), e);
      std::printf("%s\n  %s - just what the game wrote\n", f.c_str(),
                  human(fs::file_size(f)).c_str());
      return 0;
    }

    if (cmd == "import-saves" && a.size() == 2) {
      install::import_saves(a[0], a[1]);
      std::printf("restored what %s wrote, from %s\n", a[0].c_str(), a[1].c_str());
      std::printf("  what was there before is kept as a snapshot: kretro saves %s\n", a[0].c_str());
      return 0;
    }

    if (cmd == "import" && a.size() == 1) {
      install::ImportResult r = install::import_pack(e, a[0], /*replace=*/false, say);
      std::printf("\n%s\n", r.name.c_str());
      if (r.rebuilt) {
        std::printf("  rebuilt from your own disc, and %s\n",
                    r.root_matched ? "it matches the recipe exactly" : "NOT verified");
      } else {
        std::printf("  installed from the capsule\n");
      }
      std::printf("  %s\n", r.pack.c_str());
      return 0;
    }

    if (cmd == "show" && a.size() == 1) {
      gpu::Report gl2 = gpu::probe();
      gpu::materialize(gl2);
      gui::Startup entry;
      entry.game = a[0];
      return gui::run(rt::make(&gl2), entry);
    }
    if (cmd == "input") {
      // Started by a session beside a game; not something to run by hand.
      std::string display, game;
      pid_t pid = 0;
      bool pause = true;
      for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == "--display" && i + 1 < a.size()) display = a[++i];
        else if (a[i] == "--pid" && i + 1 < a.size()) pid = std::atoi(a[++i].c_str());
        else if (a[i] == "--game" && i + 1 < a.size()) game = a[++i];
        else if (a[i] == "--no-pause") pause = false;
      }
      if (display.empty() || pid <= 0) return usage();
      // The game's own bindings, when the session named one.
      std::map<std::string, std::string> binds;
      if (!game.empty()) {
        std::error_code ec;
        fs::path pk = games_dir() / (game + ".kgpack");
        if (fs::exists(pk, ec)) {
          try { binds = Pack::open(pk).meta().input; } catch (const std::exception&) {}
        }
      }
      return gui::run_input(display, pid, pause, binds);
    }

    if (cmd == "compare" && !a.empty()) {
      // Which renderer looks correct is a question about pixels, and today it
      // is answered by trial and error over an afternoon. We own the
      // compositor, so we can just take the pictures.
      std::string id = a[0];
      int secs = 25;
      for (size_t i = 1; i + 1 < a.size(); ++i) {
        if (a[i] == "--seconds") secs = std::atoi(a[++i].c_str());
      }
      fs::path dir = state_dir() / "compare" / id;
      std::error_code ec;
      fs::remove_all(dir, ec);
      fs::create_directories(dir, ec);

      const char* backends[] = {"gl", "vulkan", "gdi"};
      std::printf("Running %s under each renderer for %ds and keeping a frame.\n\n", id.c_str(), secs);
      for (const char* b : backends) {
        session::Options so;
        so.renderer = b;
        so.stop_after = secs;
        so.no_journal = false;
        so.capture_to = dir / (std::string(b) + ".png");
        std::fprintf(stderr, "-- %s --\n", b);
        try {
          session::play(e, id, so);
        } catch (const std::exception& ex) {
          std::fprintf(stderr, "   %s\n", ex.what());
        }
      }
      std::printf("\nLook at these side by side and keep the one that is right:\n");
      for (const char* b : backends) {
        fs::path f = dir / (std::string(b) + ".png");
        if (fs::exists(f, ec)) std::printf("  %-7s %s\n", b, f.c_str());
        else std::printf("  %-7s (no frame captured)\n", b);
      }
      return 0;
    }

    if (cmd == "journal" && a.size() == 1) return cmd_journal(a[0]);
    if (cmd == "saves" && a.size() == 1) return cmd_saves(a[0]);
    if (cmd == "restore" && a.size() == 2) {
      session::restore(a[0], a[1]);
      std::printf("restored %s to %s (the previous state was snapshotted first)\n", a[0].c_str(), a[1].c_str());
      return 0;
    }

    if (cmd == "wine") {
      fs::path w = rt::find_wine(e.root);
      if (w.empty()) { std::fprintf(stderr, "kretro: no wine in this runtime\n"); return 1; }
      rt::exec(e, w, a);
    }
    if (cmd == "exec" && !a.empty()) {
      fs::path prog = a[0];
      if (prog.is_relative()) {
        fs::path found = rt::which(e, a[0]);
        if (found.empty()) { std::fprintf(stderr, "kretro: no %s in this runtime\n", a[0].c_str()); return 1; }
        prog = found;
      }
      a.erase(a.begin());
      rt::exec(e, prog, a);
    }
    return usage();
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "kretro: %s\n", ex.what());
    return 1;
  }
}
