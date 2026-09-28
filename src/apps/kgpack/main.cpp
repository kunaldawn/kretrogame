// kgpack(1) - inspect, create and verify kgpack files.
//
// The tool the rest of kretro's pack handling is built on: install writes
// packs with it, the session model verifies them with it, and the tier 1
// tests drive it.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "../../pack/kgpack.h"
#include "../../util/format.h"

namespace fs = std::filesystem;
using namespace kg;

namespace {

int usage() {
  std::fputs(
      "kgpack - create and inspect kgpack files\n"
      "\n"
      "  kgpack create --from <dir> --out <pack> [options]\n"
      "  kgpack info <pack>\n"
      "  kgpack verify <pack>\n"
      "  kgpack tree <pack>\n"
      "  kgpack extract-body <pack> <out>\n"
      "\n"
      "create options:\n"
      "  --id <id>            game or runtime identifier\n"
      "  --name <name>        human readable name\n"
      "  --year <n>\n"
      "  --kind game|runtime|save\n"
      "  --exe <path>         executable, relative to the tree root\n"
      "  --dwarfs             pack <dir> with mkdwarfs and embed it as the body\n"
      "                       (omit for a recipe pack, which carries no bytes)\n",
      stderr);
  return 2;
}

std::string arg_after(const std::vector<std::string>& a, size_t& i) {
  if (i + 1 >= a.size()) {
    std::fprintf(stderr, "kgpack: %s needs a value\n", a[i].c_str());
    std::exit(2);
  }
  return a[++i];
}

PackKind parse_kind(const std::string& s) {
  std::optional<PackKind> k = parse_pack_kind(s);
  if (!k) {
    std::fprintf(stderr, "kgpack: unknown kind '%s'\n", s.c_str());
    std::exit(2);
  }
  return *k;
}

int cmd_create(const std::vector<std::string>& a) {
  fs::path from, out;
  Meta m;
  PackKind kind = PackKind::Game;
  bool want_body = false;

  // clang-format off: the unknown-flag branch stays one line, like its siblings.
  for (size_t i = 0; i < a.size(); ++i) {
    const std::string& f = a[i];
    if (f == "--from") from = arg_after(a, i);
    else if (f == "--out") out = arg_after(a, i);
    else if (f == "--id") m.id = arg_after(a, i);
    else if (f == "--name") m.name = arg_after(a, i);
    else if (f == "--year") m.year = static_cast<uint32_t>(std::stoul(arg_after(a, i)));
    else if (f == "--exe") m.run.exe = arg_after(a, i);
    else if (f == "--kind") kind = parse_kind(arg_after(a, i));
    else if (f == "--dwarfs") want_body = true;
    else { std::fprintf(stderr, "kgpack: unknown option '%s'\n", f.c_str()); return 2; }
  }
  // clang-format on
  if (from.empty() || out.empty()) return usage();
  if (m.id.empty()) m.id = from.filename().string();
  if (m.name.empty()) m.name = m.id;

  m.tree = Tree::from_directory(from);
  std::fprintf(stderr, "  tree: %zu entries, %s\n", m.tree.size(),
               fmt::bytes_iec(m.tree.total_bytes()).c_str());

  WriteOptions opt;
  opt.kind = kind;

  fs::path body_tmp;
  fs::path stage;
  if (want_body) {
    body_tmp = fs::path(out).string() + ".body.tmp";
    // A game's body is a set's: the tree goes in at games/<id>/game, linked
    // rather than copied, so the pack is laid out the way install lays one out.
    fs::path in = from;
    if (kind == PackKind::Game) {
      stage = fs::path(out).string() + ".stage";
      fs::remove_all(stage);
      fs::create_directories(stage / body_game_dir(m.id));
      fs::copy(from, stage / body_game_dir(m.id) / "game",
               fs::copy_options::recursive | fs::copy_options::create_hard_links);
      in = stage;
    }
    std::string cmd = "mkdwarfs -i " + in.string() + " -o " + body_tmp.string() +
                      " --log-level=error --no-progress -f";
    std::fprintf(stderr, "  packing with mkdwarfs...\n");
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
      std::fprintf(stderr, "kgpack: mkdwarfs failed (exit %d)\n", rc);
      return 1;
    }
    opt.body = body_tmp;
  }

  write_pack(out, set_of(m), opt);
  if (!body_tmp.empty()) fs::remove(body_tmp);
  if (!stage.empty()) fs::remove_all(stage);

  Pack p = Pack::open(out);
  std::fprintf(stderr, "  wrote %s (%s), root %s\n", out.c_str(),
               fmt::bytes_iec(fs::file_size(out)).c_str(), to_hex(p.header().blake3_root).c_str());
  return 0;
}

