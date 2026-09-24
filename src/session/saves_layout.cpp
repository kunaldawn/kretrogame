#include "saves_layout.h"

#include "../util/paths.h"

namespace kg::session {
namespace fs = std::filesystem;

fs::path journal_dir(const std::string& id) { return game_saves_dir(id) / "journal"; }
fs::path generations_dir(const std::string& id) { return game_saves_dir(id) / "gen"; }
fs::path live_dir(const std::string& id) { return game_saves_dir(id) / "live"; }

fs::path title_art_file(const fs::path& frames_dir) { return frames_dir / "title.png"; }
fs::path title_marker_file(const fs::path& frames_dir) { return frames_dir / "title.provisional"; }
fs::path last_frame_file(const fs::path& frames_dir) { return frames_dir / "last.png"; }

}  // namespace kg::session
