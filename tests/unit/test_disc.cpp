// Tier 1 unit tests for the disc layer: no GPU, no container, no disc image.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>

#include "disc/cue.h"
#include "disc/audio.h"
#include "disc/container.h"
#include "disc/disc.h"
#include "disc/drive.h"
#include "disc/sector.h"
#include "disc/serial.h"
#include "disc/database.h"
#include "disc/iso.h"
#include "util/hash.h"
#include "support/check.h"

namespace fs = std::filesystem;
using namespace kg;

static void test_msf() {
  // 75 frames to the second, and the two-second pregap every disc carries.
  CHECK_EQ(disc::msf_to_sectors("00:00:00"), 0u);
  CHECK_EQ(disc::msf_to_sectors("00:02:00"), 150u);
  CHECK_EQ(disc::msf_to_sectors("01:00:00"), 4500u);
  CHECK_EQ(disc::msf_to_sectors("12:34:56"), 12u * 60u * 75u + 34u * 75u + 56u);
  CHECK_THROWS(disc::msf_to_sectors("12:34"));
  CHECK_THROWS(disc::msf_to_sectors("aa:bb:cc"));
}

static void test_geometry() {
  CHECK_EQ(disc::sector_bytes(disc::TrackMode::Mode1_2048), 2048u);
  CHECK_EQ(disc::sector_bytes(disc::TrackMode::Mode1_2352), 2352u);
  CHECK_EQ(disc::sector_bytes(disc::TrackMode::Audio), 2352u);
  CHECK_EQ(disc::payload_bytes(disc::TrackMode::Mode1_2352), 2048u);
  CHECK_EQ(disc::payload_bytes(disc::TrackMode::Mode2_2352), 2048u);
  CHECK_EQ(disc::payload_offset(disc::TrackMode::Mode1_2048), 0u);
  CHECK_EQ(disc::payload_offset(disc::TrackMode::Mode1_2352), 16u);
  CHECK_EQ(disc::payload_offset(disc::TrackMode::Mode2_2352), 24u);
}

// Demo-Game's dump: one .bin, a data track and twenty-one audio tracks.
static void test_cue_single_file() {
  const char* text =
      "FILE \"Demo-Game (USA).bin\" BINARY\n"
      "  TRACK 01 MODE1/2352\n"
      "    INDEX 01 00:00:00\n"
      "  TRACK 02 AUDIO\n"
      "    PREGAP 00:02:00\n"
      "    INDEX 01 40:00:00\n"
      "  TRACK 03 AUDIO\n"
      "    INDEX 01 43:20:00\n";
  disc::CueSheet c = disc::parse_cue(text);
  CHECK_EQ(c.tracks.size(), 3u);
  CHECK_EQ(c.tracks[0].number, 1);
  CHECK(c.tracks[0].mode == disc::TrackMode::Mode1_2352);
  CHECK_EQ(c.tracks[0].file, std::string("Demo-Game (USA).bin"));
  CHECK_EQ(c.tracks[0].start_sector, 0u);
  CHECK(c.tracks[1].mode == disc::TrackMode::Audio);
  CHECK_EQ(c.tracks[1].start_sector, 40u * 60u * 75u);
  // Every track names the same file, so lengths come from the next track.
  CHECK_EQ(c.tracks[2].file, std::string("Demo-Game (USA).bin"));

  disc::resolve_lengths(c, [](const std::string&) { return uint64_t(50u * 60u * 75u) * 2352u; });
  CHECK_EQ(c.tracks[0].sector_count, 40u * 60u * 75u);
  CHECK_EQ(c.tracks[1].sector_count, 3u * 60u * 75u + 20u * 75u);
  CHECK_EQ(c.tracks[2].sector_count, 50u * 60u * 75u - (43u * 60u * 75u + 20u * 75u));
}

