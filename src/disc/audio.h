// CD audio.
//
// Many mixed-mode discs carry the game's soundtrack as audio tracks after the
// data track, and a data-track rip throws it away. We keep it: raw CD-DA is
// already 44.1 kHz stereo 16-bit PCM, so ripping is a copy with a WAV header
// in front, and FLAC makes it small enough to live inside the pack.
//
// Be clear about what this does not do. Wine's MCI CD-audio device reads a real
// /dev/sr* and we have none, so ripped tracks restore in-game music only for
// engines that read music from disk (music/*.ogg, say). For everything else
// the tracks are preserved, and the jukebox is the honest offer.
#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

#include "../rt/env.h"

namespace kg::disc {

// A 44-byte canonical WAV header for `pcm_bytes` of CD-DA.
void write_wav_header(std::ostream& out, uint64_t pcm_bytes);

// Copies `sector_count` sectors of 2352 bytes from `byte_offset` into a WAV.
void rip_audio_track(const std::filesystem::path& track_file, uint64_t byte_offset,
                     uint64_t sector_count, const std::filesystem::path& out_wav);

// Encodes with the bundled flac. Returns false and fills `err` if flac is
// missing or fails; the caller keeps the WAV rather than losing the audio.
bool encode_flac(const rt::Env& e, const std::filesystem::path& wav,
                 const std::filesystem::path& out_flac, std::string* err);

}  // namespace kg::disc
