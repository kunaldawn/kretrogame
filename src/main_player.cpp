// The player - one file, its games, and nothing else.
//
// By the time main runs the bootstrap has checked the file's table of contents
// and mounted the runtime, and we are under the runtime's own loader. The
// first thing done here is deciding where this player keeps its state, because
// everything after it - the GPU probe's links, the settings, the saves - asks
// for a path, and paths are resolved once.
#include <SDL2/SDL.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gpu/probe.h"
#include "gui/input.h"
#include "gui/launcher.h"
#include "player/cli.h"
#include "player/doctor.h"
#include "player/player.h"
#include "rt/env.h"
#include "util/paths.h"

namespace fs = std::filesystem;
using namespace kg;

namespace {

const char* env(const char* k) {
  const char* v = std::getenv(k);
  return (v && *v) ? v : nullptr;
}

std::string gb(uint64_t n) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.1f GB", static_cast<double>(n) / 1e9);
  return b;
}

// The file the person ran. /proc/self/exe is the runtime's loader by now, so
// the bootstrap hands it down.
fs::path self_path() {
  if (const char* s = env("KRETRO_SELF")) return s;
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec);
}

void panel_size(uint32_t* w, uint32_t* h) {
  *w = *h = 0;
  if (!env("DISPLAY") && !env("WAYLAND_DISPLAY")) return;
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return;
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    *w = static_cast<uint32_t>(dm.w);
    *h = static_cast<uint32_t>(dm.h);
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

// Started by a session beside a game, as kretro's own is: the gamepad as
// keys and mouse, and pause when the window loses focus. The bindings are the
// pack's, then the author's map over them when this game's controls say so.
int cmd_input(const player::Bundle& b, const std::vector<std::string>& a) {
  std::string display, game;
  pid_t pid = 0;
  bool pause = true;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] == "--display" && i + 1 < a.size()) display = a[++i];
    else if (a[i] == "--pid" && i + 1 < a.size()) pid = std::atoi(a[++i].c_str());
    else if (a[i] == "--game" && i + 1 < a.size()) game = a[++i];
    else if (a[i] == "--no-pause") pause = false;
  }
  if (display.empty() || pid <= 0) return 2;
  std::map<std::string, std::string> binds;
  if (const bundle::GameMeta* g = b.game(game)) {
    std::map<std::string, std::string> own;
    try {
      const bundle::Entry* e = b.pack(game);
      own = Pack::open(b.self, b.toc.at(*e), e->len).meta().input;
    } catch (const std::exception&) {
    }
    player::GameSettings s = player::load_game_settings(state_dir() / game / "settings.toml",
                                                        player::default_settings(g->display, g->fullscreen));
    binds = player::gamepad_bindings(own, *g, s);
  }
  return gui::run_input(display, pid, pause, binds);
}

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

int cmd_play(const player::Player& p, const player::Command& c) {
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

  player::PlayRequest req;
  req.dry_run = c.dry_run;
  req.fullscreen_set = c.fullscreen_set;
  req.fullscreen = c.fullscreen;
  panel_size(&req.panel_w, &req.panel_h);

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
                     gb(u.free).c_str(), p.bundle().self.filename().c_str(), id.c_str());
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

int cmd_doctor(const player::Player& p, const player::Command& c) {
  player::doctor::Report rep = p.doctor_report();
  std::string text = player::doctor::render(rep);
  std::fputs(text.c_str(), stdout);
  if (!c.file.empty()) {
    std::ofstream out(c.file);
    out << player::doctor::redact(text);
    if (!out) {
      std::fprintf(stderr, "could not write %s\n", c.file.c_str());
      return 1;
    }
    std::printf("\nsaved to %s, without your home directory or user name\n", c.file.c_str());
  }
  return rep.blocking() ? 1 : 0;
}

int cmd_extract(const player::Player& p, const player::Command& c) {
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

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  const fs::path self = self_path();
  const std::string name = self.filename().string();
  player::Command c = player::parse_command(args);
  if (c.kind == player::Command::Kind::Help) {
    std::fputs(player::usage(name).c_str(), stdout);
    return 0;
  }
  if (c.kind == player::Command::Kind::Error) {
    std::fprintf(stderr, "%s: %s\n\n%s", name.c_str(), c.error.c_str(), player::usage(name).c_str());
    return 2;
  }

  try {
    player::Bundle b = player::Bundle::open(self, env("KRETRO_TOC") ? env("KRETRO_TOC") : "");
    // Before anything else asks where anything is. The gamepad helper runs
    // with the game's HOME, so it takes the state its player chose.
    if (c.kind == player::Command::Kind::Input) {
      player::inherited_state(b, self);
      return cmd_input(b, c.rest);
    }
    player::StateChoice st = player::settle_state(b, self);
    ensure_state_dirs();

    const bool needs_gpu = c.kind != player::Command::Kind::Licenses &&
                           c.kind != player::Command::Kind::SavesExport &&
                           c.kind != player::Command::Kind::SavesImport &&
                           c.kind != player::Command::Kind::ExtractTo;
    gpu::Report gl;
    if (needs_gpu) {
      gl = gpu::probe();
      gpu::materialize(gl);
    }
    rt::Env e = rt::make(needs_gpu ? &gl : nullptr);
    player::Player p(std::move(b), e, st, gl);

    // Said once on a terminal too; the launcher says it in its own window.
    if (!st.refused_portable.empty() && c.kind != player::Command::Kind::Launcher) {
      player::LauncherState ls = player::load_launcher(p.launcher_file());
      if (!ls.portable_fallback_said) {
        std::fprintf(stderr, "%s is there but %s, so saves go to %s instead.\n",
                     st.refused_portable.c_str(), st.refused_why.c_str(), st.dir.c_str());
        ls.portable_fallback_said = true;
        player::save_launcher(p.launcher_file(), ls);
      }
    }

    switch (c.kind) {
      case player::Command::Kind::Launcher: return gui::run_launcher(p);
      case player::Command::Kind::Play: return cmd_play(p, c);
      case player::Command::Kind::Doctor: return cmd_doctor(p, c);
      case player::Command::Kind::Licenses:
        std::fputs(p.licenses_text().c_str(), stdout);
        return 0;
      case player::Command::Kind::SavesExport: {
        fs::path f = p.export_saves(c.game, c.file);
        std::printf("%s - what %s has written\n", f.c_str(), p.game(c.game).name.c_str());
        return 0;
      }
      case player::Command::Kind::SavesImport:
        p.import_saves(c.game, c.file);
        std::printf("restored what %s wrote, from %s; what was there before is kept as a snapshot\n",
                    p.game(c.game).name.c_str(), c.file.c_str());
        return 0;
      case player::Command::Kind::ExtractTo: return cmd_extract(p, c);
      default: return 2;
    }
  } catch (const player::Damaged& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s: %s\n", name.c_str(), ex.what());
    return 1;
  }
}
