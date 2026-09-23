// The host as the probes see it: a filesystem root and an environment.
//
// Every read of the host goes through one of these rather than straight to "/"
// and getenv. On a real machine the root is "/" and the environment is ours.
// In a test the root is a fixture tree - a fake /sys/module/nvidia/version, a
// fake /usr/lib full of empty libnvidia files - and the environment is a map,
// so each answer the probes can give is reachable without the hardware that
// would give it.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace kg::gpu {

struct Host {
  std::filesystem::path root = "/";
  // Empty means unset. A variable set to the empty string is treated the same,
  // as it is everywhere else in kretro.
  std::function<std::string(const std::string&)> getenv;

  // The real machine.
  static Host system();
  // A fixture: files under `root`, variables from `env` and nothing else.
  static Host fixture(const std::filesystem::path& root,
                      std::map<std::string, std::string> env = {});

  // An absolute host path, as seen through the root.
  std::filesystem::path at(const std::filesystem::path& abs) const;
  std::string env(const std::string& key) const { return getenv ? getenv(key) : ""; }
};

// Something the person running the program should know. `what` says what is
// wrong and `fix` what they can do about it; rendered together they are one
// line, because a report nobody can act on is a report nobody reads.
struct Problem {
  enum class Severity { Warning, Blocking };
  Severity severity = Severity::Warning;
  std::string what;
  std::string fix;

  bool blocking() const { return severity == Severity::Blocking; }
  std::string line() const { return fix.empty() ? what : what + ". " + fix; }
};

}  // namespace kg::gpu
