// The gamepad helper's command line, spelled in one place.
//
// A session starts the helper beside a game by re-executing KRETRO_APP - which
// is kretro or a player - as `<app> input --display D --pid N [--game ID]
// [--no-pause]`. The session writes that line and both programs read it, so it
// is a contract across processes: input_helper_argv and parse_input_helper are
// the two halves of it and change together or not at all.
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace kg::session {

struct InputHelperArgs {
  std::string display;  // the game's X display, ":N"
  pid_t pid = 0;        // the game's process
  std::string game;     // whose bindings to read; empty for none
  bool pause = true;    // pause the game when its window loses focus

  bool valid() const { return !display.empty() && pid > 0; }
};

// {"input", "--display", D, "--pid", N}, then "--game" G when there is a game,
// then "--no-pause" when it should not pause.
std::vector<std::string> input_helper_argv(const InputHelperArgs& a);

// The words after "input". Unknown words are ignored, --pid is read with atoi,
// and a flag in last place with no value after it is ignored.
InputHelperArgs parse_input_helper(const std::vector<std::string>& args_after_input);

}  // namespace kg::session
