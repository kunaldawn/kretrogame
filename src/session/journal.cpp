#include "journal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "saves_layout.h"

namespace kg::session {
namespace fs = std::filesystem;

namespace {

std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o.push_back(c);
    }
  }
  return o;
}

std::string json_field(const std::string& src, const std::string& key) {
  std::string needle = "\"" + key + "\":";
  size_t p = src.find(needle);
  if (p == std::string::npos) return "";
  p += needle.size();
  while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
  if (p >= src.size()) return "";
  if (src[p] == '"') {
    ++p;
    std::string out;
    while (p < src.size() && src[p] != '"') {
      if (src[p] == '\\' && p + 1 < src.size()) ++p;
      out.push_back(src[p++]);
    }
    return out;
  }
  size_t end = src.find_first_of(",}\n", p);
  return src.substr(p, end == std::string::npos ? std::string::npos : end - p);
}

}  // namespace

void write_record(const std::string& id, const Record& r) {
  std::error_code ec;
  fs::create_directories(journal_dir(id), ec);
  char name[64];
  std::snprintf(name, sizeof(name), "%010lld.json", static_cast<long long>(r.started));
  std::ofstream f(journal_dir(id) / name);
  f << "{\n"
    << "  \"started\": " << r.started << ",\n"
    << "  \"ended\": " << r.ended << ",\n"
    << "  \"seconds\": " << (r.ended - r.started) << ",\n"
    << "  \"runtime\": \"" << json_escape(r.runtime_id) << "\",\n"
    << "  \"files_written\": " << r.files_written << ",\n"
    << "  \"status\": " << r.status << ",\n"
    << "  \"generation\": \"" << json_escape(r.generation) << "\",\n"
    << "  \"screenshot\": \"" << json_escape(r.screenshot) << "\",\n"
    << "  \"note\": \"" << json_escape(r.note) << "\"\n"
    << "}\n";
}

std::vector<Record> journal(const std::string& id) {
  std::vector<Record> out;
  std::error_code ec;
  std::vector<fs::path> files;
  for (const fs::directory_entry& de : fs::directory_iterator(journal_dir(id), ec)) {
    if (de.path().extension() == ".json") files.push_back(de.path());
  }
  std::sort(files.rbegin(), files.rend());
  for (const fs::path& p : files) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    Record r;
    r.started = std::atoll(json_field(s, "started").c_str());
    r.ended = std::atoll(json_field(s, "ended").c_str());
    r.runtime_id = json_field(s, "runtime");
    r.note = json_field(s, "note");
    r.screenshot = json_field(s, "screenshot");
    r.generation = json_field(s, "generation");
    r.files_written = static_cast<size_t>(std::atoll(json_field(s, "files_written").c_str()));
    r.status = std::atoi(json_field(s, "status").c_str());
    out.push_back(r);
  }
  return out;
}

}  // namespace kg::session
