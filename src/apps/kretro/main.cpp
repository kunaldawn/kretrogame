// kretro - old games, one binary.
//
// By the time main runs we are already under the runtime's own loader, against
// the runtime's own libraries. Nothing below links anything from the host.
#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "../../cli/cli.h"
#include "../../gpu/probe.h"
#include "../../gui/shelf/shelf.h"
#include "../../rt/env.h"

using namespace kg;

int main(int argc, char** argv) {
  std::vector<std::string> a(argv + 1, argv + argc);

  // No arguments means the shelf. Running the binary is the whole interface;
  // the subcommands are there for when you would rather type.
  if (a.empty()) {
    try {
      gpu::Report gl = gpu::probe();
      gpu::materialize(gl);
      rt::Env e = rt::make(&gl);
      return gui::run(e);
    } catch (const std::exception& ex) {
      std::fprintf(stderr, "kretro: %s\n", ex.what());
      return 1;
    }
  }
  return cli::run(std::move(a));
}
