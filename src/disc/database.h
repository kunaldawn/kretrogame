// Naming a disc from the shipped disc database.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../rt/env.h"
#include "../util/hash.h"
#include "iso.h"

namespace kg::iso {

// An entry in the shipped disc database.
struct Known {
  Hash prefix{};
  uint64_t size = 0;
  std::string volume_id;
  std::string set_id;   // which multi-disc set this belongs to, if any
  int disc_no = 1;      // its position in that set
  std::string name;
};

// Loads db/discs.txt from the runtime, or from the checkout when running from
// one. Missing is not an error - it only means we cannot name a disc.
std::vector<Known> load_database(const rt::Env& e);

// Parses one database file. Split out from load_database so it can be tested
// without a runtime. Accepts both the four-column v1 rows and the six-column
// v2 rows; a v1 row is disc 1 of an unnamed set.
std::vector<Known> load_database_file(const std::filesystem::path& p);

// Names the disc, or an empty optional. Matching is by size and prefix hash;
// a size match with the right volume id but a different hash is reported
// separately, because that is what a bad dump looks like.
struct Match {
  const Known* entry = nullptr;
  bool exact = false;
  bool suspect_bad_dump = false;
};
Match identify(const std::vector<Known>& db, const Info& info);

}  // namespace kg::iso
