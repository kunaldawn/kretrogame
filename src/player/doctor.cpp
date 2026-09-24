#include "doctor.h"

#include <gnu/libc-version.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "../backend/policy.h"
#include "../util/text.h"

namespace kg::player::doctor {
namespace fs = std::filesystem;

namespace {

std::string first_line(const fs::path& p) {
  std::ifstream f(p);
  std::string s;
  std::getline(f, s);
  return trim(s);
}

// PRETTY_NAME="Ubuntu 26.04 LTS" -> Ubuntu 26.04 LTS
std::string distro(const gpu::Host& h) {
  for (const char* f : {"/etc/os-release", "/usr/lib/os-release"}) {
    std::ifstream in(h.at(f));
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
      std::string v = trim(line.substr(12));
      // The quotes off both ends, in place.
      if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) {
        v.erase(0, 1);
        v.pop_back();
      }
      return v;
    }
  }
  return "unknown";
}

// Which libc the host has. We never use it - that is the point of carrying
// our own - but a report from Alpine should say it is from Alpine.
std::string host_libc(const gpu::Host& h) {
  std::error_code ec;
  if (fs::exists(h.at("/lib/ld-musl-x86_64.so.1"), ec)) return "musl";
  for (const char* f : {"/lib64/ld-linux-x86-64.so.2", "/lib/x86_64-linux-gnu/libc.so.6",
                        "/usr/lib/libc.so.6", "/usr/lib64/libc.so.6"}) {
    if (fs::exists(h.at(f), ec)) return "glibc";
  }
  return "unknown";
}

std::string yes_no(bool b) { return b ? "yes" : "no"; }

std::string count_of(int ok, int all, const char* what) {
  if (all == 0) return std::string("no ") + what;
  return std::to_string(ok) + " of " + std::to_string(all) + " " + what;
}

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Replaces `needle` where it is a whole word, ignoring case.
std::string replace_word(const std::string& s, const std::string& needle, const std::string& with) {
  if (needle.empty()) return s;
  std::string ls = to_lower(s), ln = to_lower(needle);
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t p = ls.find(ln, i);
    if (p == std::string::npos) break;
    bool left = p == 0 || !word_char(s[p - 1]);
    bool right = p + ln.size() >= s.size() || !word_char(s[p + ln.size()]);
    out += s.substr(i, p - i);
    out += (left && right) ? with : s.substr(p, ln.size());
    i = p + ln.size();
  }
  out += s.substr(i);
  return out;
}

// Replaces a directory path where it is the whole of a path component chain:
// /home/kim is replaced in /home/kim/x and in "/home/kim", and not in
// /home/kimberly, which is somebody else and is handled as such.
std::string replace_path(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t p = 0;
  while ((p = s.find(from, p)) != std::string::npos) {
    size_t end = p + from.size();
    bool whole = end >= s.size() || !(word_char(s[end]) || s[end] == '.' || s[end] == '-');
    if (!whole) {
      p = end;
      continue;
    }
    s.replace(p, from.size(), to);
    p += to.size();
  }
  return s;
}

}  // namespace

bool Report::blocking() const {
  return std::any_of(problems.begin(), problems.end(), [](const gpu::Problem& p) { return p.blocking(); });
}

