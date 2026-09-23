// The bundled runtime: where its parts are, and how to run them.
//
// Nothing inside the runtime can have been built knowing where it would live,
// because its path contains the hash of its own contents. Every path it needs
// is therefore handed to it here.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "../gpu/probe.h"
#include "../util/proc.h"

namespace kg::rt {

struct Env {
  std::filesystem::path root;
  std::string library_path;
  std::vector<std::pair<std::string, std::string>> vars;

  bool valid() const { return !root.empty(); }
  std::filesystem::path loader() const { return root / "lib" / "ld-linux-x86-64.so.2"; }

  // Adds to `vars`, replacing any existing entry with the same name.
  void set(const std::string& key, const std::string& value);
};

// Builds the environment. Pass the GPU report when the program will draw;
// nullptr is fine for extraction tools, which never touch a display.
Env make(const gpu::Report* gl);

// Gives Wine a home of its own: HOME and every XDG base directory under it.
// HOME alone is not enough. Wine's winemenubuilder writes menu entries and
// icons wherever XDG_DATA_HOME points, and a person who sets that variable
// would find a 1998 installer's shortcuts in their real application menu - a
// write outside the player's state that the spec promises never happens.
void confine_home(Env& e, const std::filesystem::path& home);

// Finds a bundled program by name: bin, usr/bin, then Wine's own directories.
std::filesystem::path which(const Env& e, const std::string& name);

std::filesystem::path find_wine(const std::filesystem::path& root);

// True for a real ELF; false for a shell script or anything else.
bool is_elf(const std::filesystem::path& p);

// Runs a bundled program through the runtime's loader, which is what keeps the
// host's libc out of it: the program's own PT_INTERP names a path that may not
// exist on this machine at all.
ProcResult run(const Env& e, const std::filesystem::path& prog,
               const std::vector<std::string>& args, ProcOptions opt = {});

// As run, with the loader executed from an anonymous in-memory copy of itself
// rather than from its path. What that changes is the AppArmor label the
// program runs under, and nothing else; see mount_overlay in session.cpp for
// the one caller and why. Comes back not ok, like run, when this kernel will
// not execute a memfd.
ProcResult run_unnamed(const Env& e, const std::filesystem::path& prog,
                       const std::vector<std::string>& args, ProcOptions opt = {});

// The AppArmor label this process runs under, or empty when it is unconfined
// or the kernel has no AppArmor to ask.
std::string confinement();

// As above, but replaces this process.
[[noreturn]] void exec(const Env& e, const std::filesystem::path& prog,
                       const std::vector<std::string>& args);

}  // namespace kg::rt
