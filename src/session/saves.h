// What a game has written, kept: generations of its writable layer, and
// putting one back.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace kg::session {

// Copies a tree with hardlinks where it can, so a snapshot of a 600 MB game
// whose save is 2 MB costs 2 MB, and counts the files that arrived.
//
// False when anything did not arrive: a directory that could not be read, a
// file that could neither be linked nor copied, a destination that could not
// be created. Returning nothing, and counting every file it had tried, would
// make "the copy worked" indistinguishable from "the disk is full" to the one
// caller that then deletes the original.
bool link_tree(const std::filesystem::path& from, const std::filesystem::path& to,
               size_t* files);

// Copies the writable layer into a numbered generation as a hardlink farm, so
// only files that actually changed cost anything. Empty when there was nothing
// to copy; throws when the copy did not complete, having removed what it got -
// half a generation is offered as a whole one and restores a state the game
// was never in.
std::filesystem::path snapshot(const std::string& id, const std::filesystem::path& upper);
std::vector<std::string> generations(const std::string& id);

// Puts a generation back. The live saves become a generation of their own
// first, and are replaced only once the replacement is complete and on disk:
// every failure here leaves the game exactly as it was, and says so.
void restore(const std::string& id, const std::string& generation);

}  // namespace kg::session
