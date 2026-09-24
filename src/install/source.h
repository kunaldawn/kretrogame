// What a person handed step 1, and what can be done with it.
//
// A source is a file or a directory: a disc image, an archive holding discs, a
// directory that is a disc, or a bare installer somebody downloaded. Which of
// those it is decides which install methods step 3 can offer.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "draft.h"

namespace kg::install {

struct Source {
  std::filesystem::path path;
  enum class Kind { DiscImage, Archive, Directory, BareExe, Unreadable } kind;
  std::string trouble;          // why, when Unreadable
};

// Classifies without opening anything. Cheap; runs as the user adds files.
Source classify_source(const std::filesystem::path& p);

// The bare .exe among a set of sources, if there is one: a repacked installer
// somebody downloaded, with no disc behind it and none wanted. open_sources
// deliberately opens no disc for one (build_sources.cpp's BareExe case), which is why
// this is read off the classification rather than out of the disc set - and
// why a source set can be complete with nothing in discs() at all.
std::filesystem::path bare_exe(const std::vector<Source>& sources);

// Whether step 1 has enough to go on. A disc set is enough; so is that bare
// .exe, on its own, and refusing it is what made Draft::Method::InstallerExe
// unreachable from the wizard - the fourth method offered on step 3 and
// selectable from nowhere.
bool sources_are_enough(const std::vector<Source>& sources, size_t discs);

// The methods these sources can actually run, in the order step 3 offers them.
// Three of the four need a disc and the fourth needs the bare .exe, so
// offering all four always is offering a failure three steps later. Never
// empty: with nothing readable at all it is the one method that wants a disc,
// which is the honest thing to be asking for.
std::vector<Draft::Method> methods_for(const std::vector<Source>& sources, size_t discs);

// The method a page can actually be editing, given what the sources can run.
//
// methods_for is the list step 3 offers, and a draft arrives with a method
// from somewhere else: a manifest, a preset, or the sources the user has since
// removed. A method that is not in the list is drawn as whichever entry the
// combo happens to land on while every edit below it and the Go button run the
// other one - so it is clamped to the offer instead, once, wherever the offer
// changes.
Draft::Method clamp_method(const std::vector<Draft::Method>& offered, Draft::Method want);

}  // namespace kg::install
