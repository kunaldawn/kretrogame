// The Wine prefix a game runs in, made ready before anything of the game's
// runs in it.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "../pack/kgpack.h"
#include "../rt/env.h"

namespace kg::session {

// Everything a Wine prefix needs before a Windows program runs in it: the
// wineboot, the x11 graphics pin, and the Mono/Gecko suppression that stops
// wineboot putting up a dialog nobody is there to click.
//
// `apply_registry` imports Meta.registry.fragment, once per prefix. Play passes
// true - that fragment is the difference between a capsule of files and a game
// that starts. The installer passes false, and must: install::run diffs the
// staging prefix's registry across the installer to *produce* that fragment, and
// a fragment already present on both sides of the diff cancels itself out,
// leaving a rebuilt pack with no registry at all.
//
// `system_tree` is the pack's system/ - what the installer wrote to C: outside
// the game directory. It is copied into the prefix here rather than by the
// caller, because the only correct moment is this one: after wineboot has laid
// down its own C:, and before the fragment that names those files is imported.
// Empty for a prefix with no pack behind it, and for the staging install.
//
// It begins with adopt_player_profile, before anything of Wine's runs in it.
void prepare_prefix(const rt::Env& e, const std::filesystem::path& prefix,
                    const std::filesystem::path& home, const Meta& m, bool apply_registry,
                    const std::function<void(const std::string&)>& say,
                    const std::filesystem::path& system_tree = {});

// Every Wine kretro and the player start is told its user is "player"
// (WINEUSERNAME, rt::make), because the player's prefix template was made as
// "player" and one fixed name is one fewer place a login ends up in a saves
// export. A prefix kretro made before that named its profile after the person
// - C:\users\<login> - and Wine, now running as "player", would make a new
// empty C:\users\player beside it and point the registry's profile there: a
// game's saves in My Documents would seem to vanish.
//
// So the old profile is moved, not abandoned: when drive_c/users holds exactly
// one real profile directory besides Public, and no "player" yet, it is
// renamed to "player" and a symlink with the old name is left pointing at it,
// so a path the registry or a game's own settings recorded in full
// (C:\users\<login>\...) still reaches the same files. Anything else - no old
// profile, several, or a "player" already there - is left exactly as it is:
// guessing which of two profiles holds somebody's saves is not this code's to
// do, and both stay on disk. Returns what it did, in a sentence; empty when it
// did nothing.
std::string adopt_player_profile(const std::filesystem::path& prefix);

}  // namespace kg::session
