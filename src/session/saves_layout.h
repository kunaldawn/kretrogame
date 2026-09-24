// Where a game's saves live, under game_saves_dir(id): live/ is the writable
// layer, gen/NNNN the snapshots, journal/ one JSON file per session and the
// frames the compositor harvested. This is the one definition of that layout.
#pragma once

#include <filesystem>
#include <string>

namespace kg::session {

std::filesystem::path journal_dir(const std::string& id);
std::filesystem::path live_dir(const std::string& id);
std::filesystem::path generations_dir(const std::string& id);

// The game's tile art in a frames directory, and the mark an install leaves
// beside it when the tile is only a stand-in (see set_title_art).
std::filesystem::path title_art_file(const std::filesystem::path& frames_dir);
std::filesystem::path title_marker_file(const std::filesystem::path& frames_dir);
// The most recent frame the compositor harvested, in a frames directory.
std::filesystem::path last_frame_file(const std::filesystem::path& frames_dir);

}  // namespace kg::session
