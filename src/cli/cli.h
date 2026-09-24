// kretro's command line: everything but the shelf, which is what running the
// binary with no arguments opens.
#pragma once

#include <string>
#include <vector>

namespace kg::cli {

// Runs one command. args is the command line after the program name, and is
// never empty: no arguments means the shelf, which main opens itself. Returns
// the process exit status.
int run(std::vector<std::string> args);

// Prints the help to stderr and returns 2, the status of a command line that
// was not understood.
int usage();

}  // namespace kg::cli