// Other Story's dump: one .bin per track, each starting at zero.
static void test_cue_multi_file() {
  const char* text =
      "FILE \"Op4 (Track 01).bin\" BINARY\n"
      "  TRACK 01 MODE1/2352\n"
      "    INDEX 01 00:00:00\n"
      "FILE \"Op4 (Track 02).bin\" BINARY\n"
      "  TRACK 02 AUDIO\n"
      "    INDEX 01 00:00:00\n";
  disc::CueSheet c = disc::parse_cue(text);
  CHECK_EQ(c.tracks.size(), 2u);
  CHECK_EQ(c.tracks[0].file, std::string("Op4 (Track 01).bin"));
  CHECK_EQ(c.tracks[1].file, std::string("Op4 (Track 02).bin"));
  CHECK_EQ(c.tracks[1].start_sector, 0u);

  disc::resolve_lengths(c, [](const std::string& f) {
    return f == "Op4 (Track 01).bin" ? uint64_t(100) * 2352u : uint64_t(200) * 2352u;
  });
  // A track that owns its file runs to the end of it, not to the next INDEX.
  CHECK_EQ(c.tracks[0].sector_count, 100u);
  CHECK_EQ(c.tracks[1].sector_count, 200u);
}

static void test_cue_rejects_garbage() {
  CHECK_THROWS(disc::parse_cue("TRACK 01 MODE1/2352\n  INDEX 01 00:00:00\n"));  // no FILE
  CHECK_THROWS(disc::parse_cue("FILE \"x.bin\" BINARY\n  TRACK 01 MODE9/9999\n"));
  CHECK_THROWS(disc::parse_cue(""));
}

// A synthetic raw disc: `n` sectors of MODE1/2352, each carrying a recognisable
// 2048-byte payload behind the 16-byte sync and header.
static std::string synth_2352(int n) {
  std::string img;
  for (int s = 0; s < n; ++s) {
    img.append(16, char(0xAA));                // sync + header
    img.append(2048, char('A' + (s % 26)));    // the payload
    img.append(2352 - 16 - 2048, char(0xBB));  // EDC/ECC
  }
  return img;
}

static void test_sector_extract_2352() {
  std::string raw = synth_2352(3);
  std::istringstream in(raw);
  std::ostringstream out;
  disc::extract_data_track(in, 0, disc::TrackMode::Mode1_2352, 3, out);
  std::string got = out.str();
  CHECK_EQ(got.size(), 3u * 2048u);
  CHECK_EQ(got[0], 'A');
  CHECK_EQ(got[2048], 'B');
  CHECK_EQ(got[2 * 2048], 'C');
  // Nothing from the sync bytes or the error correction may leak through.
  CHECK(got.find(char(0xAA)) == std::string::npos);
  CHECK(got.find(char(0xBB)) == std::string::npos);
}

static void test_sector_extract_mode2() {
  // MODE2/2352 puts the payload eight bytes further in, behind the subheader.
  std::string raw;
  raw.append(24, char(0xAA));
  raw.append(2048, char('Z'));
  raw.append(2352 - 24 - 2048, char(0xBB));
  std::istringstream in(raw);
  std::ostringstream out;
  disc::extract_data_track(in, 0, disc::TrackMode::Mode2_2352, 1, out);
  CHECK_EQ(out.str().size(), 2048u);
  CHECK_EQ(out.str()[0], 'Z');
}

// The property the whole database depends on: an .iso normalises to itself.
static void test_sector_identity_for_2048() {
  std::string raw(4096, 'Q');
  raw[0] = 'H';
  std::istringstream in(raw);
  std::ostringstream out;
  disc::extract_data_track(in, 0, disc::TrackMode::Mode1_2048, 2, out);
  CHECK_EQ(out.str(), raw);
}

static void test_sector_honours_offset() {
  std::string raw = synth_2352(3);
  std::istringstream in(raw);
  std::ostringstream out;
  disc::extract_data_track(in, 2352, disc::TrackMode::Mode1_2352, 1, out);
  CHECK_EQ(out.str().size(), 2048u);
  CHECK_EQ(out.str()[0], 'B');
}

static void test_sector_short_read_throws() {
  std::string raw = synth_2352(1);
  std::istringstream in(raw);
  std::ostringstream out;
  CHECK_THROWS(disc::extract_data_track(in, 0, disc::TrackMode::Mode1_2352, 5, out));
}

