// Offering what a local manifest knows about the discs already on the table.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "../disc/disc.h"
#include "../pack/kgpack.h"
#include "../rt/env.h"
#include "draft.h"

namespace kg::install {

// A local manifest that recognises the discs already on the table.
//
// games/*.toml is never required and never consulted unless it matches
// something the user has already brought in. A manifest exists because
// somebody sat with a disc and found out what its installer is called, which of
// two executables is the game, and which Windows version its installer wants.
// That is knowledge worth offering. It is worth nothing as a gate: a disc
// nobody had written a manifest for could not be installed at all.
struct Preset {
  std::string id, name;
  std::filesystem::path manifest;
  size_t matched = 0;           // how many of its discs are here
  size_t of = 0;                // how many it names
  bool by_fingerprint = false;  // identity, rather than a name that agrees
};

// How much of one manifest the assembled set accounts for. `from` is only
// carried through so the caller can load it again if the offer is accepted.
Preset score_preset(const Meta& manifest, const std::filesystem::path& from,
                    const std::vector<disc::Disc>& discs);

// Every manifest that recognises this set, strongest first. An empty vector
// means "we know nothing about these discs", which is an ordinary answer and
// not an error.
std::vector<Preset> match_presets(const rt::Env& e, const std::vector<disc::Disc>& discs);

// Copies into a draft what a manifest knows: id, name, year, setup, method,
// member, subdir, exe, args, width, height, windows_version, dgvoodoo. Every
// one stays editable.
//
// The method is among them, and the two fields that go with it. It was left
// out once, on the grounds that the wizard already knows whether it is looking
// at a disc or a bare .exe and a manifest disagreeing would be a guess
// overriding a fact - but the method is not that fact. A manifest may say
// method="unzip", member="Data2.zip", subdir="Example Game 1.02", and a disc
// is exactly what it wants: the manifest exists to say that the compilation
// disc carries several games as zips and which of them this one is. Leaving it
// out prefilled that title as an installer disc naming nothing, which is not a
// thinner answer than the manifest's but a wrong one.
//
// What the wizard does know better is what is *possible*: step 3 offers only
// the methods these sources can run, and clamp_method puts a preset that names
// a method not on offer back onto one that is. That is where the fact wins,
// and it wins after the copy rather than instead of it.
void apply_preset(const Meta& manifest, Draft* d);

}  // namespace kg::install
