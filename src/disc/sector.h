// Turning a raw disc track into an ISO9660 image.
//
// A ripper writes 2352 bytes per sector because that is what the drive reads:
// twelve sync bytes, a four-byte header, 2048 bytes of user data, and error
// correction. ISO9660 is only the user data. Stripping the rest gives an image
// byte-identical to what the disc presents as a filesystem - which means every
// later layer, including the existing PVD reader and 7z, can stay ignorant of
// sector formats entirely.
//
// For a MODE1/2048 track this is a copy, so a .iso normalises to itself and its
// fingerprint does not change.
#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>

#include "cue.h"

namespace kg::disc {

// Reads `sector_count` sectors starting at byte `in_off`, writing each
// sector's user data to `out`. Throws std::runtime_error on a short read,
// because a truncated data track is a bad dump and pretending otherwise wastes
// a forty-hour playthrough.
void extract_data_track(std::istream& in, uint64_t in_off, TrackMode mode,
                        uint64_t sector_count, std::ostream& out);

// The same over files.
void normalise_to_iso(const std::filesystem::path& track_file, uint64_t byte_offset,
                      TrackMode mode, uint64_t sector_count,
                      const std::filesystem::path& out_iso);

}  // namespace kg::disc
