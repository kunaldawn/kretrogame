// The one table of kretro's commands, and the dispatch that reads it.
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "cli.h"
#include "command.h"
#include "../gpu/probe.h"
#include "handlers.h"
#include "../rt/env.h"

namespace kg::cli {

namespace {

constexpr Arity kAny{};
constexpr Arity kNone{0, 0};
constexpr Arity kOne{1, 1};
constexpr Arity kTwo{2, 2};
constexpr Arity kSome{1, SIZE_MAX};

// In the order of the help, each row carrying its own lines of it. stage-probe,
// bundle-build and panel are hidden: the first is a test hook, the second the
// name `bundle build` had while it was one, which scripts still use, and the
// third is how the shelf measures the panel from another process.
// clang-format off: each entry keeps its help text on lines of its own.
constexpr Command kCommands[] = {
    {"create", Needs::GpuEnv, kNone, cmd_create,
     "  kretro create                install a game from your own discs\n"},
    {"doctor", Needs::Env, kAny, cmd_doctor,
     "  kretro doctor [--save FILE]  what this machine can and cannot do; --save\n"
     "                               writes it without your home directory or name\n"},
    {"info", Needs::Env, kAny, cmd_info,
     "  kretro info                  runtime, state and library paths\n"},
    {"games", Needs::Env, kAny, cmd_games,
     "  kretro games                 the manifests, and whether their discs are here\n"},
    {"identify", Needs::Env, kOne, cmd_identify,
     "  kretro identify <iso>        what a disc image is\n"},
    {"key", Needs::Env, {1, 3}, cmd_key,
     "  kretro key <id> [serial]     show or store a game's serial\n"},
    {"panel", Needs::Env, kNone, cmd_panel, ""},
    {"display", Needs::Env, kOne, cmd_display,
     "  kretro display <id>          where this game lands on this screen\n"},
    {"swap", Needs::Env, kTwo, cmd_swap,
     "  kretro swap <id> <n>         put disc n in the drive, mid-install\n"},
    {"scan", Needs::Env, kAny, cmd_scan,
     "  kretro scan [dir] [--write-db]  identify every disc in a directory\n"},
    {"contents", Needs::Env, kOne, cmd_contents,
     "  kretro contents <archive[#LABEL]>   what is on a disc\n"},
    {"stage-probe", Needs::Env, kAny, cmd_stage_probe, ""},
    {"install", Needs::GpuEnv, kAny, cmd_install,
     "  kretro install <id> [--headless] [--force] [--keep-tree] [--no-discs]\n"
     "                               open the wizard on a game we have a manifest for;\n"
     "                               --headless installs a copy or unzip game with no\n"
     "                               display, for scripts. An installer you have to click\n"
     "                               through has no headless form and refuses.\n"
     "                               --no-discs leaves the discs out of the pack.\n"},
    {"list", Needs::Env, kAny, cmd_list,
     "  kretro list                  your collection\n"},
    {"verify", Needs::Env, kOne, cmd_verify,
     "  kretro verify <id>           check a pack's tree root and its body\n"},
    {"uninstall", Needs::Env, kOne, cmd_uninstall,
     "  kretro uninstall <id>\n"},
    {"play", Needs::GpuEnv, kAny, cmd_play,
     "  kretro play <id> [--fullscreen] [--integer|--fit|--native] [--scale N] [--dry-run]\n"
     "                    [--note \"...\"]\n"},
    // Stays Env: it probes the GPU itself, after its argument is checked.
    {"show", Needs::Env, kOne, cmd_show,
     "  kretro show <id>             open the shelf on one game\n"},
    {"export", Needs::Env, kSome, cmd_export,
     "  kretro export <id> [--recipe|--capsule] [-o FILE]\n"
     "                               a game for another kretro: the whole pack, or\n"
     "                               the recipe to rebuild it from the same disc\n"},
    {"export-saves", Needs::Env, kSome, cmd_export_saves,
     "  kretro export-saves <id>     just what the game wrote\n"},
    {"import", Needs::Env, kOne, cmd_import,
     "  kretro import <file>         a capsule, or a recipe to rebuild\n"},
    {"bundle", Needs::Env, kAny, cmd_bundle,
     "  kretro bundle build -o FILE|DIR [--id ID] [--title T] [--version V]\n"
     "                    [--base PLAYER-BASE] --rights GAME...\n"
     "                               a player for these games, the file you ship;\n"
     "                               the Bundles page on the shelf is the same build\n"
     "  kretro bundle list           the bundles the Bundles page remembers\n"
     "  kretro bundle rebuild <id>   build one of them again, as its Build button would\n"},
    {"bundle-build", Needs::Env, kAny, cmd_bundle_build, ""},
    {"import-saves", Needs::Env, kTwo, cmd_import_saves,
     "  kretro import-saves <id> <file>  put somebody's saves back\n"},
    {"compare", Needs::Env, kSome, cmd_compare,
     "  kretro compare <id>          photograph each renderer so you can pick\n"},
    {"journal", Needs::Env, kOne, cmd_journal,
     "  kretro journal <id>          when you last played, and where you were\n"},
    {"saves", Needs::Env, kOne, cmd_saves,
     "  kretro saves <id>            the session timeline\n"},
    {"restore", Needs::Env, kTwo, cmd_restore,
     "  kretro restore <id> <gen>    go back to a snapshot\n"},
    {"wine", Needs::GpuEnv, kAny, cmd_wine,
     "  kretro wine [args...]        run the bundled Wine\n"},
    {"exec", Needs::GpuEnv, kSome, cmd_exec,
     "  kretro exec <prog> [args]    run a bundled program\n"},
    {"input", Needs::Env, kAny, cmd_input,
     "  kretro input --display D --pid N [--game <id>] [--no-pause]\n"
     "                               gamepad translation for a running game\n"},
};
// clang-format on

const Command* find_command(std::string_view name) {
  for (const Command& c : kCommands) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

}  // namespace

std::span<const Command> commands() { return kCommands; }

int usage() {
  std::string text =
      "kretro - old games, one binary\n"
      "\n"
      "  kretro                       the shelf\n";
  for (const Command& c : commands()) text += c.help;
  text += "\n";
  std::fputs(text.c_str(), stderr);
  return 2;
}

int run(std::vector<std::string> args) {
  std::string cmd = args[0];
  args.erase(args.begin());

  try {
    const Command* entry = find_command(cmd);
    // The GPU probe is only needed by things that draw; extraction and packing
    // do not, and it costs a directory walk.
    bool needs_gpu = entry && entry->needs == Needs::GpuEnv;
    gpu::Report gl;
    if (needs_gpu) {
      gl = gpu::probe();
      gpu::materialize(gl);
    }
    rt::Env e = rt::make(needs_gpu ? &gl : nullptr);

    if (!entry || args.size() < entry->arity.min || args.size() > entry->arity.max) return usage();
    return entry->run(e, args);
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "kretro: %s\n", ex.what());
    return 1;
  }
}

}  // namespace kg::cli
