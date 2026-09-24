#include "survey.h"

#include <algorithm>
#include <map>
#include <system_error>
#include <utility>

#include "../util/fs_ci.h"
#include "../util/text.h"

namespace kg::install {
namespace fs = std::filesystem;

std::string protection_of(std::string_view filename) {
  // SafeDisc and SecuROM read raw sectors below the filesystem. These are the
  // files each leaves beside the executable it wraps.
  static const struct { const char* file; const char* what; } kSigns[] = {
      {"drvmgt.dll", "SafeDisc"},
      {"secdrv.sys", "SafeDisc"},
      {"cmdlineext.dll", "SecuROM"},
      {"sintf32.dll", "SecuROM"},
  };
  std::string n = to_lower(std::string(filename));
  for (const auto& s : kSigns) {
    if (n == s.file) return s.what;
  }
  return {};
}

namespace {

// Lower sorts earlier. The numbers are spaced so a later tier can be inserted
// without renumbering the ones around it.
int exe_tier(const std::string& filename, bool prefer_setup) {
  std::string n = to_lower(filename);
  // An uninstaller is never what runs the game, and it is frequently the
  // largest executable in the directory - InstallShield's is a megabyte - so
  // this has to beat the size rule outright rather than nudge it.
  if (n.rfind("unins", 0) == 0 || n.find("uninstall") != std::string::npos) return 90;
  if (prefer_setup) {
    if (n.rfind("setup", 0) == 0) return 0;
    if (n.rfind("install", 0) == 0) return 10;
    if (n.rfind("autorun", 0) == 0) return 20;
  }
  return 50;
}

size_t depth_of(const fs::path& p) {
  size_t n = 0;
  for (auto it = p.begin(); it != p.end(); ++it) ++n;
  return n;
}

}  // namespace

std::vector<fs::path> rank_executables(const fs::path& dir, bool prefer_setup) {
  struct Row {
    fs::path rel;
    uint64_t size = 0;
    int tier = 0;
  };
  std::vector<Row> rows;
  std::error_code ec;

  auto consider = [&](const fs::directory_entry& de, const fs::path& rel) {
    std::error_code e2;
    if (!de.is_regular_file(e2)) return;
    if (to_lower(de.path().extension().string()) != ".exe") return;
    rows.push_back(Row{rel, de.file_size(e2), exe_tier(de.path().filename().string(),
                                                       prefer_setup)});
  };

  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    consider(de, de.path().filename());
    // The disc root and one directory down, and no further. A disc carrying
    // several games may keep each one's setup a directory in; nothing real is
    // two directories in, and a full walk of a 700 MB disc for a list nobody
    // reads to the bottom is not free.
    if (!prefer_setup) continue;
    std::error_code e2;
    if (!de.is_directory(e2)) continue;
    for (const fs::directory_entry& sub : fs::directory_iterator(de.path(), e2)) {
      consider(sub, de.path().filename() / sub.path().filename());
    }
  }

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.tier != b.tier) return a.tier < b.tier;
    if (a.size != b.size) return a.size > b.size;
    return a.rel.generic_string() < b.rel.generic_string();
  });

  std::vector<fs::path> out;
  out.reserve(rows.size());
  for (const Row& r : rows) out.push_back(r.rel);
  return out;
}

std::vector<Candidate> rank_candidates(const fs::path& drive_c, const Tree::Diff& d) {
  // Every ancestor of every added file, not only the file's own directory.
  // An installer writes into Program Files\Publisher\Game, and the person
  // choosing has to be able to see the publisher directory as a choice - the
  // game is sometimes one level up from where the .exe landed.
  std::map<std::string, Candidate> by_dir;
  for (const TreeEntry& te : d.added) {
    if (te.is_dir()) continue;
    fs::path dir = fs::path(te.path).parent_path();
    while (!dir.empty()) {
      Candidate& c = by_dir[dir.generic_string()];
      c.dir = dir;
      ++c.files;
      c.bytes += te.size;
      dir = dir.parent_path();
    }
  }

  std::vector<Candidate> out;
  out.reserve(by_dir.size());
  for (auto& kv : by_dir) out.push_back(std::move(kv.second));

  // Deepest first, then by file count. The deepest directory is not always the
  // game's - a save subdirectory the installer pre-created is deeper still -
  // which is precisely why this is a ranking shown to a person rather than an
  // answer. detect_install_dir had to guess; the wizard asks.
  std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
    size_t da = depth_of(a.dir), db = depth_of(b.dir);
    if (da != db) return da > db;
    if (a.files != b.files) return a.files > b.files;
    return a.dir.generic_string() < b.dir.generic_string();
  });

  for (Candidate& c : out) c.executables = rank_executables(drive_c / c.dir, false);
  return out;
}

