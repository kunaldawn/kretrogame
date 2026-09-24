// kretro games, identify, scan, contents and key: what there is to install,
// and what a disc is.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../disc/database.h"
#include "../disc/disc.h"
#include "../disc/iso.h"
#include "../disc/members.h"
#include "../disc/serial.h"
#include "handlers.h"
#include "../install/discs.h"
#include "../install/collection.h"
#include "../install/keys.h"
#include "../install/manifest.h"
#include "../pack/kgpack.h"
#include "../rt/env.h"
#include "../util/format.h"
#include "../util/hash.h"
#include "../util/paths.h"

namespace fs = std::filesystem;

namespace kg::cli {

int cmd_games(const rt::Env& e, std::vector<std::string>& /*a*/) {
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
      if (fs::exists(game_pack(id), ec)) {
        status = "installed (" + fmt::bytes_iec(fs::file_size(game_pack(id), ec)) + ")";
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

int cmd_identify(const rt::Env& e, std::vector<std::string>& args) {
  const fs::path p = args[0];
  std::error_code ec;
  if (!fs::exists(p, ec)) {
    std::fprintf(stderr, "kretro: no such file: %s\n", p.c_str());
    return 1;
  }
  iso::Info i = iso::scan(p, /*full=*/false);
  std::printf("file          %s\n", p.filename().c_str());
  std::printf("size          %s\n", fmt::bytes_iec(i.size).c_str());
  std::printf("iso 9660      %s\n", i.valid_iso9660 ? "yes" : "no - this may not be a disc image");
  if (i.valid_iso9660) {
    std::printf("volume        %s\n", i.volume_id.c_str());
    if (!i.publisher.empty()) std::printf("publisher     %s\n", i.publisher.c_str());
    if (!i.created.empty()) std::printf("created       %s\n", i.created.c_str());
  }
  std::printf("fingerprint   %s (first %s)\n", to_hex(i.prefix).substr(0, 32).c_str(),
              fmt::bytes_iec(iso::kPrefixBytes).c_str());

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

int cmd_scan(const rt::Env& e, std::vector<std::string>& args) {
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

int cmd_contents(const rt::Env& e, std::vector<std::string>& a) {
  const std::string& ref_s = a[0];
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

int cmd_key(const rt::Env& /*e*/, std::vector<std::string>& a) {
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

}  // namespace kg::cli