static void test_7z_listing() {
  // `7z l -slt -ba` emits a stanza per member, blank-line separated.
  const char* out =
      "Path = Disc Images/Disc 1 - Demo-Game.bin\n"
      "Size = 740816496\n"
      "Attributes = _ -rw-r--r--\n"
      "\n"
      "Path = Disc Images/Disc 1 - Demo-Game.cue\n"
      "Size = 1191\n"
      "\n"
      "Path = Disc Images\n"
      "Size = 0\n"
      "Attributes = D_ drwxr-xr-x\n";
  auto m = disc::parse_7z_listing(out);
  CHECK_EQ(m.size(), 3u);
  CHECK_EQ(m[0].path, std::string("Disc Images/Disc 1 - Demo-Game.bin"));
  CHECK_EQ(m[0].size, 740816496ull);
  CHECK_EQ(m[1].size, 1191ull);
  CHECK_EQ(m[2].size, 0ull);
}

static void test_7z_listing_empty() {
  CHECK_EQ(disc::parse_7z_listing("").size(), 0u);
}

static void test_classification() {
  CHECK(disc::is_disc_image("Classic Collection (USA).iso"));
  CHECK(disc::is_disc_image("Demo-Game.CUE"));
  CHECK(disc::is_disc_image("game.bin"));
  CHECK(disc::is_disc_image("disc.mdf"));
  CHECK(!disc::is_disc_image("readme.txt"));
  CHECK(!disc::is_disc_image("setup.exe"));

  CHECK(disc::is_archive("Adventure II (USA) (Disc 1).zip"));
  CHECK(disc::is_archive("thing.7z"));
  CHECK(disc::is_archive("thing.ZIP"));
  CHECK(!disc::is_archive("thing.iso"));
  CHECK(!disc::is_archive("thing.bin"));
}

static void test_key_extraction_hits() {
  auto k = disc::extract_keys("CD Key: ABCDE-FGHIJ-KLMNO-PQRST-UVWXY\n", "serial.txt");
  CHECK_EQ(k.size(), 1u);
  CHECK_EQ(k[0].value, std::string("ABCDE-FGHIJ-KLMNO-PQRST-UVWXY"));
  CHECK_EQ(k[0].source, std::string("serial.txt"));

  auto k2 = disc::extract_keys("Serial 1234-5678-9012-3456", "nfo");
  CHECK_EQ(k2.size(), 1u);
  CHECK_EQ(k2[0].value, std::string("1234-5678-9012-3456"));

  // A bare run of the right shape counts only when a key word is near it.
  auto k3 = disc::extract_keys("cd-key\nD4E5F6G7H8J9K0L1\n", "readme");
  CHECK_EQ(k3.size(), 1u);
  CHECK_EQ(k3[0].value, std::string("D4E5F6G7H8J9K0L1"));

  // Duplicates collapse.
  auto k4 = disc::extract_keys(
      "key: AAAAA-BBBBB-CCCCC-DDDDD-EEEEE\nkey: AAAAA-BBBBB-CCCCC-DDDDD-EEEEE\n", "x");
  CHECK_EQ(k4.size(), 1u);
}

static void test_key_extraction_non_hits() {
  // Prose must not produce candidates; a scanner that cries wolf is useless.
  CHECK_EQ(disc::extract_keys(
               "Thank you for purchasing this product. Please read the license "
               "agreement before installing. Installation requires 650 megabytes.",
               "readme").size(), 0u);
  CHECK_EQ(disc::extract_keys("2026-09-02 14:33:07 installation complete", "log").size(), 0u);
  // A long alphanumeric run with no key word nearby is not a key.
  CHECK_EQ(disc::extract_keys("checksum ABCDEF0123456789ABCDEF", "log").size(), 0u);
}

