#include "registry.h"
#include "../util/hash.h"
#include "../util/text.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace kg::wine {
namespace {

// Wine escapes backslashes in both key lines and string data.
std::string unescape(std::string_view s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      ++i;
      switch (s[i]) {
        case 'n': o.push_back('\n'); break;
        case 'r': o.push_back('\r'); break;
        case '0': o.push_back('\0'); break;
        default: o.push_back(s[i]);
      }
    } else {
      o.push_back(s[i]);
    }
  }
  return o;
}

std::string escape(std::string_view s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') o.push_back('\\');
    o.push_back(c);
  }
  return o;
}

// The bytes between the outer quotes of a .reg right-hand side, unescaped. The
// quotes are punctuation, not data.
std::string dequote(std::string_view rhs) {
  if (rhs.size() < 2 || rhs.front() != '"') return unescape(rhs);
  size_t close = rhs.rfind('"');
  if (close == 0) return {};
  return unescape(rhs.substr(1, close - 1));
}

// UTF-8 in, UTF-16LE out. REGEDIT4 has no syntax for an expand-string or a
// multi-string other than raw hex, and raw hex means the wide bytes Windows
// stores. Malformed input is read as Latin-1 rather than dropped: a registry
// value is data we are copying, not text we are interpreting.
std::vector<uint8_t> to_utf16le(std::string_view s) {
  std::vector<uint8_t> out;
  auto put = [&out](uint32_t u) {
    out.push_back(static_cast<uint8_t>(u & 0xff));
    out.push_back(static_cast<uint8_t>((u >> 8) & 0xff));
  };
  for (size_t i = 0; i < s.size();) {
    uint8_t c = static_cast<uint8_t>(s[i]);
    uint32_t cp = c;
    size_t extra = 0;
    if (c >= 0xf0) { cp = c & 0x07u; extra = 3; }
    else if (c >= 0xe0) { cp = c & 0x0fu; extra = 2; }
    else if (c >= 0xc0) { cp = c & 0x1fu; extra = 1; }
    else if (c >= 0x80) { extra = 0; }  // a stray continuation byte
    bool ok = extra > 0 && i + extra < s.size();
    for (size_t k = 1; ok && k <= extra; ++k) {
      uint8_t n = static_cast<uint8_t>(s[i + k]);
      if ((n & 0xc0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (n & 0x3fu);
    }
    if (!ok) { put(c); ++i; continue; }
    i += extra + 1;
    if (cp > 0x10ffff) cp = 0xfffd;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      put(0xd800 + (cp >> 10));
      put(0xdc00 + (cp & 0x3ff));
    } else {
      put(cp);
    }
  }
  return out;
}

std::string hex_bytes(const std::vector<uint8_t>& b) {
  static const char* digits = "0123456789abcdef";
  std::string o;
  o.reserve(b.size() * 3);
  for (size_t i = 0; i < b.size(); ++i) {
    if (i) o.push_back(',');
    o.push_back(digits[b[i] >> 4]);
    o.push_back(digits[b[i] & 0xf]);
  }
  return o;
}

// A string written as REGEDIT4 hex of the given type. Wine drops the value's
// terminating NUL when it dumps a str(n), so we put exactly one back - which
// for a multi-string is the second NUL that ends the list.
std::string hex_string_value(int type, std::string_view data) {
  std::vector<uint8_t> b = to_utf16le(data);
  b.push_back(0);
  b.push_back(0);
  return "hex(" + std::to_string(type) + "):" + hex_bytes(b);
}

}  // namespace

std::vector<RegValue> parse_reg(std::string_view text, std::string_view hive) {
  std::vector<RegValue> out;
  std::istringstream is{std::string(text)};
  std::string line;
  std::string key;
  while (std::getline(is, line)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    if (t.rfind("WINE REGISTRY", 0) == 0) continue;

    if (t[0] == '[') {
      size_t close = t.rfind(']');
      if (close == std::string::npos) continue;
      key = unescape(t.substr(1, close - 1));
      continue;
    }
    if (key.empty()) continue;

    // Wine wraps a long value - a CD key blob, an icon, anything binary - by
    // ending the line with a backslash and continuing on the next. Read the
    // whole value before looking at it: a line-at-a-time parser keeps the
    // trailing backslash as data and then throws the rest of the bytes away.
    while (!t.empty() && t.back() == '\\') {
      t.pop_back();
      std::string cont;
      if (!std::getline(is, cont)) break;
      t += trim(cont);
    }

    size_t eq;
    RegValue v;
    v.key = key;
    v.hive = std::string(hive);
    if (t[0] == '@') {
      v.name = "@";
      eq = t.find('=');
    } else if (t[0] == '"') {
      size_t close = t.find('"', 1);
      if (close == std::string::npos) continue;
      v.name = unescape(t.substr(1, close - 1));
      eq = t.find('=', close);
    } else {
      continue;
    }
    if (eq == std::string::npos) continue;
    std::string rhs = trim(t.substr(eq + 1));
    if (!rhs.empty() && rhs[0] == '"') {
      v.type = "sz";
      v.data = dequote(rhs);
    } else {
      size_t colon = rhs.find(':');
      if (colon == std::string::npos) {
        v.type = "sz";
        v.data = rhs;
      } else {
        std::string tok = rhs.substr(0, colon);
        std::string rest = rhs.substr(colon + 1);
        // str(2) and str(7) are Wine's own spelling of REG_EXPAND_SZ and
        // REG_MULTI_SZ, and their data is a quoted string like any other
        // string's. Name them for what they are here; nothing downstream
        // should have to know Wine's private vocabulary.
        if (tok == "str" || tok == "str(1)") {
          v.type = "sz";
          v.data = dequote(rest);
        } else if (tok == "str(2)") {
          v.type = "expand_sz";
          v.data = dequote(rest);
        } else if (tok == "str(7)") {
          v.type = "multi_sz";
          v.data = dequote(rest);
        } else {
          v.type = tok;
          v.data = rest;
        }
      }
    }
    out.push_back(std::move(v));
  }
  return out;
}

