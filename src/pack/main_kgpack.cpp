// kgpack(1) - inspect, create and verify kgpack files.
//
// This is the tool the rest of M1 is built on: install writes packs with it,
// the session model verifies them with it, and the tier 1 tests drive it.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "kgpack.h"

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

Kind parse_kind(const std::string& s) {
  if (s == "game") return Kind::Game;
  if (s == "runtime") return Kind::Runtime;
  if (s == "save") return Kind::SaveExport;
  std::fprintf(stderr, "kgpack: unknown kind '%s'\n", s.c_str());
  std::exit(2);
}

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Game: return "game";
    case Kind::Runtime: return "runtime";
    case Kind::SaveExport: return "save-export";
  }
  return "?";
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

int cmd_create(std::vector<std::string> a) {
  fs::path from, out;
  Meta m;
  Kind kind = Kind::Game;
  bool want_body = false;

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
  if (from.empty() || out.empty()) return usage();
  if (m.id.empty()) m.id = from.filename().string();
  if (m.name.empty()) m.name = m.id;

  m.tree = Tree::from_directory(from);
  std::fprintf(stderr, "  tree: %zu entries, %s\n", m.tree.size(),
               human(m.tree.total_bytes()).c_str());

  WriteOptions opt;
  opt.kind = kind;

  fs::path body_tmp;
  if (want_body) {
    body_tmp = fs::path(out).string() + ".body.tmp";
    std::string cmd = "mkdwarfs -i " + from.string() + " -o " + body_tmp.string() +
                      " --log-level=error --no-progress -f";
    std::fprintf(stderr, "  packing with mkdwarfs...\n");
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
      std::fprintf(stderr, "kgpack: mkdwarfs failed (exit %d)\n", rc);
      return 1;
    }
    opt.body = body_tmp;
  }

  write_pack(out, m, opt);
  if (!body_tmp.empty()) fs::remove(body_tmp);

  Pack p = Pack::open(out);
  std::fprintf(stderr, "  wrote %s (%s), root %s\n", out.c_str(),
               human(fs::file_size(out)).c_str(), to_hex(p.header().blake3_root).c_str());
  return 0;
}

int cmd_info(const fs::path& p) {
  Pack pk = Pack::open(p);
  const Header& h = pk.header();
  const Meta& m = pk.meta();
  std::printf("file          %s (%s)\n", p.c_str(), human(fs::file_size(p)).c_str());
  std::printf("kind          %s\n", kind_name(h.kind));
  std::printf("format        v%u, container rev %u\n", h.format_version, h.revision);
  std::printf("id            %s\n", m.id.c_str());
  std::printf("name          %s%s", m.name.c_str(), m.year ? "" : "\n");
  if (m.year) std::printf(" (%u)\n", m.year);
  std::printf("tree          %zu entries, %s\n", m.tree.size(), human(m.tree.total_bytes()).c_str());
  std::printf("merkle root   %s\n", to_hex(h.blake3_root).c_str());
  if (h.has_body()) {
    std::printf("body          %s %s at +%llu\n", h.body_is_squashfs() ? "squashfs" : "dwarfs",
                human(h.body_len).c_str(), static_cast<unsigned long long>(h.body_off));
    double ratio = m.tree.total_bytes() ? 100.0 * static_cast<double>(h.body_len) /
                                              static_cast<double>(m.tree.total_bytes())
                                        : 0.0;
    std::printf("              %.1f%% of the tree's %s\n", ratio, human(m.tree.total_bytes()).c_str());
  } else {
    std::printf("body          none - this is a recipe pack\n");
  }
  // What the body is shaped like, and what the recipe says was done to make
  // it. Checking a pack by hand meant decoding the CBOR to find out whether
  // the layout was rooted, whether the discs came along and whether the
  // fingerprints were ever written - all of it already in the metadata this
  // command reads, and none of it printed.
  std::printf("layout        %s\n",
              m.rooted() ? "rooted - game/ , system/ , discs/<n>/ , registry.reg"
                         : "flat - the image is the game tree");
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
  // The discs and their fingerprints are one list each, indexed together:
  // disc i is fingerprint i, and lay_out_body puts it at discs/<i+1>/. A
  // recipe pack carries the fingerprints and no discs, so print whichever is
  // longer and say which half is missing.
  size_t discs = m.discs.size() > m.recipe.fingerprints.size() ? m.discs.size()
                                                               : m.recipe.fingerprints.size();
  for (size_t i = 0; i < discs; ++i) {
    char head[32];
    std::snprintf(head, sizeof(head), "disc %zu", i + 1);
    if (i < m.discs.size()) {
      const Meta::Disc& d = m.discs[i];
      std::printf("%-13s %s  serial %08x  %s\n", head, d.label.c_str(), d.serial,
                  d.embedded ? "carried at discs/" : "not carried - needs the original");
    } else {
      std::printf("%-13s named by the recipe, not carried\n", head);
    }
    if (i < m.recipe.fingerprints.size()) {
      const DiscFingerprint& f = m.recipe.fingerprints[i];
      std::printf("              %s  %s  %s\n", f.filename.c_str(), human(f.size).c_str(),
                  to_hex(f.blake3).substr(0, 16).c_str());
    }
  }
  if (m.system.files) {
    std::printf("system files  %u, %s (outside the game folder)\n", m.system.files,
                human(m.system.bytes).c_str());
  }
  if (!m.run.exe.empty()) std::printf("run           %s\n", m.run.exe.c_str());
  if (!m.runtime.id.empty()) std::printf("runtime pin   %s\n", m.runtime.id.c_str());
  return 0;
}

int cmd_verify(const fs::path& p) {
  Pack pk = Pack::open(p);
  Pack::Verification v = pk.verify();
  std::printf("%s: %s\n", p.c_str(), v.detail.c_str());
  return v.ok ? 0 : 1;
}

int cmd_tree(const fs::path& p) {
  Pack pk = Pack::open(p);
  std::string c = pk.meta().tree.canonical();
  std::fwrite(c.data(), 1, c.size(), stdout);
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
