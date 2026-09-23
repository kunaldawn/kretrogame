// The player's command line.
//
//   ./bundle.run                      the launcher, or the only game
//   ./bundle.run play <game>          skip the launcher
//   ./bundle.run play <game> --dry-run
//                                     everything but starting the game
//   ./bundle.run --doctor [--save F]  diagnostics
//   ./bundle.run saves <game> export|import <file>
//   ./bundle.run --extract-to DIR [<game>]
//   ./bundle.run --licenses
//
// Parsed into a plain struct, apart from doing any of it, so every spelling a
// person might type is a table test. Anything not understood is an error that
// says which word it was: a player is run by people who were not told how.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kg::player {

struct Command {
  enum class Kind {
    Launcher,
    Play,
    Doctor,
    SavesExport,
    SavesImport,
    ExtractTo,
    Licenses,
    Help,
    Input,  // started by a session beside a game; not for people
    Error,
  };
  Kind kind = Kind::Launcher;
  std::string game;
  std::filesystem::path file;  // saves export/import, --doctor --save, --extract-to
  bool dry_run = false;
  // play's one-session overrides; unset means the game's settings.
  bool fullscreen_set = false, fullscreen = false;
  std::string error;  // for Kind::Error, what was wrong with the line
  // The input helper's own arguments, passed through as given.
  std::vector<std::string> rest;
};

Command parse_command(const std::vector<std::string>& args);

// The text --help prints, with the file's own name in it.
std::string usage(const std::string& exe_name);

}  // namespace kg::player
