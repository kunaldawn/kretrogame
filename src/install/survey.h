// Reading what an install left behind: where the game went, which executable
// runs it, and which files prove a later rebuild found the same game.
//
// These are the parts of the wizard that can be wrong in a way nobody would
// notice until a rebuild fails, so they live here, where the test binaries can
// reach them, rather than in src/gui.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../pack/tree.h"
#include "draft.h"

namespace kg::install {

// One directory an install might be: where the installer wrote, or the tree a
// copy or unzip extracted.
struct Candidate {
  std::filesystem::path dir;                       // relative to installed_root()
  size_t files = 0;
  uint64_t bytes = 0;
  std::vector<std::filesystem::path> executables;  // ranked
};

// "SafeDisc" or "SecuROM" for a file only those protections put beside a game,
// and nothing for any other name; any case. The wizard names what it finds on
// its build step and the Bundles page puts it on its check list, and one list
// of signs keeps the two from disagreeing about the same game.
std::string protection_of(std::string_view filename);

// Every directory the diff added, deepest first then by file count. Counts are
// cumulative over the subtree, which is why a game directory and its parent
// show the same number.
std::vector<Candidate> rank_candidates(const std::filesystem::path& drive_c,
                                       const Tree::Diff& d);

// The one candidate a copy or unzip install has: the extracted tree itself,
// counted, with its executables ranked the same way an installed directory's
// are. Its `dir` is empty, because the tree is the game's directory and there
// is nothing under it to choose between.
Candidate survey_tree(const std::filesystem::path& tree);

// Which entry of a ranked executable list the draft already names, or the list
// size when it names none. The mirror of setup_index, and there for the same
// reason: a manifest that says Gamew.exe is saying which of two executables
// is the game, and ranking by size answers GAME.EXE - the DOS build.
size_t exe_index(const std::vector<std::filesystem::path>& exes,
                 const std::filesystem::path& want);

// Executables in `dir`, ranked, relative to `dir`. `prefer_setup` is the disc
// side: it also looks one directory down and puts setup, then install, then
// autorun first. Uninstallers are last on both sides, whatever their size.
std::vector<std::filesystem::path> rank_executables(const std::filesystem::path& dir,
                                                    bool prefer_setup);

// Entries under a directory, counted recursively - files and directories both,
// because a count of entries under drive_c is what a person watching an
// installer sees moving. Zero for a directory that is not there.
size_t count_entries(const std::filesystem::path& dir);

// At most five entries: the executable, then the largest regular files at the
// top level. Every entry is a requirement detect_install_dir must satisfy
// simultaneously, so a hundred-entry list would make a rebuild fragile for no
// gain.
std::vector<std::string> verify_list(const std::filesystem::path& dir,
                                     const std::filesystem::path& exe);

// The list a draft's pack should carry. The one the draft already has, when it
// has one: step 6 reads it off the confirmed directory the moment the user
// picks the executable, which is the last moment the tree is exactly what they
// were looking at. Otherwise one read now, off `installed_root` joined with the
// draft's install_dir - which for a copy or unzip install is the tree itself,
// because there the tree is the game.
//
// One answer rather than two. Build::write takes the draft's list through
// draft_to_meta and computes one only when it is empty, so the field is never
// filled, carried and thrown away on arrival.
std::vector<std::string> verify_for(const Draft& d,
                                    const std::filesystem::path& installed_root);

// Where a recipe's game landed under drive_c: the first candidate, in ranked
// order, that holds every verify entry at once. Empty when none does, and
// empty when there is no verify list to hold.
std::filesystem::path find_install_dir(const std::vector<Candidate>& candidates,
                                       const std::filesystem::path& drive_c,
                                       const std::vector<std::string>& verify);

}  // namespace kg::install
