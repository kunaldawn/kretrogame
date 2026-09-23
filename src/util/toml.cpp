#include "toml.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace kg {
namespace {

std::string trim(std::string_view s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string_view::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(a, b - a + 1));
}

// Strips a quoted string, honouring the handful of escapes a game manifest
// could plausibly contain.
std::string unquote(const std::string& in, const std::string& origin, int line) {
  if (in.size() < 2) throw std::runtime_error(origin + ":" + std::to_string(line) + ": bad string");
  char q = in.front();
  if ((q != '"' && q != '\'') || in.back() != q) {
    throw std::runtime_error(origin + ":" + std::to_string(line) + ": unterminated string");
  }
  std::string body = in.substr(1, in.size() - 2);
  if (q == '\'') return body;  // literal strings take no escapes

  std::string out;
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] != '\\' || i + 1 == body.size()) {
      out.push_back(body[i]);
      continue;
    }
    switch (body[++i]) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      default: out.push_back(body[i]); break;
    }
  }
  return out;
}

// Splits an inline array body on commas that are not inside quotes.
std::vector<std::string> split_items(const std::string& body) {
  std::vector<std::string> out;
  std::string cur;
  char quote = 0;
  for (char c : body) {
    if (quote) {
      cur.push_back(c);
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      cur.push_back(c);
    } else if (c == ',') {
      out.push_back(trim(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!trim(cur).empty()) out.push_back(trim(cur));
  return out;
}

}  // namespace

Toml Toml::parse(std::string_view text, const std::string& origin) {
  Toml t;
  std::string section;
  std::istringstream in{std::string(text)};
  std::string raw;
  int line = 0;

  while (std::getline(in, raw)) {
    ++line;
    // A '#' inside a quoted value is not a comment.
    std::string s;
    char quote = 0;
    for (char c : raw) {
      if (!quote && c == '#') break;
      if (quote && c == quote) quote = 0;
      else if (!quote && (c == '"' || c == '\'')) quote = c;
      s.push_back(c);
    }
    s = trim(s);
    if (s.empty()) continue;

    if (s.front() == '[') {
      if (s.back() != ']') throw std::runtime_error(origin + ":" + std::to_string(line) + ": bad section header");
      section = trim(s.substr(1, s.size() - 2));
      continue;
    }

    size_t eq = s.find('=');
    if (eq == std::string::npos) {
      throw std::runtime_error(origin + ":" + std::to_string(line) + ": expected key = value");
    }
    std::string key = trim(s.substr(0, eq));
    std::string val = trim(s.substr(eq + 1));
    if (key.empty()) throw std::runtime_error(origin + ":" + std::to_string(line) + ": empty key");
    if (!section.empty()) key = section + "." + key;

    Value v;
    if (!val.empty() && val.front() == '[') {
      // An array may run over several lines; keep reading until the brackets
      // balance outside of quotes.
      int depth = 0;
      char q = 0;
      auto scan = [&](const std::string& chunk) {
        for (char c : chunk) {
          if (q) { if (c == q) q = 0; continue; }
          if (c == '"' || c == '\'') q = c;
          else if (c == '[') ++depth;
          else if (c == ']') --depth;
        }
      };
      scan(val);
      while (depth > 0) {
        std::string more;
        if (!std::getline(in, more)) {
          throw std::runtime_error(origin + ":" + std::to_string(line) + ": unterminated array");
        }
        ++line;
        size_t hash = more.find('#');
        if (hash != std::string::npos && more.find('"') == std::string::npos) more = more.substr(0, hash);
        scan(more);
        val += " " + trim(more);
      }
      if (val.back() != ']') {
        throw std::runtime_error(origin + ":" + std::to_string(line) + ": malformed array");
      }
      v.t = Value::T::Array;
      for (const std::string& item : split_items(val.substr(1, val.size() - 2))) {
        v.a.push_back(unquote(item, origin, line));
      }
    } else if (!val.empty() && (val.front() == '"' || val.front() == '\'')) {
      v.t = Value::T::Str;
      v.s = unquote(val, origin, line);
    } else if (val == "true" || val == "false") {
      v.t = Value::T::Bool;
      v.b = (val == "true");
    } else {
      v.t = Value::T::Int;
      try {
        size_t used = 0;
        v.i = std::stoll(val, &used);
        if (used != val.size()) throw std::invalid_argument("trailing");
      } catch (const std::exception&) {
        throw std::runtime_error(origin + ":" + std::to_string(line) + ": cannot read value '" + val + "'");
      }
    }
    t.values_[key] = std::move(v);
  }
  return t;
}

Toml Toml::parse_file(const std::filesystem::path& p) {
  std::ifstream f(p);
  if (!f) throw std::runtime_error("cannot read " + p.string());
  std::stringstream ss;
  ss << f.rdbuf();
  return parse(ss.str(), p.filename().string());
}

bool Toml::has(const std::string& key) const { return values_.count(key) != 0; }

std::string Toml::str(const std::string& key, const std::string& def) const {
  auto it = values_.find(key);
  return (it != values_.end() && it->second.t == Value::T::Str) ? it->second.s : def;
}

int64_t Toml::integer(const std::string& key, int64_t def) const {
  auto it = values_.find(key);
  return (it != values_.end() && it->second.t == Value::T::Int) ? it->second.i : def;
}

bool Toml::boolean(const std::string& key, bool def) const {
  auto it = values_.find(key);
  return (it != values_.end() && it->second.t == Value::T::Bool) ? it->second.b : def;
}

std::vector<std::string> Toml::array(const std::string& key) const {
  auto it = values_.find(key);
  if (it == values_.end() || it->second.t != Value::T::Array) return {};
  return it->second.a;
}

}  // namespace kg
