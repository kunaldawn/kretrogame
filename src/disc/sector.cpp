#include "sector.h"

#include <algorithm>
#include <fstream>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace kg::disc {

void extract_data_track(std::istream& in, uint64_t in_off, TrackMode mode,
                        uint64_t sector_count, std::ostream& out) {
  const uint32_t ss = sector_bytes(mode);
  const uint32_t po = payload_offset(mode);
  const uint32_t pb = mode == TrackMode::Mode1_2048 ? ss : payload_bytes(mode);

  in.seekg(static_cast<std::streamoff>(in_off));
  if (!in) throw std::runtime_error("disc: cannot seek to the start of the track");

  // A megabyte at a time: large enough that a 700 MB track is a few hundred
  // reads, small enough to stay out of the way.
  const uint64_t batch = std::max<uint64_t>(1, (1u << 20) / ss);
  std::vector<char> buf(static_cast<size_t>(batch) * ss);

  uint64_t left = sector_count;
  while (left > 0) {
    uint64_t n = left < batch ? left : batch;
    in.read(buf.data(), static_cast<std::streamsize>(n * ss));
    if (static_cast<uint64_t>(in.gcount()) != n * ss) {
      throw std::runtime_error("disc: track is shorter than its cue sheet claims");
    }
    for (uint64_t s = 0; s < n; ++s) {
      out.write(buf.data() + s * ss + po, pb);
    }
    if (!out) throw std::runtime_error("disc: cannot write the normalised image");
    left -= n;
  }
}

void normalise_to_iso(const std::filesystem::path& track_file, uint64_t byte_offset,
                      TrackMode mode, uint64_t sector_count,
                      const std::filesystem::path& out_iso) {
  std::ifstream in(track_file, std::ios::binary);
  if (!in) throw std::runtime_error("disc: cannot open " + track_file.string());
  std::ofstream out(out_iso, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("disc: cannot create " + out_iso.string());
  extract_data_track(in, byte_offset, mode, sector_count, out);
}

}  // namespace kg::disc
