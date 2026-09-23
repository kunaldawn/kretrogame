#include "container.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

#include "cue.h"

namespace fs = std::filesystem;

namespace kg::disc {
namespace {

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string basename_of(const std::string& p) {
  size_t slash = p.find_last_of("/\\");
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

}  // namespace

std::vector<Member> parse_7z_listing(std::string_view out) {
  std::vector<Member> v;
  std::istringstream is{std::string(out)};
  std::string line;
  Member cur;
  bool have = false;
  auto flush = [&] {
    if (have && !cur.path.empty()) v.push_back(cur);
    cur = Member{};
    have = false;
  };
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) { flush(); continue; }
    size_t eq = line.find(" = ");
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 3);
    if (key == "Path") {
      flush();
      cur.path = val;
      have = true;
    } else if (key == "Size" && have) {
      cur.size = val.empty() ? 0 : std::strtoull(val.c_str(), nullptr, 10);
    }
  }
  flush();
  return v;
}

bool is_disc_image(std::string_view name) {
  std::string n = lower(name);
  return ends_with(n, ".iso") || ends_with(n, ".cue") || ends_with(n, ".bin") ||
         ends_with(n, ".img") || ends_with(n, ".mdf");
}

bool is_archive(std::string_view name) {
  std::string n = lower(name);
  return ends_with(n, ".zip") || ends_with(n, ".7z") || ends_with(n, ".rar");
}

namespace {

// One level of listing, through the bundled 7z.
std::vector<Member> list_archive(const rt::Env& e, const fs::path& p) {
  fs::path sevenzip = rt::which(e, "7z");
  if (sevenzip.empty()) throw std::runtime_error("disc: the runtime has no 7z");
  auto r = rt::run(e, sevenzip, {"l", "-slt", "-ba", p.string()});
  if (!r.ok()) return {};
  return parse_7z_listing(r.out);
}

// `root` is the file in the collection; every Candidate names it, because
// materialise() peels the trail starting from there. `on_disk` is whatever we
// are currently looking inside, which for a nested archive is a temporary copy
// that will not outlive the probe.
void probe_into(const rt::Env& e, const fs::path& root, const fs::path& on_disk,
                const std::vector<std::string>& trail, int depth,
                const fs::path& work, std::vector<Candidate>& out);

// Descending into a nested archive means extracting it, because 7z cannot list
// through two layers. This is the one place the probe touches the disk, and it
// only ever writes the intermediate archive, never the disc image inside it.
void descend(const rt::Env& e, const fs::path& root, const fs::path& outer,
             const std::vector<std::string>& trail, const Member& m, int depth,
             const fs::path& work, std::vector<Candidate>& out) {
  if (depth + 1 >= kMaxDepth) return;
  std::error_code ec;
  fs::path stage = work / ("depth" + std::to_string(depth) + "-" + std::to_string(out.size()));
  fs::create_directories(stage, ec);
  fs::path sevenzip = rt::which(e, "7z");
  auto r = rt::run(e, sevenzip, {"e", "-y", "-o" + stage.string(), outer.string(), m.path});
  if (!r.ok()) return;
  fs::path inner = stage / basename_of(m.path);
  if (!fs::exists(inner, ec)) return;
  std::vector<std::string> t = trail;
  t.push_back(m.path);
  probe_into(e, root, inner, t, depth + 1, work, out);
}

void probe_into(const rt::Env& e, const fs::path& root, const fs::path& on_disk,
                const std::vector<std::string>& trail, int depth,
                const fs::path& work, std::vector<Candidate>& out) {
  std::error_code ec;
  std::string name = on_disk.filename().string();

  // A bare image on disk: no listing needed.
  if (trail.empty() && is_disc_image(name)) {
    Candidate c;
    c.archive = root;
    c.name = name;
    c.size = fs::file_size(on_disk, ec);
    c.is_cue = ends_with(lower(name), ".cue");
    out.push_back(c);
    return;
  }
  if (trail.empty() && !is_archive(name)) return;

  std::vector<Member> members = list_archive(e, on_disk);

  // A .bin is reached through its cue sheet, which knows the sector format and
  // which tracks are audio. Only when the archive has no cue at all does a
  // .bin stand on its own.
  bool has_cue = false;
  for (const Member& m : members) {
    if (ends_with(lower(basename_of(m.path)), ".cue")) has_cue = true;
  }

  for (const Member& m : members) {
    std::string base = basename_of(m.path);
    if (base.empty()) continue;
    if (is_disc_image(base)) {
      if (has_cue && ends_with(lower(base), ".bin")) continue;
      Candidate c;
      c.archive = root;
      c.members = trail;
      c.members.push_back(m.path);
      c.name = base;
      c.size = m.size;
      c.is_cue = ends_with(lower(base), ".cue");
      out.push_back(c);
    } else if (is_archive(base)) {
      descend(e, root, on_disk, trail, m, depth, work, out);
    }
  }
}

}  // namespace

