// Numbers, durations and times as the programs print them. Each function here
// is one variant, shared by the files that print it; the variants differ on
// purpose - one base against the other, one unit spelling
// against the other - and each keeps the exact output its callers always had.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>

namespace kg {

// "  <line>\n" on stderr: how kretro, the player and a session say what they
// are doing.
void log_line(std::string_view line);

}  // namespace kg

namespace kg::fmt {

// Base 1024 with IEC units: "512 B", "1.5 KiB", "4.7 GiB". kretro and kgpack.
std::string bytes_iec(uint64_t n);
// Base 1024 with short units, whole numbers up to kilobytes: "512 B", "2 KB",
// "4.7 GB". The shelf and the Bundles page.
std::string bytes_compact(uint64_t n);
// Base 1000: "512 B", "1.5 KB", "5.0 GB". The player's sentences about space.
std::string bytes_si(uint64_t n);
// "3.5 GB": decimal gigabytes, one place. The player's free-space messages.
std::string gigabytes(uint64_t n);

// "59s", "1m 1s", "1h 1m". kretro's journal.
std::string duration(double seconds);
// "59s", "1m", "1h 1m": the shelf's shorter form, minutes without seconds.
std::string duration_short(double seconds);

// "8 months ago": what a person needs to see after a long absence, where a
// timestamp says less. `never` is the words for a time that is not set (zero
// or less).
std::string ago(std::time_t then, const char* never);

// "2024-05-01 18:30" in local time.
std::string local_minute(std::time_t t);

}  // namespace kg::fmt
