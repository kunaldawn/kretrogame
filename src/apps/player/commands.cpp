#include "commands.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "../../gpu/probe.h"
#include "../../gui/gamepad_bridge.h"
#include "../../gui/screen.h"
#include "../../player/doctor.h"
#include "../../session/input_helper.h"
#include "../../util/format.h"
#include "../../util/paths.h"

namespace fs = std::filesystem;

namespace kg::player::app {

namespace {

void print_plan(const session::Outcome& o) {
  size_t w = 0;
  for (const auto& [k, v] : o.plan) w = std::max(w, k.size());
  for (const auto& [k, v] : o.plan) {
    std::printf("  %s%s  %s\n", k.c_str(), std::string(w - k.size(), ' ').c_str(), v.c_str());
  }
}

bool ask_yes(const std::string& question) {
  if (!isatty(STDIN_FILENO)) return false;
  std::fprintf(stderr, "%s [y/N] ", question.c_str());
  std::string line;
  if (!std::getline(std::cin, line)) return false;
  return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

}  // namespace

// Started by a session beside a game, as kretro's own is: the gamepad as
// keys and mouse, and pause when the window loses focus. The bindings are the
// pack's, then the author's map over them when this game's controls say so.
int input_helper(const Bundle& b, const std::vector<std::string>& args) {
  const session::InputHelperArgs ia = session::parse_input_helper(args);
  if (!ia.valid()) return 2;
  const std::string& game = ia.game;
  std::map<std::string, std::string> binds;
  if (const bundle::GameMeta* g = b.game(game)) {
    std::map<std::string, std::string> own;
    try {
      const bundle::Entry* e = b.pack(game);
      own = Pack::open(b.self, b.toc.at(*e), e->len).game(game).input;
    } catch (const std::exception&) {
    }
    player::GameSettings s = player::load_game_settings(state_dir() / game / "settings.toml",
                                                        player::default_settings(g->display, g->fullscreen));
    binds = player::gamepad_bindings(own, *g, s);
  }
  return gui::run_input(ia.display, ia.pid, ia.pause, binds);
}

int play(const Player& p, const Command& c) {
  const std::string& id = c.game;
  const bundle::GameMeta& g = p.game(id);

  // The silent check. A blocking problem stops a real launch here, with its
  // line; a dry run is how a machine with no display is tested, so it reports
  // the problem and carries on to the plan.
  const player::doctor::Report rep = p.doctor_report();
  for (const gpu::Problem& pr : rep.problems) {
    if (!pr.blocking()) continue;
    std::fprintf(stderr, "%s: %s\n", p.bundle().self.filename().c_str(), pr.line().c_str());
    if (!c.dry_run) return 1;
  }
  // The warnings, once each on this machine, as the launcher says them: no
  // sound server, an NVIDIA driver that does not match its kernel module and
  // so software rendering. Otherwise a game started from a terminal is slow
  // or silent with nothing to say why.
  {
    player::LauncherState ls = player::load_launcher(p.launcher_file());
    std::vector<std::string> fresh = player::unseen_warnings(rep, ls);
    for (const std::string& w : fresh) {
      std::fprintf(stderr, "%s: warning: %s\n", p.bundle().self.filename().c_str(), w.c_str());
    }
    if (!fresh.empty()) {
      try {
        player::save_launcher(p.launcher_file(), ls);
      } catch (const std::exception&) {
        // Said again next time, which is all a failed save costs.
      }
    }
  }

  player::PlayOverrides req;
  req.dry_run = c.dry_run;
  req.fullscreen_set = c.fullscreen_set;
  req.fullscreen = c.fullscreen;
  // Only with a display to ask about: a terminal with none gets 0x0.
  const gui::PanelSize ps = gui::desktop_size(true);
  req.panel_w = ps.w;
  req.panel_h = ps.h;
  req.usable_w = ps.usable_w;
  req.usable_h = ps.usable_h;

  for (int attempt = 0; attempt < 2; ++attempt) {
    try {
      if (!p.verified(id)) std::fprintf(stderr, "  checking %s (the first time only)\n", g.name.c_str());
      session::Outcome o = p.play(id, req);
      if (c.dry_run) {
        std::printf("%s is ready to play. Not started (--dry-run):\n", g.name.c_str());
        print_plan(o);
        return 0;
      }
      std::printf("\nplayed for %.0f seconds\n", o.seconds);
      if (!o.generation.empty()) std::printf("snapshot %s\n", o.generation.filename().c_str());
      if (o.status != 0 && o.seconds < 15) {
        std::fprintf(stderr, "\n%s stopped as soon as it started (exit status %d). The end of its log:\n",
                     g.name.c_str(), o.status);
        std::fputs(p.last_log(40).c_str(), stderr);
      }
      return o.status;
    } catch (const session::NeedsUnpack& ex) {
      if (attempt > 0) throw;
      player::UnpackPlan u = p.unpack_plan(id);
      std::fprintf(stderr, "%s\n", ex.what());
      if (!u.fits()) {
        std::fprintf(stderr, "There is not room: %s free. Unpack it somewhere roomier with:\n  %s --extract-to DIR %s\n",
                     fmt::gigabytes(u.free).c_str(), p.bundle().self.filename().c_str(), id.c_str());
        return 1;
      }
      if (!ask_yes("Unpack " + g.name + " there now?")) {
        std::fprintf(stderr, "Not unpacked. %s --extract-to DIR %s does it somewhere of your choosing.\n",
                     p.bundle().self.filename().c_str(), id.c_str());
        return 1;
      }
      p.unpack(id);
    }
  }
  return 1;
}

int doctor(const Player& p, const Command& c) {
  player::doctor::Report rep = p.doctor_report();
  std::string text = player::doctor::render(rep);
  std::fputs(text.c_str(), stdout);
  if (!c.file.empty()) {
    if (!player::doctor::save_redacted(text, c.file)) {
      std::fprintf(stderr, "could not write %s\n", c.file.c_str());
      return 1;
    }
    std::printf("\nsaved to %s, without your home directory or user name\n", c.file.c_str());
  }
  return rep.blocking() ? 1 : 0;
}

int extract(const Player& p, const Command& c) {
  std::string id = c.game;
  if (id.empty()) {
    if (p.bundle().meta.games.size() != 1) {
      std::fprintf(stderr, "This file carries %zu games; say which: --extract-to DIR <game>\n  %s\n",
                   p.bundle().meta.games.size(), p.bundle().game_list().c_str());
      return 2;
    }
    id = p.bundle().meta.games.front().id;
  }
  const bundle::GameMeta& g = p.game(id);
  if (!p.verified(id)) std::fprintf(stderr, "  checking %s first\n", g.name.c_str());
  p.verify(id);
  fs::path where = fs::absolute(c.file) / id;
  std::fprintf(stderr, "  unpacking %s into %s\n", g.name.c_str(), where.c_str());
  fs::path done = p.unpack(id, where);
  std::printf("%s is unpacked in %s\n", g.name.c_str(), done.c_str());
  std::printf("  it is played from there from now on; delete that directory to undo this\n");
  return 0;
}

int licenses(const Player& p) {
  std::fputs(p.licenses_text().c_str(), stdout);
  return 0;
}

int saves_export(const Player& p, const Command& c) {
  fs::path f = p.export_saves(c.game, c.file);
  std::printf("%s - what %s has written\n", f.c_str(), p.game(c.game).name.c_str());
  return 0;
}

int saves_import(const Player& p, const Command& c) {
  p.import_saves(c.game, c.file);
  std::printf("restored what %s wrote, from %s; what was there before is kept as a snapshot\n",
              p.game(c.game).name.c_str(), c.file.c_str());
  return 0;
}

}  // namespace kg::player::app
