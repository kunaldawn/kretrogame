// kretro info, doctor, wine, exec and stage-probe: the runtime itself.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli.h"
#include "../gpu/probe.h"
#include "../gui/stage/stage_probe.h"
#include "handlers.h"
#include "../install/collection.h"
#include "../player/doctor.h"
#include "../rt/env.h"
#include "../util/paths.h"

namespace fs = std::filesystem;

namespace kg::cli {

int cmd_info(const rt::Env& e, std::vector<std::string>& /*a*/) {
  std::printf("runtime       %s\n", e.valid() ? e.root.c_str() : "(none - running outside the bootstrap)");
  if (e.valid() && fs::exists(e.root / "RUNTIME")) {
    std::ifstream f(e.root / "RUNTIME");
    std::string line;
    while (std::getline(f, line)) std::printf("  %s\n", line.c_str());
  }
  std::printf("state         %s\n", state_dir().c_str());
  std::printf("  games       %s\n", games_dir().c_str());
  std::printf("  packs       %s\n", packs_dir().c_str());
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
int cmd_doctor(const rt::Env& /*env*/, std::vector<std::string>& a) {
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
  std::vector<fs::path> logs;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(home_dir(), ec)) {
    logs.push_back(de.path() / "weston.log");
  }
  fs::path newest = doc::newest_file(logs);
  if (!newest.empty()) in.log_tail = doc::tail_lines(newest, 40);

  doc::Report rep = doc::collect(in);
  std::string text = doc::render(rep);
  std::fputs(text.c_str(), stdout);

  if (a.size() == 2 && a[0] == "--save") {
    if (!doc::save_redacted(text, a[1])) {
      std::fprintf(stderr, "kretro: could not write %s\n", a[1].c_str());
      return 1;
    }
    std::printf("\nsaved to %s, without your home directory or user name\n", a[1].c_str());
  }
  // Only what stops a game from starting fails the command; a warning such as
  // "no hidraw access" is true of most machines and would make it always fail.
  return rep.blocking() ? 1 : 0;
}

// rt::exec does not return when it runs the program. What follows it is the
// help, as for any command line nothing matched.
int cmd_wine(const rt::Env& e, std::vector<std::string>& a) {
  fs::path w = rt::find_wine(e.root);
  if (w.empty()) { std::fprintf(stderr, "kretro: no wine in this runtime\n"); return 1; }
  rt::exec(e, w, a);
  return usage();
}

int cmd_exec(const rt::Env& e, std::vector<std::string>& a) {
  fs::path prog = a[0];
  if (prog.is_relative()) {
    fs::path found = rt::which(e, a[0]);
    if (found.empty()) { std::fprintf(stderr, "kretro: no %s in this runtime\n", a[0].c_str()); return 1; }
    prog = found;
  }
  a.erase(a.begin());
  rt::exec(e, prog, a);
  return usage();
}

int cmd_stage_probe(const rt::Env& e, std::vector<std::string>& a) {
  return gui::stage_probe(e, a.empty() ? 20 : std::atoi(a[0].c_str()));
}

}  // namespace kg::cli