std::vector<Candidate> probe(const rt::Env& e, const fs::path& p) {
  std::vector<Candidate> out;
  std::error_code ec;
  if (!fs::exists(p, ec)) return out;
  fs::path work = fs::temp_directory_path() / ("kretro-probe-" + std::to_string(::getpid()));
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  try {
    probe_into(e, p, p, {}, 0, work, out);
  } catch (...) {
    fs::remove_all(work, ec);
    throw;
  }
  fs::remove_all(work, ec);
  return out;
}

fs::path materialise(const rt::Env& e, const Candidate& c, const fs::path& work) {
  std::error_code ec;
  fs::create_directories(work, ec);
  fs::path sevenzip = rt::which(e, "7z");
  if (sevenzip.empty()) throw std::runtime_error("disc: the runtime has no 7z");

  // Peel the trail one archive at a time, each into its own directory.
  fs::path current = c.archive;
  for (size_t i = 0; i + 1 < c.members.size(); ++i) {
    fs::path stage = work / ("peel" + std::to_string(i));
    fs::create_directories(stage, ec);
    auto r = rt::run(e, sevenzip, {"e", "-y", "-o" + stage.string(), current.string(), c.members[i]});
    if (!r.ok()) throw std::runtime_error("disc: cannot extract " + c.members[i] + "\n" + r.out);
    current = stage / basename_of(c.members[i]);
  }

  if (c.members.empty()) return current;  // the file on disk is the image

  const std::string& inner = c.members.back();
  fs::path stage = work / "disc";
  fs::create_directories(stage, ec);

  if (!c.is_cue) {
    auto r = rt::run(e, sevenzip, {"e", "-y", "-o" + stage.string(), current.string(), inner});
    if (!r.ok()) throw std::runtime_error("disc: cannot extract " + inner + "\n" + r.out);
    return stage / basename_of(inner);
  }

  // A cue sheet is useless without the tracks it names. Extract it, read it,
  // then extract every file it mentions from the same directory of the archive.
  auto r = rt::run(e, sevenzip, {"e", "-y", "-o" + stage.string(), current.string(), inner});
  if (!r.ok()) throw std::runtime_error("disc: cannot extract " + inner + "\n" + r.out);
  fs::path cue_path = stage / basename_of(inner);
  std::ifstream cf(cue_path);
  std::stringstream ss;
  ss << cf.rdbuf();
  CueSheet sheet = parse_cue(ss.str());

  std::string dir;
  size_t slash = inner.find_last_of('/');
  if (slash != std::string::npos) dir = inner.substr(0, slash + 1);

  std::vector<std::string> wanted;
  for (const CueTrack& t : sheet.tracks) {
    if (std::find(wanted.begin(), wanted.end(), t.file) == wanted.end()) wanted.push_back(t.file);
  }
  for (const std::string& f : wanted) {
    if (fs::exists(stage / f, ec)) continue;
    auto rr = rt::run(e, sevenzip, {"e", "-y", "-o" + stage.string(), current.string(), dir + f});
    if (!rr.ok()) {
      throw std::runtime_error("disc: the cue sheet names " + f + ", which is not in the archive");
    }
  }
  return cue_path;
}

}  // namespace kg::disc
