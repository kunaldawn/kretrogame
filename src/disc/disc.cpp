#include "disc.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "audio.h"
#include "cue.h"
#include "sector.h"

namespace fs = std::filesystem;

namespace kg::disc {

uint32_t volume_serial(const iso::Info& info) {
  // FNV-1a over whatever identity the disc records. Deterministic, cheap, and
  // it does not matter that it is not what the original pressing used - what
  // matters is that it is stable and non-zero for the same disc.
  std::string seed = info.created + "|" + info.volume_id + "|" + info.publisher;
  uint32_t h = 2166136261u;
  for (unsigned char c : seed) {
    h ^= c;
    h *= 16777619u;
  }
  return h == 0 ? 1u : h;
}

std::string set_id_from(std::string_view name) {
  std::string out;
  bool dash = true;  // leading separators are dropped
  for (char c : name) {
    unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) {
      out.push_back(static_cast<char>(std::tolower(u)));
      dash = false;
    } else if (!dash) {
      out.push_back('-');
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

Disc open(const rt::Env& e, const Candidate& c, const fs::path& work, bool rip_audio) {
  std::error_code ec;
  fs::create_directories(work, ec);

  Disc d;
  d.source = c.archive;
  d.members = c.members;

  fs::path inner = materialise(e, c, work);

  if (!c.is_cue) {
    // An .iso is already normalised. A .bin with no cue sheet beside it is raw
    // sectors, and its size gives the format away: 2352 divides it and 2048
    // does not.
    std::string low = inner.filename().string();
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    uint64_t sz = fs::file_size(inner, ec);
    if (low.size() > 4 && low.compare(low.size() - 4, 4, ".bin") == 0 && sz % 2352ull == 0 &&
        sz % 2048ull != 0) {
      fs::path out = work / "disc.iso";
      normalise_to_iso(inner, 0, TrackMode::Mode1_2352, sz / 2352ull, out);
      d.iso = out;
    } else {
      d.iso = inner;
    }
  } else {
    std::ifstream cf(inner);
    std::stringstream ss;
    ss << cf.rdbuf();
    CueSheet sheet = parse_cue(ss.str());
    fs::path dir = inner.parent_path();
    resolve_lengths(sheet, [&](const std::string& f) -> uint64_t {
      std::error_code e2;
      uint64_t sz = fs::file_size(dir / f, e2);
      return e2 ? 0 : sz;
    });

    bool have_data = false;
    for (const CueTrack& t : sheet.tracks) {
      if (t.mode == TrackMode::Audio) continue;
      if (have_data) continue;  // the first data track is the filesystem
      fs::path out = work / "disc.iso";
      normalise_to_iso(dir / t.file, t.start_sector * sector_bytes(t.mode), t.mode,
                       t.sector_count, out);
      d.iso = out;
      have_data = true;
    }
    if (!have_data) throw std::runtime_error("disc: the cue sheet has no data track");

    if (rip_audio) {
      fs::path adir = work / "audio";
      fs::create_directories(adir, ec);
      for (const CueTrack& t : sheet.tracks) {
        if (t.mode != TrackMode::Audio) continue;
        char stem[32];
        std::snprintf(stem, sizeof(stem), "track-%02d", t.number);
        fs::path wav = adir / (std::string(stem) + ".wav");
        rip_audio_track(dir / t.file, t.start_sector * 2352ull, t.sector_count, wav);
        fs::path flac = adir / (std::string(stem) + ".flac");
        std::string err;
        AudioTrack at;
        at.number = t.number;
        at.sector_count = t.sector_count;
        if (encode_flac(e, wav, flac, &err)) {
          fs::remove(wav, ec);
          at.file = flac;
        } else {
          at.file = wav;  // keep the audio rather than lose it
        }
        d.audio.push_back(at);
      }
    }
  }

  d.info = iso::scan(d.iso, false);
  d.label = d.info.volume_id;
  d.serial = volume_serial(d.info);
  return d;
}

DiscSet assemble(std::string set_id, std::string name, std::vector<Disc> discs) {
  DiscSet s;
  s.set_id = std::move(set_id);
  s.name = std::move(name);
  for (Disc& d : discs) {
    bool dup = false;
    for (const Disc& kept : s.discs) {
      if (kept.info.size == d.info.size && kept.info.prefix == d.info.prefix) dup = true;
    }
    if (dup) s.alternates.push_back(std::move(d));
    else s.discs.push_back(std::move(d));
  }
  return s;
}

}  // namespace kg::disc
