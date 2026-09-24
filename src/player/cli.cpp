#include "cli.h"

#include <algorithm>
#include <utility>

namespace kg::player {

namespace {
Command error(const std::string& why) {
  Command c;
  c.kind = Command::Kind::Error;
  c.error = why;
  return c;
}
}  // namespace

Command parse_command(const std::vector<std::string>& a) {
  Command c;
  if (a.empty()) return c;
  const std::string& first = a[0];

  if (first == "-h" || first == "--help" || first == "help") {
    c.kind = Command::Kind::Help;
    return c;
  }
  if (first == "input") {
    c.kind = Command::Kind::Input;
    c.rest.assign(a.begin() + 1, a.end());
    return c;
  }
  if (first == "--licenses" || first == "--licences") {
    if (a.size() != 1) return error("--licenses takes nothing after it");
    c.kind = Command::Kind::Licenses;
    return c;
  }
  if (first == "--doctor") {
    c.kind = Command::Kind::Doctor;
    if (a.size() == 1) return c;
    if (a.size() == 3 && a[1] == "--save") {
      c.file = a[2];
      return c;
    }
    return error("--doctor takes only --save FILE");
  }
  if (first == "--extract-to") {
    if (a.size() < 2 || a[1].empty() || a[1][0] == '-') return error("--extract-to needs a directory");
    c.kind = Command::Kind::ExtractTo;
    c.file = a[1];
    if (a.size() == 3) c.game = a[2];
    if (a.size() > 3) return error("--extract-to takes a directory and at most one game");
    return c;
  }
  if (first == "play") {
    c.kind = Command::Kind::Play;
    for (size_t i = 1; i < a.size(); ++i) {
      const std::string& s = a[i];
      if (s == "--dry-run") c.dry_run = true;
      else if (s == "--fullscreen") { c.fullscreen_set = true; c.fullscreen = true; }
      else if (s == "--windowed") { c.fullscreen_set = true; c.fullscreen = false; }
      else if (!s.empty() && s[0] == '-') return error("play does not know " + s);
      else if (c.game.empty()) c.game = s;
      else return error("play takes one game, and was given " + c.game + " and " + s);
    }
    if (c.game.empty()) return error("play needs the game to play");
    return c;
  }
  if (first == "saves") {
    if (a.size() != 4) return error("saves wants: saves <game> export|import <file>");
    c.game = a[1];
    c.file = a[3];
    if (a[2] == "export") c.kind = Command::Kind::SavesExport;
    else if (a[2] == "import") c.kind = Command::Kind::SavesImport;
    else return error("saves can export or import, not " + a[2]);
    return c;
  }
  return error("this player does not know '" + first + "'");
}

std::string usage(const std::string& exe) {
  // The file's own name leads every line and is any length, so the column
  // the explanations start in is worked out rather than typed.
  const std::pair<std::string, std::vector<std::string>> lines[] = {
      {"", {"the launcher, or the only game"}},
      {"play <game> [--dry-run]", {"start one game; --dry-run gets it", "ready and stops before starting it"}},
      {"--doctor [--save FILE]", {"what this machine can do; --save", "writes it without your name in it"}},
      {"saves <game> export <file>", {"what the game has written, as a file"}},
      {"saves <game> import <file>", {"put saves back; yours are kept first"}},
      {"--extract-to DIR [<game>]", {"unpack a game into DIR, for machines", "that cannot mount it"}},
      {"--licenses", {"the notices this file carries, and", "where the source of each part is"}},
  };
  size_t w = 0;
  for (const auto& [cmd, text] : lines) w = std::max(w, exe.size() + 1 + cmd.size());
  std::string out;
  for (const auto& [cmd, text] : lines) {
    std::string left = exe;
    if (!cmd.empty()) left += " " + cmd;
    for (size_t i = 0; i < text.size(); ++i) {
      out += "  " + (i == 0 ? left : std::string()) +
             std::string(w - (i == 0 ? left.size() : 0) + 3, ' ') + text[i] + "\n";
    }
  }
  return out;
}

}  // namespace kg::player