std::vector<RegValue> diff_reg(const std::vector<RegValue>& before,
                               const std::vector<RegValue>& after) {
  auto same_slot = [](const RegValue& a, const RegValue& b) {
    return a.hive == b.hive && a.key == b.key && a.name == b.name;
  };
  std::vector<RegValue> out;
  for (const RegValue& a : after) {
    const RegValue* prev = nullptr;
    for (const RegValue& b : before) {
      if (same_slot(a, b)) { prev = &b; break; }
    }
    if (prev && prev->data == a.data && prev->type == a.type) continue;
    out.push_back(a);
  }
  std::sort(out.begin(), out.end(), [](const RegValue& a, const RegValue& b) {
    if (a.hive != b.hive) return a.hive < b.hive;
    if (a.key != b.key) return a.key < b.key;
    return a.name < b.name;
  });
  return out;
}

std::string to_reg_fragment(const std::vector<RegValue>& v) {
  std::vector<RegValue> s = v;
  std::sort(s.begin(), s.end(), [](const RegValue& a, const RegValue& b) {
    if (a.hive != b.hive) return a.hive < b.hive;
    if (a.key != b.key) return a.key < b.key;
    return a.name < b.name;
  });

  std::ostringstream o;
  o << "REGEDIT4\n";
  std::string current_hive;
  std::string current;
  bool first = true;
  for (const RegValue& r : s) {
    std::string hive = r.hive.empty() ? std::string(kHiveCurrentUser) : r.hive;
    if (first || r.key != current || hive != current_hive) {
      current_hive = hive;
      current = r.key;
      first = false;
      o << "\n[" << current_hive << "\\" << current << "]\n";
    }
    if (r.name == "@") o << "@=";
    else o << "\"" << escape(r.name) << "\"=";
    if (r.type == "sz") o << "\"" << escape(r.data) << "\"\n";
    else if (r.type == "expand_sz") o << hex_string_value(2, r.data) << "\n";
    else if (r.type == "multi_sz") o << hex_string_value(7, r.data) << "\n";
    else o << r.type << ":" << r.data << "\n";
  }
  return o.str();
}

std::vector<RegValue> parse_fragment(std::string_view text) {
  std::vector<RegValue> out;
  std::string hive, key;
  size_t at = 0;
  auto quoted = [](std::string_view s, size_t& i) {
    // s[i] is the opening quote; returns the unescaped contents and leaves i
    // after the closing one.
    std::string o;
    for (++i; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) { o.push_back(s[++i]); continue; }
      if (s[i] == '"') { ++i; break; }
      o.push_back(s[i]);
    }
    return o;
  };
  while (at <= text.size()) {
    size_t nl = text.find('\n', at);
    std::string_view line = text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
    at = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      std::string_view inner = line.substr(1, line.size() - 2);
      size_t bs = inner.find('\\');
      hive = std::string(inner.substr(0, bs));
      key = bs == std::string_view::npos ? std::string() : std::string(inner.substr(bs + 1));
      continue;
    }
    if (hive.empty()) continue;  // REGEDIT4, or anything before the first key
    RegValue v;
    v.hive = hive;
    v.key = key;
    size_t i = 0;
    if (line.front() == '@') {
      v.name = "@";
      i = 1;
    } else if (line.front() == '"') {
      v.name = quoted(line, i);
    } else {
      continue;
    }
    if (i >= line.size() || line[i] != '=') continue;
    ++i;
    if (i < line.size() && line[i] == '"') {
      v.type = "sz";
      v.data = quoted(line, i);
    } else {
      std::string_view rhs = line.substr(i);
      size_t colon = rhs.find(':');
      if (colon == std::string_view::npos) continue;
      v.type = std::string(rhs.substr(0, colon));
      v.data = std::string(rhs.substr(colon + 1));
    }
    out.push_back(std::move(v));
  }
  return out;
}

