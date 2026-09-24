// The command table: one row per command, in the order the help lists them.
// Internal to src/cli.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../rt/env.h"

namespace kg::cli {

// What a command needs before it runs. GpuEnv probes the GPU first, which
// costs a directory walk, so only the commands that draw or start Wine ask
// for it.
// clang-format off: it would put each enumerator on a line of its own.
enum class Needs { Env, GpuEnv };
// clang-format on

// How many arguments, after the command's own name, it takes. Outside this
// the help is printed instead - after the environment is set up, as it always
// was.
struct Arity {
  size_t min = 0, max = SIZE_MAX;
};

using Handler = int (*)(const rt::Env& env, std::vector<std::string>& args);

struct Command {
  std::string_view name;
  Needs needs;
  Arity arity;
  Handler run;
  // This command's lines of the help, exactly as printed. Empty hides it.
  std::string_view help;
};

// Every command, in the order of the help.
std::span<const Command> commands();

}  // namespace kg::cli
