#include "cue.h"

#include <cctype>
#include <sstream>
#include <stdexcept>

#include "../util/text.h"

namespace kg::disc {
namespace {

// A cue sheet quotes any filename with a space in it, and most rippers quote
// unconditionally. Take the quoted span when there is one, the first word
// otherwise.
std::string quoted_or_first_word(const std::string& rest) {
  size_t q = rest.find('"');
  if (q != std::string::npos) {
    size_t e = rest.find('"', q + 1);
    if (e == std::string::npos) throw std::runtime_error("cue: unterminated quote");
    return rest.substr(q + 1, e - q - 1);
  }
  std::istringstream is(rest);
  std::string w;
  is >> w;
  if (w.empty()) throw std::runtime_error("cue: FILE with no filename");
  return w;
}

TrackMode mode_from(const std::string& s) {
  std::string u = to_upper(s);
  if (u == "AUDIO") return TrackMode::Audio;
  if (u == "MODE1/2048") return TrackMode::Mode1_2048;
  if (u == "MODE1/2352") return TrackMode::Mode1_2352;
  if (u == "MODE2/2352") return TrackMode::Mode2_2352;
  if (u == "MODE2/2336") return TrackMode::Mode2_2336;
  throw std::runtime_error("cue: unsupported track mode " + s);
}

}  // namespace

uint64_t msf_to_sectors(std::string_view msf) {
  int part[3] = {0, 0, 0};
  size_t at = 0;
  for (int i = 0; i < 3; ++i) {
    if (at >= msf.size()) throw std::runtime_error("cue: malformed MSF");
    size_t start = at;
    while (at < msf.size() && std::isdigit(static_cast<unsigned char>(msf[at]))) ++at;
    if (at == start) throw std::runtime_error("cue: malformed MSF");
    part[i] = std::stoi(std::string(msf.substr(start, at - start)));
    if (i < 2) {
      if (at >= msf.size() || msf[at] != ':') throw std::runtime_error("cue: malformed MSF");
      ++at;
    }
  }
  if (at != msf.size()) throw std::runtime_error("cue: trailing text in MSF");
  return uint64_t(part[0]) * 60u * 75u + uint64_t(part[1]) * 75u + uint64_t(part[2]);
}

uint32_t sector_bytes(TrackMode m) {
  switch (m) {
    case TrackMode::Mode1_2048: return 2048;
    case TrackMode::Mode2_2336: return 2336;
    default: return 2352;
  }
}

uint32_t payload_bytes(TrackMode) { return 2048; }

uint32_t payload_offset(TrackMode m) {
  switch (m) {
    case TrackMode::Mode1_2048: return 0;
    case TrackMode::Mode1_2352: return 16;
    case TrackMode::Mode2_2352: return 24;
    case TrackMode::Mode2_2336: return 8;
    case TrackMode::Audio: return 0;
  }
  return 0;
}

CueSheet parse_cue(std::string_view text) {
  CueSheet c;
  std::string current_file;
  std::istringstream is{std::string(text)};
  std::string line;
  while (std::getline(is, line)) {
    std::string t = trim(line);
    if (t.empty()) continue;
    std::istringstream ls(t);
    std::string word;
    ls >> word;
    std::string key = to_upper(word);
    std::string rest = trim(t.substr(word.size()));

    if (key == "FILE") {
      current_file = quoted_or_first_word(rest);
    } else if (key == "TRACK") {
      if (current_file.empty()) throw std::runtime_error("cue: TRACK before any FILE");
      std::istringstream rs(rest);
      int num = 0;
      std::string mode;
      rs >> num >> mode;
      if (mode.empty()) throw std::runtime_error("cue: TRACK with no mode");
      CueTrack tr;
      tr.number = num;
      tr.mode = mode_from(mode);
      tr.file = current_file;
      c.tracks.push_back(tr);
    } else if (key == "INDEX") {
      if (c.tracks.empty()) throw std::runtime_error("cue: INDEX before any TRACK");
      std::istringstream rs(rest);
      int idx = 0;
      std::string msf;
      rs >> idx >> msf;
      // INDEX 00 is the pregap; INDEX 01 is where the track's content starts.
      if (idx == 1) c.tracks.back().start_sector = msf_to_sectors(msf);
    }
    // PREGAP, POSTGAP, FLAGS, REM, CATALOG, PERFORMER and TITLE carry nothing
    // this layer needs. PREGAP in particular describes silence that is not in
    // the file, so ignoring it is correct.
  }
  if (c.tracks.empty()) throw std::runtime_error("cue: no tracks");
  return c;
}

void resolve_lengths(CueSheet& c, const std::function<uint64_t(const std::string&)>& file_size) {
  for (size_t i = 0; i < c.tracks.size(); ++i) {
    CueTrack& t = c.tracks[i];
    uint64_t sz = sector_bytes(t.mode);
    bool shares_file = i + 1 < c.tracks.size() && c.tracks[i + 1].file == t.file;
    if (shares_file) {
      t.sector_count = c.tracks[i + 1].start_sector - t.start_sector;
    } else {
      uint64_t bytes = file_size(t.file);
      uint64_t total = bytes / sz;
      t.sector_count = total > t.start_sector ? total - t.start_sector : 0;
    }
  }
}

}  // namespace kg::disc
