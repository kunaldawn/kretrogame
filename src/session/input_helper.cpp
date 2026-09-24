#include "input_helper.h"

#include <cstdlib>

namespace kg::session {

std::vector<std::string> input_helper_argv(const InputHelperArgs& a) {
  std::vector<std::string> argv = {"input", "--display", a.display, "--pid", std::to_string(a.pid)};
  if (!a.game.empty()) {
    argv.push_back("--game");
    argv.push_back(a.game);
  }
  if (!a.pause) argv.push_back("--no-pause");
  return argv;
}

InputHelperArgs parse_input_helper(const std::vector<std::string>& args_after_input) {
  const std::vector<std::string>& a = args_after_input;
  InputHelperArgs out;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] == "--display" && i + 1 < a.size()) out.display = a[++i];
    else if (a[i] == "--pid" && i + 1 < a.size()) out.pid = std::atoi(a[++i].c_str());
    else if (a[i] == "--game" && i + 1 < a.size()) out.game = a[++i];
    else if (a[i] == "--no-pause") out.pause = false;
  }
  return out;
}

}  // namespace kg::session
