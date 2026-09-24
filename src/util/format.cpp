#include "format.h"

#include <cstdio>

namespace kg {

void log_line(std::string_view line) {
  std::fprintf(stderr, "  %.*s\n", static_cast<int>(line.size()), line.data());
}

}  // namespace kg

namespace kg::fmt {

std::string bytes_iec(uint64_t n) {
  const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(n);
  int i = 0;
  while (v >= 1024.0 && i < 4) {
    v /= 1024.0;
    ++i;
  }
  char b[64];
  std::snprintf(b, sizeof(b), i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
  return b;
}

std::string bytes_compact(uint64_t n) {
  const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(n);
  int i = 0;
  while (v >= 1024.0 && i < 4) {
    v /= 1024.0;
    ++i;
  }
  char b[64];
  std::snprintf(b, sizeof(b), i <= 1 ? "%.0f %s" : "%.1f %s", v, u[i]);
  return b;
}

std::string bytes_si(uint64_t n) {
  const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(n);
  int i = 0;
  while (v >= 1000.0 && i < 4) {
    v /= 1000.0;
    ++i;
  }
  char b[32];
  std::snprintf(b, sizeof(b), i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
  return b;
}

std::string gigabytes(uint64_t n) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.1f GB", static_cast<double>(n) / 1e9);
  return b;
}

std::string duration(double seconds) {
  long s = static_cast<long>(seconds);
  char b[64];
  if (s < 60) std::snprintf(b, sizeof(b), "%lds", s);
  else if (s < 3600) std::snprintf(b, sizeof(b), "%ldm %lds", s / 60, s % 60);
  else std::snprintf(b, sizeof(b), "%ldh %ldm", s / 3600, (s % 3600) / 60);
  return b;
}

std::string duration_short(double seconds) {
  long s = static_cast<long>(seconds);
  char b[64];
  if (s < 60) std::snprintf(b, sizeof(b), "%lds", s);
  else if (s < 3600) std::snprintf(b, sizeof(b), "%ldm", s / 60);
  else std::snprintf(b, sizeof(b), "%ldh %ldm", s / 3600, (s % 3600) / 60);
  return b;
}

std::string ago(std::time_t then, const char* never) {
  if (then <= 0) return never;
  double d = std::difftime(std::time(nullptr), then);
  struct Step {
    double limit, div;
    const char* unit;
  };
  static const Step steps[] = {
      {60, 1, "second"},
      {3600, 60, "minute"},
      {86400, 3600, "hour"},
      {86400 * 30.0, 86400, "day"},
      {86400 * 365.0, 86400 * 30.0, "month"},
      {1e18, 86400 * 365.0, "year"},
  };
  for (const Step& s : steps) {
    if (d < s.limit) {
      long n = static_cast<long>(d / s.div);
      char b[64];
      std::snprintf(b, sizeof(b), "%ld %s%s ago", n, s.unit, n == 1 ? "" : "s");
      return b;
    }
  }
  return "long ago";
}

std::string local_minute(std::time_t t) {
  // localtime_r rather than std::localtime: the Bundles page calls this from
  // the UI thread while a build runs on another.
  std::tm tm{};
  localtime_r(&t, &tm);
  char when[32] = {};
  std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
  return when;
}

}  // namespace kg::fmt
