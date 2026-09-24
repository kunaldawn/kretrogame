// What the installer wrote to the registry.
//
// An installer's output is not only files. Some games find their install
// directory through the registry, others keep their video settings there, and
// most of that era's games store the CD key beside them. A capsule
// that restores only files restores a game that will not start, so the install
// engine diffs the prefix's registry across the installer run and keeps the
// difference.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../rt/env.h"

namespace kg::wine {

// The two hives a prefix keeps in files of its own. Wine writes
// HKEY_LOCAL_MACHINE to system.reg and HKEY_CURRENT_USER to user.reg, and a
// key path inside either file is relative to that root. Which file a value came
// out of is therefore part of the value: an installer's HKLM keys restored to
// HKCU are keys the game will never find.
inline constexpr std::string_view kHiveLocalMachine = "HKEY_LOCAL_MACHINE";
inline constexpr std::string_view kHiveCurrentUser = "HKEY_CURRENT_USER";

struct RegValue {
  std::string key;    // without the hive: "Software\\Example Publisher\\Example Game"
  std::string name;   // "@" for the default value
  std::string type;   // sz | expand_sz | multi_sz | dword | hex | hex(n)
  std::string data;   // strings decoded; dword and hex left as their digits
  std::string hive = std::string(kHiveCurrentUser);
};

// Parses Wine's own .reg format (system.reg, user.reg). `hive` is the root the
// file speaks for, since the file itself never names it.
std::vector<RegValue> parse_reg(std::string_view text,
                                std::string_view hive = kHiveCurrentUser);

// Values present in `after` with a different value, or not present in `before`.
// Sorted by hive, key then name, so a recipe's fragment is byte-stable across
// runs.
std::vector<RegValue> diff_reg(const std::vector<RegValue>& before,
                               const std::vector<RegValue>& after);

// A REGEDIT4 fragment that `wine regedit` will import, one section per key,
// each under the hive the value was read from. Wine's internal type tokens are
// translated on the way out: regedit's importer has never heard of str(2), so
// expand- and multi-strings are written as hex(2)/hex(7) UTF-16LE.
std::string to_reg_fragment(const std::vector<RegValue>& v);

// A pack's registry fragment is the REGEDIT4 text to_reg_fragment writes, not
// the Wine hive format parse_reg reads; this reads it back into values, strings
// decoded and everything else left as written. It is not parse_reg: the two
// formats escape and continue lines differently.
std::vector<RegValue> parse_fragment(std::string_view regedit4);

// Whether a value the installer wrote is the serial the person read off their
// own disc sleeve and typed into it.
//
// keys.h states, twice, that a serial stays on this machine and goes into no
// pack, because it is the user's and not the game's. The registry diff was
// unfiltered, so a CD key an installer wrote to HKLM landed in
// Meta.registry.fragment, in registry.reg inside the body, and in every recipe
// exported from that pack - which is the policy broken in the one place the
// user could not see it.
//
// The test is deliberately narrow, because dropping a value the game needs is
// its own kind of damage: the name has to read as a key - CDKey, Serial,
// ProductKey, RegistrationNumber - and the data has to be shaped like one. A
// path, a filename, a number Wine computed, or anything under eight characters
// is not a serial whatever it is called.
bool is_serial_value(const RegValue& v);

// The values minus those, with what was held back named in `dropped` so the
// person watching the install is told rather than not told.
std::vector<RegValue> without_serials(const std::vector<RegValue>& v,
                                      std::vector<std::string>* dropped = nullptr);

// Reads system.reg (HKLM) and user.reg (HKCU) out of a prefix.
std::vector<RegValue> snapshot_prefix(const std::filesystem::path& prefix);

// Writes the fragment to a temporary file and imports it with `wine regedit`.
void apply_fragment(const rt::Env& e, const std::filesystem::path& prefix,
                    const std::string& fragment);

// Whether `prefix` has already had exactly this fragment imported.
//
// The fragment is applied once per prefix, not once per launch. A game that
// keeps its own settings in the registry - and most of this era do - would have
// them written back to the installer's defaults on every start otherwise. The
// marker holds the BLAKE3 of the fragment last applied, so reinstalling or
// reimporting the game, which produces a different fragment, applies again.
bool registry_marker_matches(const std::filesystem::path& prefix, const std::string& fragment);
void write_registry_marker(const std::filesystem::path& prefix, const std::string& fragment);

}  // namespace kg::wine