Report collect(const Inputs& in) {
  Report r;
  const gpu::HostCaps& c = in.caps;
  const gpu::Report& g = in.gpu;

  r.sections.push_back({"system",
                        {{"distro", distro(in.host)},
                         {"kernel", first_line(in.host.at("/proc/sys/kernel/osrelease"))},
                         {"host libc", host_libc(in.host)},
                         {"our glibc", in.runtime_glibc.empty() ? "unknown" : in.runtime_glibc}}});

  Section disp{"display", {}};
  disp.lines.push_back({"session", gpu::session_name(c.session)});
  disp.lines.push_back({"compositor", c.desktop.empty() ? "unknown" : c.desktop});
  if (g.wayland) disp.lines.push_back({"wayland", g.wayland_display});
  if (g.x11) disp.lines.push_back({"x11", g.x_display});
  disp.lines.push_back({"gamescope", yes_no(c.gamescope)});
  disp.lines.push_back({"games shown", backend::display_path_name(backend::display_path(c))});
  r.sections.push_back(disp);

  Section gfx{"graphics", {}};
  if (g.devices.empty()) gfx.lines.push_back({"gpu", "none found"});
  for (const gpu::Device& d : g.devices) {
    gfx.lines.push_back({d.node, std::string(gpu::vendor_name(d.vendor)) + ", " +
                                     (d.driver.empty() ? "unknown driver" : d.driver) + ", " +
                                     (d.readable ? "accessible" : "NOT ACCESSIBLE")});
  }
  if (!c.vulkan.ran) {
    gfx.lines.push_back({"vulkan", "not checked"});
  } else if (c.vulkan.level == gpu::VulkanLevel::None) {
    gfx.lines.push_back({"vulkan", c.vulkan.software_only ? "software only (lavapipe)" : "none"});
  } else {
    gfx.lines.push_back({"vulkan", c.vulkan.api_version + " on " + c.vulkan.device +
                                       (c.vulkan.driver.empty() ? "" : " (" + c.vulkan.driver + ")")});
  }
  if (!c.gl.ran) gfx.lines.push_back({"opengl", "not checked"});
  else gfx.lines.push_back({"opengl", c.gl.version + " on " + c.gl.renderer});

  std::string nv;
  switch (g.nvidia.state) {
    case gpu::NvidiaState::Absent: nv = "not present"; break;
    case gpu::NvidiaState::Ok:
      nv = "kernel " + g.nvidia.kernel_version + ", libraries match (" +
           std::to_string(g.nvidia.libs.size()) + " taken)";
      break;
    case gpu::NvidiaState::Missing: nv = "kernel " + g.nvidia.kernel_version + ", libraries missing"; break;
    case gpu::NvidiaState::Mismatch:
      nv = "kernel " + g.nvidia.kernel_version + ", libraries " + g.nvidia.library_version + ": MISMATCH";
      break;
  }
  gfx.lines.push_back({"nvidia", nv});
  r.sections.push_back(gfx);

  r.sections.push_back(
      {"mounting",
       {{"fuse", c.fuse() ? "available" : "not available"},
        {"used", c.mount_mode.empty() ? "unknown (not started by the bootstrap)" : c.mount_mode}}});
  r.sections.push_back({"audio", {{"server", c.audio_server}, {"wine drivers", backend::audio_drivers()}}});
  r.sections.push_back(
      {"input",
       {{"gamepads", count_of(c.input_readable, c.input_nodes, "connected gamepads readable")},
        {"hidraw", count_of(c.hidraw_usable, c.hidraw_nodes, "/dev/hidraw devices usable") +
                       " (not needed: SDL reads pads)"}}});

  Section ver{"versions", {}};
  if (!in.bundle_id.empty()) {
    ver.lines.push_back({"bundle", in.bundle_id + (in.bundle_title.empty() ? "" : " - " + in.bundle_title)});
    ver.lines.push_back({"bundle version", in.bundle_version.empty() ? "unknown" : in.bundle_version});
  }
  if (!in.kretro_version.empty()) ver.lines.push_back({"kretro", in.kretro_version});
  ver.lines.push_back({"runtime", in.runtime_version.empty() ? "none" : in.runtime_version});
  r.sections.push_back(ver);

  for (const Section& s : in.extra) r.sections.push_back(s);

  // Blocking first, each once: the GPU report and the capabilities can reach
  // the same conclusion by different roads.
  std::set<std::string> seen;
  for (bool want_blocking : {true, false}) {
    for (const auto* list : {&g.issues, &c.problems}) {
      for (const gpu::Problem& p : *list) {
        if (p.blocking() != want_blocking) continue;
        if (seen.insert(p.line()).second) r.problems.push_back(p);
      }
    }
  }
  r.log_tail = in.log_tail;
  return r;
}

std::string render(const Report& r) {
  std::ostringstream o;
  for (const Section& s : r.sections) {
    o << s.title << "\n";
    size_t w = 0;
    for (const Line& l : s.lines) w = std::max(w, l.label.size());
    for (const Line& l : s.lines) {
      o << "  " << l.label << std::string(w - l.label.size() + 2, ' ') << l.value << "\n";
    }
    o << "\n";
  }
  if (r.problems.empty()) {
    o << "no problems found\n";
  } else {
    o << "problems\n";
    for (const gpu::Problem& p : r.problems) {
      o << (p.blocking() ? "  ! " : "  - ") << p.line() << "\n";
    }
  }
  if (!r.log_tail.empty()) {
    o << "\nlast session log\n";
    std::istringstream in(r.log_tail);
    std::string line;
    while (std::getline(in, line)) o << "  " << line << "\n";
  }
  return o.str();
}

