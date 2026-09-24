#include "words.h"

#include <cstdio>

namespace kg::gui::wizard_detail {
namespace fs = std::filesystem;

// "three discs" reads better than "3 discs" in a sentence, and a disc set is
// never large enough for the words to run out.
std::string discs_in_words(size_t n) {
  static const char* w[] = {"no", "one", "two", "three", "four", "five", "six", "seven", "eight"};
  std::string s = n < 9 ? std::string(w[n]) : std::to_string(n);
  return s + (n == 1 ? " disc" : " discs");
}

const char* kind_word(install::Source::Kind k) {
  switch (k) {
    case install::Source::Kind::DiscImage: return "disc image";
    case install::Source::Kind::Archive:   return "archive";
    case install::Source::Kind::Directory: return "folder";
    case install::Source::Kind::BareExe:   return "an installer, not a disc";
    case install::Source::Kind::Unreadable: return "unreadable";
  }
  return "";
}

// How step 3 says each method out loud. The list of methods on offer is
// install::methods_for - what these sources can actually run - and this is the
// sentence for each one of them.
const char* method_word(install::Draft::Method m) {
  switch (m) {
    case install::Draft::Method::Installer:    return "run an installer on the disc";
    case install::Draft::Method::Copy:         return "copy the files off the disc";
    case install::Draft::Method::Unzip:        return "extract an archive from the disc";
    case install::Draft::Method::InstallerExe: return "run the installer you dropped";
  }
  return "";
}

// A candidate directory, said out loud. A copy or an unzip has exactly one
// candidate and its directory is the extracted tree itself, which has no name
// to print: where it landed is answered by the method rather than by a path.
std::string where_word(const fs::path& dir) {
  return dir.empty() ? std::string("what came off the disc") : dir.generic_string();
}

void set_buf(char* buf, size_t n, const std::string& v) {
  std::snprintf(buf, n, "%s", v.c_str());
}

}  // namespace kg::gui::wizard_detail