static void test_companion_scan(const fs::path& tmp) {
  fs::path d = tmp / "companions";
  fs::create_directories(d / "crack");
  std::ofstream(d / "game.iso") << "x";
  std::ofstream(d / "serial.txt") << "key";
  std::ofstream(d / "Game.nfo") << "info";
  std::ofstream(d / "README.TXT") << "read me";
  std::ofstream(d / "crack" / "game.exe") << "patched";
  std::ofstream(d / "unrelated.dat") << "no";

  auto c = disc::scan_companions(d / "game.iso");
  auto kind_of = [&](const std::string& name) {
    for (const auto& x : c) if (x.path.filename() == name) return x.kind;
    return std::string("(missing)");
  };
  CHECK_EQ(kind_of("serial.txt"), std::string("serial"));
  CHECK_EQ(kind_of("Game.nfo"), std::string("nfo"));
  CHECK_EQ(kind_of("README.TXT"), std::string("readme"));
  CHECK_EQ(kind_of("crack"), std::string("crack"));
  CHECK_EQ(kind_of("unrelated.dat"), std::string("(missing)"));
  CHECK_EQ(kind_of("game.iso"), std::string("(missing)"));  // not its own companion
}

static void test_wav_header() {
  std::ostringstream o;
  disc::write_wav_header(o, 1000);
  std::string h = o.str();
  CHECK_EQ(h.size(), 44u);
  CHECK_EQ(h.substr(0, 4), std::string("RIFF"));
  CHECK_EQ(h.substr(8, 4), std::string("WAVE"));
  CHECK_EQ(h.substr(12, 4), std::string("fmt "));
  CHECK_EQ(h.substr(36, 4), std::string("data"));
  auto le32 = [&](size_t at) {
    return uint32_t(uint8_t(h[at])) | uint32_t(uint8_t(h[at + 1])) << 8 |
           uint32_t(uint8_t(h[at + 2])) << 16 | uint32_t(uint8_t(h[at + 3])) << 24;
  };
  auto le16 = [&](size_t at) {
    return uint16_t(uint16_t(uint8_t(h[at])) | uint16_t(uint8_t(h[at + 1])) << 8);
  };
  CHECK_EQ(le32(4), 36u + 1000u);      // RIFF chunk size
  CHECK_EQ(le16(22), uint16_t(2));     // stereo, as CD-DA always is
  CHECK_EQ(le32(24), 44100u);          // 44.1 kHz
  CHECK_EQ(le16(34), uint16_t(16));    // 16 bits
  CHECK_EQ(le32(40), 1000u);           // data chunk size
}

static void test_rip_audio(const fs::path& tmp) {
  // Two sectors of CD-DA: 2352 bytes each, straight PCM, no header to strip.
  std::string raw;
  raw.append(2352, char(0x11));
  raw.append(2352, char(0x22));
  fs::path bin = tmp / "audio.bin";
  { std::ofstream f(bin, std::ios::binary); f << raw; }

  fs::path wav = tmp / "track02.wav";
  disc::rip_audio_track(bin, 2352, 1, wav);
  CHECK_EQ(fs::file_size(wav), 44u + 2352u);

  std::ifstream f(wav, std::ios::binary);
  std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK_EQ(got.substr(0, 4), std::string("RIFF"));
  CHECK_EQ(got[44], char(0x22));   // the second sector, because of the offset
}

static void test_volume_serial() {
  iso::Info a;
  a.created = "2000042512000000";
  iso::Info b;
  b.created = "2000042512000000";
  iso::Info c;
  c.created = "1997093015300000";
  // Same disc, same serial. Different disc, different serial. Never zero,
  // because Windows treats a zero serial as "no volume".
  CHECK_EQ(disc::volume_serial(a), disc::volume_serial(b));
  CHECK(disc::volume_serial(a) != disc::volume_serial(c));
  CHECK(disc::volume_serial(a) != 0u);
  iso::Info empty;
  CHECK(disc::volume_serial(empty) != 0u);
}

static void test_set_id() {
  CHECK_EQ(disc::set_id_from("Demo-Game (USA) (Special Edition)"),
           std::string("demo-game-usa-special-edition"));
  CHECK_EQ(disc::set_id_from("Strategy II - Online Edition"),
           std::string("strategy-ii-online-edition"));
  CHECK_EQ(disc::set_id_from("  Arena III  "), std::string("arena-iii"));
}

