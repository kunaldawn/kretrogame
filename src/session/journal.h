// A game's timeline: one record per session, in journal/<started>.json.
#pragma once

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

namespace kg::session {

// One entry in a game's timeline.
struct Record {
  std::time_t started = 0;
  std::time_t ended = 0;
  std::string runtime_id;
  std::string note;
  std::string screenshot;   // relative to the game's journal directory
  size_t files_written = 0;
  int status = 0;
  std::string generation;   // the snapshot this session produced, if any
};

// The timeline, newest first.
std::vector<Record> journal(const std::string& id);
void write_record(const std::string& id, const Record& r);

}  // namespace kg::session
