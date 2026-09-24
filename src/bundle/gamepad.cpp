#include "gamepad.h"

namespace kg::bundle {

std::string format_gamepad(const std::map<std::string, std::string>& binds) {
  std::string out;
  for (const auto& [button, key] : binds) {
    if (button.empty() || key.empty()) continue;
    out += button + "=";
    out += key + "\n";
  }
  return out;
}

std::map<std::string, std::string> parse_gamepad(std::string_view text) {
  std::map<std::string, std::string> out;
  auto trim = [](std::string_view v) {
    size_t a = v.find_first_not_of(" \t\r\n");
    size_t b = v.find_last_not_of(" \t\r\n");
    return a == std::string_view::npos ? std::string() : std::string(v.substr(a, b - a + 1));
  };
  size_t start = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i < text.size() && text[i] != ',' && text[i] != ';' && text[i] != '\n') continue;
    std::string_view item = text.substr(start, i - start);
    start = i + 1;
    size_t eq = item.find('=');
    if (eq == std::string_view::npos) continue;
    std::string k = trim(item.substr(0, eq)), v = trim(item.substr(eq + 1));
    if (!k.empty() && !v.empty()) out[k] = v;
  }
  return out;
}

}  // namespace kg::bundle
