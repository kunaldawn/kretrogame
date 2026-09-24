// A small TOML reader for the subset the game manifests use: sections, string,
// integer and boolean values, and arrays of strings. Not a general TOML
// implementation, and it says so when it meets something it does not know.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace kg {

class Toml {
 public:
  static Toml parse(std::string_view text, const std::string& origin = "<memory>");
  static Toml parse_file(const std::filesystem::path& p);

  // Keys are dotted: "source.method", "run.width".
  bool has(const std::string& key) const;
  std::string str(const std::string& key, const std::string& def = "") const;
  int64_t integer(const std::string& key, int64_t def = 0) const;
  bool boolean(const std::string& key, bool def = false) const;
  std::vector<std::string> array(const std::string& key) const;

 private:
  struct Value {
    enum class T { Str, Int, Bool, Array } t = T::Str;
    std::string s;
    int64_t i = 0;
    bool b = false;
    std::vector<std::string> a;
  };
  std::map<std::string, Value> values_;
};

// A TOML basic string: quoted, with its backslashes and quotes escaped.
std::string toml_string(const std::string& s);

}  // namespace kg
