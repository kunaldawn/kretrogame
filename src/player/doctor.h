// What this machine can and cannot do, said so a person can act on it.
//
// `--doctor` in the player and `kretro doctor` in the builder print this. It
// is also what a player runs silently before its launcher opens: a blocking
// problem stops there with its message, a warning is shown once.
//
// Collecting is split from probing so the report is testable: collect() is a
// pure function of what the probes found, and render() of what collect()
// made. gather() is the one part that touches the machine.
//
// The report is written to be pasted into a bug report by somebody who does
// not know what is in it, so the saved form carries no home directory and no
// user name. redact() is what guarantees that, and it is applied to the whole
// rendered text rather than field by field, so nothing added later can slip
// past it.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "../gpu/caps.h"
#include "../gpu/probe.h"
#include "../rt/env.h"

namespace kg::player::doctor {

struct Line {
  std::string label;
  std::string value;
};

struct Section {
  std::string title;
  std::vector<Line> lines;
};

struct Report {
  std::vector<Section> sections;
  std::vector<gpu::Problem> problems;  // one each, blocking first
  std::string log_tail;

  bool blocking() const;
};

struct Inputs {
  gpu::Host host;
  gpu::Report gpu;
  gpu::HostCaps caps;
  std::string runtime_glibc;    // the glibc we run on, not the host's
  std::string bundle_id, bundle_title, bundle_version;
  std::string kretro_version;
  std::string runtime_version;
  std::string log_tail;         // the last session's log, already cut to size
  std::vector<Section> extra;   // the caller's own, printed before the problems
};

Report collect(const Inputs& in);
std::string render(const Report& r);

// Removes the home directory (as "~"), any other /home/<name>, and the user
// name wherever it stands as a word. A name shorter than two characters is
// left alone: removing every "a" from a report would make it unreadable and
// hide nobody.
std::string redact(const std::string& text, const std::string& home, const std::string& user);
// The same, for whoever is running this.
std::string redact(const std::string& text);

// The last `n` lines of a file, or empty.
std::string tail_lines(const std::filesystem::path& file, size_t n);

// Runs the runtime's own vulkaninfo and glxinfo, under `e` - which must carry
// the GPU routing a game would get, or the answer is about a different driver.
gpu::Probes runtime_probes(const rt::Env& e);

// Everything, from this machine.
Inputs gather(const rt::Env& e, const gpu::Report& gpu);

}  // namespace kg::player::doctor
