#include "serial.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace fs = std::filesystem;

namespace kg::disc {
namespace {

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// Is there a key word within `window` characters either side of `at`?
bool key_word_near(const std::string& low, size_t at, size_t window) {
  size_t from = at > window ? at - window : 0;
  size_t to = std::min(low.size(), at + window);
  std::string span = low.substr(from, to - from);
  return contains(span, "cd key") || contains(span, "cd-key") || contains(span, "cdkey") ||
         contains(span, "serial") || contains(span, "key:") || contains(span, "key ") ||
         contains(span, "key\n") || contains(span, "product key") || contains(span, "reg code");
}

void add(std::vector<KeyCandidate>& out, const std::string& v, std::string_view source) {
  for (const auto& k : out) if (k.value == v) return;
  out.push_back(KeyCandidate{v, std::string(source)});
}

}  // namespace

std::vector<KeyCandidate> extract_keys(std::string_view text, std::string_view source) {
  std::vector<KeyCandidate> out;
  const std::string s(text);
  const std::string low = lower(s);

  // Grouped forms are distinctive enough to stand alone.
  static const std::regex five(R"([A-Z0-9]{5}(?:-[A-Z0-9]{5}){2,4})");
  static const std::regex four(R"([A-Z0-9]{4}(?:-[A-Z0-9]{4}){2,5})");
  for (const std::regex* re : {&five, &four}) {
    for (auto it = std::sregex_iterator(s.begin(), s.end(), *re); it != std::sregex_iterator(); ++it) {
      add(out, it->str(), source);
    }
  }

  // An ungrouped run needs a key word nearby, or every hash in every log
  // becomes a serial.
  static const std::regex run(R"([A-Z0-9]{16,25})");
  for (auto it = std::sregex_iterator(s.begin(), s.end(), run); it != std::sregex_iterator(); ++it) {
    size_t at = static_cast<size_t>(it->position());
    if (!key_word_near(low, at, 120)) continue;
    bool covered = false;
    for (const auto& k : out) if (k.value.find(it->str()) != std::string::npos) covered = true;
    if (!covered) add(out, it->str(), source);
  }
  return out;
}

std::vector<Companion> scan_companions(const fs::path& image) {
  std::vector<Companion> out;
  std::error_code ec;
  fs::path dir = image.parent_path();
  if (dir.empty()) dir = ".";
  for (const auto& de : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    std::string name = de.path().filename().string();
    if (de.path() == image) continue;
    std::string low = lower(name);
    std::string kind;
    if (de.is_directory(ec)) {
      if (low == "crack" || low == "cracks") kind = "crack";
      else if (low == "nocd" || low == "no-cd") kind = "nocd";
    } else {
      if (low.rfind("serial", 0) == 0 || contains(low, "cdkey") || contains(low, "cd-key")) {
        kind = "serial";
      } else if (low.size() > 4 && low.compare(low.size() - 4, 4, ".nfo") == 0) {
        kind = "nfo";
      } else if (low.rfind("readme", 0) == 0 || low.rfind("read me", 0) == 0) {
        kind = "readme";
      }
    }
    if (!kind.empty()) out.push_back(Companion{de.path(), kind});
  }
  std::sort(out.begin(), out.end(),
            [](const Companion& a, const Companion& b) { return a.path < b.path; });
  return out;
}

}  // namespace kg::disc