bool is_serial_value(const RegValue& v) {
  if (v.data.empty()) return false;
  // A dword is a number the installer or Wine computed - a volume serial, a
  // flag - and never a key somebody typed.
  if (v.type == "dword") return false;

  std::string n;
  for (unsigned char c : v.name) {
    if (std::isalnum(c)) n.push_back(static_cast<char>(std::tolower(c)));
  }
  if (n.empty()) return false;

  // Names that are a key by themselves, and names a key hides inside:
  // "CDKey", "CD Key", "Serial Number", "ProductKey", "RegistrationNumber".
  static const char* const kWhole[] = {
      "serial", "key", "cdkey", "regnum", "registration", "license", "licence", "productid",
  };
  static const char* const kInside[] = {
      "cdkey", "serialnumber", "serialno", "productkey", "productcode", "licensekey",
      "licencekey", "registrationnumber", "registrationcode", "regcode", "unlockcode",
      "keycode", "activationkey", "installationid",
  };
  bool named = false;
  for (const char* w : kWhole) {
    if (n == w) { named = true; break; }
  }
  if (!named) {
    for (const char* w : kInside) {
      if (n.find(w) != std::string::npos) { named = true; break; }
    }
  }
  if (!named) return false;

  // A key written as bytes is still a key, and the name is the whole of the
  // evidence for those: "00,01,02,..." has no shape of its own.
  if (v.type.rfind("hex", 0) == 0) return true;

  // Otherwise the data has to look like something a person typed off a sleeve.
  // A path, a filename or a five-letter word under a name like "Key" is a value
  // the game wants and this must not take.
  if (v.data.size() < 8) return false;
  for (unsigned char c : v.data) {
    if (std::isalnum(c) || c == '-' || c == ' ' || c == '_') continue;
    return false;
  }
  return true;
}

std::vector<RegValue> without_serials(const std::vector<RegValue>& v,
                                      std::vector<std::string>* dropped) {
  std::vector<RegValue> out;
  out.reserve(v.size());
  for (const RegValue& r : v) {
    if (is_serial_value(r)) {
      if (dropped) dropped->push_back(r.key + "\\" + r.name);
      continue;
    }
    out.push_back(r);
  }
  return out;
}

std::vector<RegValue> snapshot_prefix(const fs::path& prefix) {
  std::vector<RegValue> out;
  // Which file a value lives in is which hive it belongs to. system.reg is
  // HKEY_LOCAL_MACHINE, where an installer puts the machine-wide things a game
  // looks up at startup - an InstallPath, often - and user.reg is
  // HKEY_CURRENT_USER. Reading both into one hive-less list would restore the
  // first set under the second's root, which is the same as not restoring it.
  const std::pair<const char*, std::string_view> files[] = {
      {"system.reg", kHiveLocalMachine},
      {"user.reg", kHiveCurrentUser},
  };
  for (const auto& [name, hive] : files) {
    std::ifstream f(prefix / name);
    if (!f) continue;
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<RegValue> v = parse_reg(ss.str(), hive);
    out.insert(out.end(), v.begin(), v.end());
  }
  return out;
}

void apply_fragment(const rt::Env& e, const fs::path& prefix, const std::string& fragment) {
  if (fragment.empty()) return;
  // Inside drive_c and named as C:, because regedit reads a Windows path: a
  // Unix one only resolves through a Z: drive mapping /, and a player's
  // prefix has none - it would import nothing and say so only in its exit
  // status.
  std::error_code mk;
  fs::create_directories(prefix / "drive_c", mk);
  fs::path tmp = prefix / "drive_c" / ".kretro-restore.reg";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) throw std::runtime_error("registry: cannot write the fragment");
    f << fragment;
  }
  fs::path wine = rt::which(e, "wine");
  if (wine.empty()) throw std::runtime_error("registry: the runtime has no wine");
  rt::Env we = e;
  we.set("WINEPREFIX", prefix.string());
  ProcResult r = rt::run(we, wine, {"regedit", "/S", "C:\\.kretro-restore.reg"});
  std::error_code ec;
  fs::remove(tmp, ec);
  if (!r.ok()) throw std::runtime_error("registry: could not import the fragment\n" + r.out);
}

namespace {
fs::path marker_file(const fs::path& prefix) { return prefix / ".kretro-registry"; }
}  // namespace

bool registry_marker_matches(const fs::path& prefix, const std::string& fragment) {
  std::ifstream f(marker_file(prefix));
  if (!f) return false;
  std::string got;
  std::getline(f, got);
  return got == to_hex(hash_string(fragment));
}

void write_registry_marker(const fs::path& prefix, const std::string& fragment) {
  std::error_code ec;
  fs::create_directories(prefix, ec);
  std::ofstream f(marker_file(prefix), std::ios::trunc);
  if (!f) throw std::runtime_error("registry: cannot write the marker");
  f << to_hex(hash_string(fragment)) << "\n";
}

}  // namespace kg::wine