// Three rips of one Strategy disc are one disc and two alternates.
static void test_assemble_moves_duplicate_dumps_aside() {
  disc::Disc a, b, c;
  a.info.size = 100; a.info.prefix = hash_string("one");
  b.info.size = 100; b.info.prefix = hash_string("one");
  c.info.size = 200; c.info.prefix = hash_string("two");
  disc::DiscSet s = disc::assemble("strategy", "Strategy", {a, b, c});
  CHECK_EQ(s.set_id, std::string("strategy"));
  CHECK_EQ(s.name, std::string("Strategy"));
  CHECK_EQ(s.discs.size(), 2u);
  CHECK_EQ(s.alternates.size(), 1u);
  CHECK_EQ(s.alternates[0].info.size, 100ull);
}

static void test_drive_metadata(const fs::path& tmp) {
  fs::path d = tmp / "drive_d";
  fs::create_directories(d);
  disc::write_drive_metadata(d, "DEMOGAME", 0x1A2B3C4Du);

  std::ifstream lf(d / ".windows-label");
  std::string label;
  std::getline(lf, label);
  CHECK_EQ(label, std::string("DEMOGAME"));

  std::ifstream sf(d / ".windows-serial");
  std::string serial;
  std::getline(sf, serial);
  // Wine reads this as hex without a prefix, lower case.
  CHECK_EQ(serial, std::string("1a2b3c4d"));
}

static void test_repoint(const fs::path& tmp) {
  fs::path prefix = tmp / "prefix";
  fs::create_directories(prefix / "dosdevices");
  fs::path one = tmp / "disc1";
  fs::path two = tmp / "disc2";
  fs::create_directories(one);
  fs::create_directories(two);

  disc::repoint(prefix, 'd', one);
  CHECK(fs::read_symlink(prefix / "dosdevices" / "d:") == one);
  // Swapping a disc is re-pointing the symlink, which Wine resolves per open,
  // so it takes effect without restarting anything.
  disc::repoint(prefix, 'd', two);
  CHECK(fs::read_symlink(prefix / "dosdevices" / "d:") == two);
}

// The database gained two columns. Old four-column rows must still load, or a
// user's existing db stops naming their discs after an upgrade. The stored
// prefix is 32 hex digits, not 64: only the leading bytes are compared.
static void test_database_v1_and_v2(const fs::path& tmp) {
  fs::path f = tmp / "discs.txt";
  {
    std::ofstream o(f);
    o << "# comment\n";
    o << "00112233445566778899aabbccddeeff\t512000000\tEXAMPLE\tExample Game\n";
    o << "ffeeddccbbaa99887766554433221100\t3800000000\tCLASSICS\tclassic-collection\t2\tClassic Collection\n";
    o << "not-a-hash\t1\tX\tignored\n";
  }
  auto db = iso::load_database_file(f);
  CHECK_EQ(db.size(), 2u);
  CHECK_EQ(db[0].volume_id, std::string("EXAMPLE"));
  CHECK_EQ(db[0].name, std::string("Example Game"));
  CHECK_EQ(db[0].disc_no, 1);          // v1 rows default to disc 1
  CHECK_EQ(db[0].set_id, std::string(""));
  CHECK_EQ(db[1].set_id, std::string("classic-collection"));
  CHECK_EQ(db[1].disc_no, 2);
  CHECK_EQ(db[1].name, std::string("Classic Collection"));
}

int main() {
  fs::path tmp = fs::temp_directory_path() / "kretro-test-disc";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  try {
    test_msf();
    test_geometry();
    test_cue_single_file();
    test_cue_multi_file();
    test_cue_rejects_garbage();
    test_sector_extract_2352();
    test_sector_extract_mode2();
    test_sector_identity_for_2048();
    test_sector_honours_offset();
    test_sector_short_read_throws();
    test_7z_listing();
    test_7z_listing_empty();
    test_classification();
    test_key_extraction_hits();
    test_key_extraction_non_hits();
    test_companion_scan(tmp);
    test_wav_header();
    test_rip_audio(tmp);
    test_volume_serial();
    test_set_id();
    test_assemble_moves_duplicate_dumps_aside();
    test_drive_metadata(tmp);
    test_repoint(tmp);
    test_database_v1_and_v2(tmp);
  } catch (const std::exception& e) {
    kgtest::unexpected(e);
  }

  fs::remove_all(tmp);
  return kgtest::finish();
}
