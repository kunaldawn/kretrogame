// What a game wrote, carried between machines: a saves pack made out of its
// live directory, and one put back into it.
#pragma once

#include <filesystem>
#include <string>

namespace kg::session {

// Writes a kind=3 pack holding only what the game wrote - saves, configs.
std::filesystem::path export_saves(const std::string& id, const std::filesystem::path& out);

// Restores what a game wrote, from a saves pack. What is there now is
// snapshotted first, so this is reversible.
void import_saves(const std::string& id, const std::filesystem::path& in);

}  // namespace kg::session
