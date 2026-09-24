// Turning a disc you own into a game you can play.
//
// The result is one file: state/games/<id>.kgpack, carrying the game's tree,
// its Merkle root, the recipe that produced it and the fingerprint of the disc
// it came from. That file is simultaneously the installed game and the
// shareable capsule; there is no separate export step and no install directory.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "../pack/kgpack.h"
#include "../rt/env.h"

namespace kg::install {

struct Options {
  bool force = false;        // reinstall over an existing pack
  bool keep_tree = false;    // leave the extracted tree for inspection
  // Whether the pack carries the discs it was built from. True is the honest
  // default and what the wizard's checkbox starts on: the disc is stored once
  // and deduplicated against the installed files, so a copy game pays almost
  // nothing for it. A disc that is mostly *not* this game - one volume holding
  // three of them - is the case where a caller wants it off, and then the
  // game's page says "needs the original disc" the way it does for a disc the
  // library cannot find.
  bool embed_discs = true;
  // Set when rebuilding from a recipe someone shared: the tree that comes out
  // is compared against this and the difference reported. It is not a refusal -
  // a person clicking through an installer twice need not produce the same
  // bytes, and no answer file makes that promise.
  bool expect_root_set = false;
  Hash expect_root{};
};

struct Result {
  std::filesystem::path pack;
  bool root_matched = false;  // meaningful only when Options::expect_root_set
  uint64_t tree_bytes = 0;
  uint64_t pack_bytes = 0;
  size_t entries = 0;
  Hash root{};
};

Result run(const rt::Env& e, Meta manifest, const Options& opt,
           const std::function<void(const std::string&)>& progress);

}  // namespace kg::install
