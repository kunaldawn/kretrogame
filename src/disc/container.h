// What is actually inside this file.
//
// A download from an archive is rarely the disc. It is a zip holding three
// zips, each holding a .bin and a .cue. No disc image is extracted until
// somebody asks for its bytes, so probing a 6.8 GB DVD costs a directory
// listing. Descending into a nested archive does copy that archive out, which
// is unavoidable: 7z cannot list through two layers.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../rt/env.h"

namespace kg::disc {

// How far down to look. Three is enough for the deepest real case in the
// collection (zip -> zip -> bin/cue) with one to spare, and it stops a zip bomb
// from turning a library scan into an afternoon.
inline constexpr int kMaxDepth = 3;

struct Member {
  std::string path;
  uint64_t size = 0;
};

// Parses `7z l -slt -ba` output. Directories are included; callers filter them
// by name, because 7z's Attributes field is not consistent across formats.
std::vector<Member> parse_7z_listing(std::string_view out);

bool is_disc_image(std::string_view name);
bool is_archive(std::string_view name);

struct Candidate {
  std::filesystem::path archive;      // the file in the collection
  std::vector<std::string> members;   // path within, outermost first; empty means the file is the image
  std::string name;                   // basename of the innermost element
  uint64_t size = 0;
  bool is_cue = false;                // a cue sheet, which needs its .bin siblings too
};

// Every disc image reachable from `p`, descending into nested archives.
std::vector<Candidate> probe(const rt::Env& e, const std::filesystem::path& p);

// Extracts a candidate into `work` and returns the path to the innermost file.
// For a cue sheet, every file the sheet names is extracted alongside it.
std::filesystem::path materialise(const rt::Env& e, const Candidate& c,
                                  const std::filesystem::path& work);

}  // namespace kg::disc