std::string redact(const std::string& text, const std::string& home, const std::string& user) {
  std::string s = text;
  std::string h = home;
  while (h.size() > 1 && h.back() == '/') h.pop_back();
  // "/" or "" as a home would replace every slash; neither is a directory
  // anybody could be identified by.
  if (h.size() > 1) s = replace_path(s, h, "~");

  // Somebody else's home, or ours reached through another spelling.
  static const std::regex other(R"((/(?:var/)?home|/Users)/[^/\s:;"'=]+)");
  s = std::regex_replace(s, other, "/home/<user>");

  if (user.size() >= 2) s = replace_word(s, user, "<user>");
  return s;
}

std::string redact(const std::string& text) {
  std::string home, user;
  if (const char* h = std::getenv("HOME")) home = h;
  for (const char* k : {"USER", "LOGNAME"}) {
    if (const char* u = std::getenv(k); u && *u) {
      user = u;
      break;
    }
  }
  // The password database knows even when the environment was cleaned.
  if (const passwd* pw = getpwuid(getuid())) {
    if (user.empty() && pw->pw_name) user = pw->pw_name;
    if (home.empty() && pw->pw_dir) home = pw->pw_dir;
    std::string r = redact(text, home, user);
    if (pw->pw_name && user != pw->pw_name) r = redact(r, pw->pw_dir ? pw->pw_dir : "", pw->pw_name);
    return r;
  }
  return redact(text, home, user);
}

std::string tail_lines(const fs::path& file, size_t n) {
  std::ifstream f(file);
  if (!f || n == 0) return "";
  std::deque<std::string> last;
  std::string line;
  while (std::getline(f, line)) {
    last.push_back(line);
    if (last.size() > n) last.pop_front();
  }
  std::string out;
  for (const std::string& l : last) out += l + "\n";
  return out;
}

bool save_redacted(const std::string& text, const fs::path& to) {
  std::ofstream out(to);
  out << redact(text);
  return static_cast<bool>(out);
}

fs::path newest_file(const std::vector<fs::path>& candidates) {
  fs::path newest;
  fs::file_time_type when{};
  std::error_code ec;
  for (const fs::path& f : candidates) {
    if (!fs::exists(f, ec)) continue;
    fs::file_time_type t = fs::last_write_time(f, ec);
    if (newest.empty() || t > when) {
      newest = f;
      when = t;
    }
  }
  return newest;
}

gpu::Probes runtime_probes(const rt::Env& e) {
  gpu::Probes p;
  if (!e.valid()) return p;
  auto run = [e](const char* prog, const std::vector<std::string>& args,
                 const char* must) -> std::optional<std::string> {
    fs::path bin = rt::which(e, prog);
    if (bin.empty()) return std::nullopt;
    ProcOptions o;
    o.timeout_sec = 20;
    ProcResult r = rt::run(e, bin, args, o);
    // vulkaninfo exits non-zero over one broken ICD while still describing
    // the working ones; what it printed is the answer either way.
    if (r.out.find(must) == std::string::npos) return std::nullopt;
    return r.out;
  };
  p.vulkaninfo = [run] { return run("vulkaninfo", {"--summary"}, "GPU"); };
  p.glxinfo = [run] {
    const char* d = std::getenv("DISPLAY");
    if (!d || !*d) return std::optional<std::string>();
    return run("glxinfo", {"-B"}, "OpenGL");
  };
  return p;
}

Inputs gather(const rt::Env& e, const gpu::Report& g) {
  Inputs in;
  in.host = gpu::Host::system();
  in.gpu = g;
  in.caps = gpu::probe_caps(in.host, g, runtime_probes(e));
  in.runtime_glibc = gnu_get_libc_version();
  if (e.valid()) in.runtime_version = e.root.filename().string();
  return in;
}

}  // namespace kg::player::doctor