// One game of the set: who it is, how it was made, and the drives it gets.
void print_game(const Meta& m, bool carried) {
  std::printf("id            %s\n", m.id.c_str());
  std::printf("name          %s%s", m.name.c_str(), m.year ? "" : "\n");
  if (m.year) std::printf(" (%u)\n", m.year);
  std::printf("tree          %zu entries, %s\n", m.tree.size(), fmt::bytes_iec(m.tree.total_bytes()).c_str());
  std::printf("game root     %s\n", to_hex(m.tree.root()).c_str());
  if (!m.recipe.method.empty()) {
    std::string r = m.recipe.method;
    if (!m.recipe.member.empty()) r += "  " + m.recipe.member;
    if (!m.recipe.setup.empty()) r += "  " + m.recipe.setup;
    if (!m.recipe.subdir.empty()) r += "  in " + m.recipe.subdir;
    std::printf("recipe        %s\n", r.c_str());
  }
  if (!m.recipe.verify.empty()) {
    std::string v;
    for (size_t i = 0; i < m.recipe.verify.size(); ++i) v += (i ? ", " : "") + m.recipe.verify[i];
    std::printf("verify        %s\n", v.c_str());
  }
  // The game's discs in drive order, D: first, and the fingerprints its
  // recipe recorded for them: disc i is fingerprint i. A recipe pack carries
  // no discs' trees, so print whichever list is longer and say which half is
  // missing.
  size_t discs = std::max(m.discs.size(), m.recipe.fingerprints.size());
  for (size_t i = 0; i < discs; ++i) {
    char head[32];
    std::snprintf(head, sizeof(head), "disc %zu", i + 1);
    if (i < m.discs.size()) {
      const Meta::Disc& d = m.discs[i];
      const std::string where = carried ? "carried at discs/" + d.key + "/" : "named, not carried";
      std::printf("%-13s %s  serial %08x  %c:  %s\n", head, d.label.c_str(), d.serial, static_cast<char>('D' + i),
                  where.c_str());
    } else {
      std::printf("%-13s named by the recipe, not carried\n", head);
    }
    if (i < m.recipe.fingerprints.size()) {
      const DiscFingerprint& f = m.recipe.fingerprints[i];
      std::printf("              %s  %s  %s\n", f.filename.c_str(), fmt::bytes_iec(f.size).c_str(),
                  to_hex(f.blake3).substr(0, 16).c_str());
    }
  }
  if (m.system.files) {
    std::printf("system files  %u, %s (outside the game folder)\n", m.system.files,
                fmt::bytes_iec(m.system.bytes).c_str());
  }
  if (!m.run.exe.empty()) std::printf("run           %s\n", m.run.exe.c_str());
  if (!m.runtime.id.empty()) std::printf("runtime pin   %s\n", m.runtime.id.c_str());
}

int cmd_info(const fs::path& p) {
  Pack pk = Pack::open(p);
  const Header& h = pk.header();
  const SetMeta& set = pk.set();
  std::printf("file          %s (%s)\n", p.c_str(), fmt::bytes_iec(fs::file_size(p)).c_str());
  std::printf("kind          %s\n", pack_kind_name(h.kind));
  std::printf("format        v%u, container rev %u\n", h.format_version, h.revision);
  std::printf("set           %s\n", set.set_id.c_str());
  // What the body is shaped like, and what each game's recipe says was done
  // to make it: checking a pack by hand should not mean decoding its CBOR.
  std::printf("layout        %s\n", "a set - games/<id>/ , discs/<key>/");
  std::printf("merkle root   %s\n", to_hex(h.blake3_root).c_str());
  uint64_t trees = 0;
  for (const Meta& g : set.games) trees += g.tree.total_bytes();
  if (h.has_body()) {
    std::printf("body          %s %s at +%llu\n", h.body_is_squashfs() ? "squashfs" : "dwarfs",
                fmt::bytes_iec(h.body_len).c_str(), static_cast<unsigned long long>(h.body_off));
    double ratio = trees ? 100.0 * static_cast<double>(h.body_len) / static_cast<double>(trees) : 0.0;
    std::printf("              %.1f%% of the games' trees, %s\n", ratio, fmt::bytes_iec(trees).c_str());
  } else {
    std::printf("body          none - this is a recipe pack\n");
  }
  // The set's discs, each once, whichever of its games use them.
  for (const Meta::Disc& d : set.discs) {
    std::printf("set disc      %s  %s  serial %08x  %s\n", d.key.c_str(), d.label.c_str(), d.serial,
                fmt::bytes_iec(d.bytes).c_str());
  }
  std::printf("games         %zu\n", set.games.size());
  for (const Meta& g : set.games) {
    std::printf("\n");
    print_game(g, h.has_body());
  }
  return 0;
}

int cmd_verify(const fs::path& p) {
  Pack pk = Pack::open(p);
  Pack::Verification v = pk.verify();
  std::printf("%s: %s\n", p.c_str(), v.detail.c_str());
  return v.ok ? 0 : 1;
}

int cmd_tree(const fs::path& p) {
  // One game's tree as it is; each game's under a line naming it, for a set
  // of several.
  Pack pk = Pack::open(p);
  for (const Meta& g : pk.games()) {
    if (pk.games().size() > 1) std::printf("# %s
", g.id.c_str());
    std::string c = g.tree.canonical();
    std::fwrite(c.data(), 1, c.size(), stdout);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> a(argv + 1, argv + argc);
  if (a.empty()) return usage();
  const std::string cmd = a[0];
  a.erase(a.begin());

  try {
    if (cmd == "create") return cmd_create(a);
    if (cmd == "info" && a.size() == 1) return cmd_info(a[0]);
    if (cmd == "verify" && a.size() == 1) return cmd_verify(a[0]);
    if (cmd == "tree" && a.size() == 1) return cmd_tree(a[0]);
    if (cmd == "extract-body" && a.size() == 2) {
      Pack::open(a[0]).extract_body(a[1]);
      return 0;
    }
    return usage();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "kgpack: %s\n", e.what());
    return 1;
  }
}
