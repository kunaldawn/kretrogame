#include "audio.h"

#include <fstream>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace kg::disc {
namespace {

void put32(std::ostream& o, uint32_t v) {
  char b[4] = {char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff),
               char((v >> 24) & 0xff)};
  o.write(b, 4);
}
void put16(std::ostream& o, uint16_t v) {
  char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
  o.write(b, 2);
}

constexpr uint32_t kRate = 44100;
constexpr uint16_t kChannels = 2;
constexpr uint16_t kBits = 16;

}  // namespace

void write_wav_header(std::ostream& out, uint64_t pcm_bytes) {
  const uint32_t byte_rate = kRate * kChannels * (kBits / 8);
  const uint16_t block_align = kChannels * (kBits / 8);
  out.write("RIFF", 4);
  put32(out, static_cast<uint32_t>(36 + pcm_bytes));
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  put32(out, 16);
  put16(out, 1);  // PCM
  put16(out, kChannels);
  put32(out, kRate);
  put32(out, byte_rate);
  put16(out, block_align);
  put16(out, kBits);
  out.write("data", 4);
  put32(out, static_cast<uint32_t>(pcm_bytes));
}

void rip_audio_track(const fs::path& track_file, uint64_t byte_offset, uint64_t sector_count,
                     const fs::path& out_wav) {
  const uint64_t bytes = sector_count * 2352ull;
  std::ifstream in(track_file, std::ios::binary);
  if (!in) throw std::runtime_error("disc: cannot open " + track_file.string());
  in.seekg(static_cast<std::streamoff>(byte_offset));
  if (!in) throw std::runtime_error("disc: cannot seek to the audio track");

  std::ofstream out(out_wav, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("disc: cannot create " + out_wav.string());
  write_wav_header(out, bytes);

  std::vector<char> buf(1u << 20);
  uint64_t left = bytes;
  while (left > 0) {
    std::streamsize want = static_cast<std::streamsize>(left < buf.size() ? left : buf.size());
    in.read(buf.data(), want);
    if (in.gcount() != want) {
      throw std::runtime_error("disc: audio track is shorter than the cue sheet claims");
    }
    out.write(buf.data(), want);
    left -= static_cast<uint64_t>(want);
  }
}

bool encode_flac(const rt::Env& e, const fs::path& wav, const fs::path& out_flac, std::string* err) {
  fs::path flac = rt::which(e, "flac");
  if (flac.empty()) {
    if (err) *err = "the runtime has no flac";
    return false;
  }
  auto r = rt::run(e, flac, {"--totally-silent", "-f", "-8", "-o", out_flac.string(), wav.string()});
  if (!r.ok()) {
    if (err) *err = r.out;
    return false;
  }
  return true;
}

}  // namespace kg::disc
