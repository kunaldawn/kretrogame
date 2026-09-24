// Case-insensitive paths against a real directory listing. What a disc calls
// GAME.EXE a manifest may call Game.exe, and Wine agrees with both, so a path
// a manifest names is looked up one component at a time, ignoring case.
#pragma once

#include <filesystem>
#include <string_view>

namespace kg {

// The real path of `rel` under `root`, matching each component ignoring case,
// or an empty path when some component is missing. `rel` may use '/' or '\'
// and may repeat them; empty components are skipped, so an empty `rel` is
// `root` itself.
std::filesystem::path resolve_ci(const std::filesystem::path& root, std::string_view rel);

// Whether resolve_ci finds `rel` under `root`.
bool exists_ci(const std::filesystem::path& root, std::string_view rel);

}  // namespace kg