Candidate survey_tree(const fs::path& tree) {
  Candidate c;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::recursive_directory_iterator(tree, ec)) {
    std::error_code e2;
    if (!de.is_regular_file(e2)) continue;
    ++c.files;
    c.bytes += de.file_size(e2);
  }
  // `dir` stays empty. Everything that reads a Candidate joins it onto
  // installed_root(), and for a copy that join is the tree itself.
  c.executables = rank_executables(tree, false);
  return c;
}

size_t exe_index(const std::vector<fs::path>& exes, const fs::path& want) {
  std::string w = want.generic_string();
  std::replace(w.begin(), w.end(), '\\', '/');
  if (w.empty()) return exes.size();
  // A manifest names the executable, not a path to it, so the comparison is on
  // the filename either way. Case-insensitively: a disc spells its own names
  // however it likes and Game.exe arrives as GAME.EXE.
  w = to_lower(fs::path(w).filename().string());
  for (size_t i = 0; i < exes.size(); ++i) {
    if (to_lower(exes[i].filename().string()) == w) return i;
  }
  return exes.size();
}

size_t count_entries(const fs::path& dir) {
  std::error_code ec;
  size_t n = 0;
  for (auto it = fs::recursive_directory_iterator(dir, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    ++n;
  }
  return n;
}

std::vector<std::string> verify_list(const fs::path& dir, const fs::path& exe) {
  std::vector<std::string> out;
  std::string exe_name = exe.filename().string();
  if (!exe_name.empty()) out.push_back(exe_name);

  struct Row {
    std::string name;
    uint64_t size = 0;
  };
  std::vector<Row> rows;
  std::error_code ec;
  for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
    std::error_code e2;
    // Top level only, and no directories. A verify entry with a separator in
    // it is a claim about the shape of the tree as well as its contents, and
    // the shape is the part an installer is free to change between releases.
    if (!de.is_regular_file(e2)) continue;
    std::string n = de.path().filename().string();
    if (n == exe_name) continue;
    rows.push_back(Row{n, de.file_size(e2)});
  }
  // Largest first: the big immovable assets are what a different release of
  // the same game keeps, and a 40 KB readme is what it changes.
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.size != b.size) return a.size > b.size;
    return a.name < b.name;
  });

  for (const Row& r : rows) {
    if (out.size() >= 5) break;
    out.push_back(r.name);
  }
  return out;
}

std::vector<std::string> verify_for(const Draft& d, const fs::path& installed_root) {
  // Whatever step 6 read off the directory the user confirmed, when it read
  // one. Copy and unzip put the game at the staging tree and never touch
  // drive_c, so the fallback reads installed_root - whichever of the two the
  // method used - joined onto install_dir, which for a copy is empty because
  // there the tree is the game.
  if (!d.verify.empty()) return d.verify;
  return verify_list(installed_root / d.install_dir, d.exe);
}

fs::path find_install_dir(const std::vector<Candidate>& candidates, const fs::path& drive_c,
                          const std::vector<std::string>& verify) {
  // Exactly detect_install_dir's rule, over a list that is now ranked rather
  // than sorted on the spot: the deepest directory holding every verify
  // entry at once.
  fs::path installed;
  if (!verify.empty()) {
    for (const Candidate& c : candidates) {
      bool all = true;
      for (const std::string& v : verify) {
        if (!exists_ci(drive_c / c.dir, v)) { all = false; break; }
      }
      if (all) { installed = c.dir; break; }
    }
  }
  return installed;
}

}  // namespace kg::install
