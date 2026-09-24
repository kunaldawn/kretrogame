// The unit tests' checks: one count of checks and one of failures per test
// program, the macros that keep them, and the summary line each program ends
// with. Header-only, so each test stays one file linked with the library.
//
// Every macro counts exactly one check, whether it passes, fails or throws
// before it can decide: the totals `make test` prints are compared against
// fixed numbers, so a check must never count twice or not at all.
#pragma once

#include <cstdio>
#include <exception>
#include <ostream>
#include <sstream>
#include <string>

namespace kgtest {

inline int checks = 0, failures = 0;

// A failure, said where it happened. It does not count a check: the macros
// count theirs before they evaluate anything, so one that throws still counts.
inline void fail(const char* file, int line, const std::string& what) {
  ++failures;
  std::fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, what.c_str());
}

// One check, already decided.
inline void record(bool ok, const char* file, int line, const std::string& what) {
  ++checks;
  if (!ok) fail(file, line, what);
}

// The heading a group of checks is printed under.
inline void section(const char* name) { std::fprintf(stderr, "%s\n", name); }

// An exception that escaped every check: a failure, but not a check.
inline void unexpected(const std::exception& e) {
  std::fprintf(stderr, "  FAIL unexpected exception: %s\n", e.what());
  ++failures;
}

// The summary line, and the exit status that goes with it.
inline int finish() {
  std::fprintf(stderr, "\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

// A value as a failure shows it, or "<?>" for a type that cannot be printed.
template <class T>
std::string show(const T& v) {
  if constexpr (requires(std::ostream& o, const T& x) { o << x; }) {
    std::ostringstream os;
    os << v;
    return os.str();
  } else {
    return "<?>";
  }
}

}  // namespace kgtest

#define CHECK(cond)                                       \
  do {                                                    \
    ++kgtest::checks;                                     \
    if (!(cond)) kgtest::fail(__FILE__, __LINE__, #cond); \
  } while (0)

#define CHECK_EQ(a, b)                                                                               \
  do {                                                                                               \
    ++kgtest::checks;                                                                                \
    auto va_ = (a);                                                                                  \
    auto vb_ = (b);                                                                                  \
    if (!(va_ == vb_))                                                                               \
      kgtest::fail(__FILE__, __LINE__,                                                               \
                   std::string(#a " != " #b ": ") + kgtest::show(va_) + " != " + kgtest::show(vb_)); \
  } while (0)

#define CHECK_THROWS(expr)                                                                 \
  do {                                                                                     \
    ++kgtest::checks;                                                                      \
    bool threw_ = false;                                                                   \
    try {                                                                                  \
      static_cast<void>(expr);                                                             \
    } catch (const std::exception&) {                                                      \
      threw_ = true;                                                                       \
    }                                                                                      \
    if (!threw_) kgtest::fail(__FILE__, __LINE__, std::string("did not throw: ") + #expr); \
  } while (0)

// The exception has to be the one meant: its message must contain `needle`.
#define CHECK_THROWS_WITH(expr, needle)                                                    \
  do {                                                                                     \
    ++kgtest::checks;                                                                      \
    std::string what_;                                                                     \
    bool threw_ = false;                                                                   \
    try {                                                                                  \
      static_cast<void>(expr);                                                             \
    } catch (const std::exception& e_) {                                                   \
      threw_ = true;                                                                       \
      what_ = e_.what();                                                                   \
    }                                                                                      \
    if (!threw_ || what_.find(needle) == std::string::npos)                                \
      kgtest::fail(__FILE__, __LINE__,                                                     \
                   std::string(#expr) + ": wanted a throw with '" + (needle) + "', got " + \
                       (threw_ ? "" : "no throw ") + "'" + what_ + "'");                   \
  } while (0)
