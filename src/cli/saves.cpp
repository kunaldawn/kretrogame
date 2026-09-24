// kretro journal, saves, restore, export-saves and import-saves: what a game
// wrote, and its history.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "handlers.h"
#include "../rt/env.h"
#include "../session/journal.h"
#include "../session/saves.h"
#include "../session/saves_transfer.h"
#include "../util/format.h"

namespace fs = std::filesystem;

namespace kg::cli {

int cmd_journal(const rt::Env& /*e*/, std::vector<std::string>& a) {
  const std::string& id = a[0];
  std::vector<session::Record> j = session::journal(id);
  if (j.empty()) {
    std::printf("No sessions recorded for %s yet.\n", id.c_str());
    return 0;
  }
  double total = 0;
  for (const session::Record& r : j) total += static_cast<double>(r.ended - r.started);
  std::printf("%s\n", id.c_str());
  std::printf("  %zu session%s, %s in total\n", j.size(), j.size() == 1 ? "" : "s",
              fmt::duration(total).c_str());
  std::printf("  last played %s\n\n", fmt::ago(j.front().ended, "never").c_str());
  for (const session::Record& r : j) {
    std::printf("  %s  %-9s  %zu file%s written%s\n", fmt::local_minute(r.started).c_str(),
                fmt::duration(static_cast<double>(r.ended - r.started)).c_str(), r.files_written,
                r.files_written == 1 ? "" : "s",
                r.generation.empty() ? "" : ("  -> " + r.generation).c_str());
    if (!r.note.empty()) std::printf("      \"%s\"\n", r.note.c_str());
  }
  return 0;
}

int cmd_saves(const rt::Env& /*e*/, std::vector<std::string>& a) {
  const std::string& id = a[0];
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

int cmd_restore(const rt::Env& /*e*/, std::vector<std::string>& a) {
  session::restore(a[0], a[1]);
  std::printf("restored %s to %s (the previous state was snapshotted first)\n", a[0].c_str(), a[1].c_str());
  return 0;
}

int cmd_export_saves(const rt::Env& /*e*/, std::vector<std::string>& a) {
  fs::path f = session::export_saves(a[0], a.size() > 1 ? fs::path(a[1]) : fs::path());
  std::printf("%s\n  %s - just what the game wrote\n", f.c_str(),
              fmt::bytes_iec(fs::file_size(f)).c_str());
  return 0;
}

int cmd_import_saves(const rt::Env& /*e*/, std::vector<std::string>& a) {
  session::import_saves(a[0], a[1]);
  std::printf("restored what %s wrote, from %s\n", a[0].c_str(), a[1].c_str());
  std::printf("  what was there before is kept as a snapshot: kretro saves %s\n", a[0].c_str());
  return 0;
}

}  // namespace kg::cli
