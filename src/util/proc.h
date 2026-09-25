// Running other programs, with their output in hand.
#pragma once

#include <string>
#include <vector>

namespace kg {

struct ProcResult {
  int status = -1;         // exit status, or -1 if it never ran
  bool signalled = false;  // killed rather than exited
  int signal = 0;
  std::string out;         // stdout and stderr together, when captured
  bool ok() const { return status == 0 && !signalled; }
};

struct ProcOptions {
  std::vector<std::pair<std::string, std::string>> env;  // added to the child's
  std::string cwd;
  bool capture = true;   // false lets the child use our terminal
  int timeout_sec = 0;   // for the whole call; 0 means wait indefinitely
};

// argv[0] is the program path; it is executed directly, not through a shell,
// so nothing here can be confused by a space or a quote in a game's path.
//
// Returns once that program has exited, with what it wrote by then (and in
// the moment after). Processes it leaves running do not hold the call up,
// even though they inherit its output: a Wine command's wineserver and
// services outlive it by seconds, and a daemon for as long as it likes.
ProcResult run(const std::vector<std::string>& argv, const ProcOptions& opt = {});

// fork(), with the child sent `sig` when the thread that forked it ends -
// however it ends: returning, exiting from a signal handler with _exit, or
// killed outright. For the helpers a session starts beside a game, which
// take sessions of their own (setsid) and so are out of reach of a signal
// sent to the group the game is in: without this, a Ctrl-C or a Stop ends
// the player and leaves its compositor on the screen. Tied across exec,
// unless what is executed is set-user-ID. Returns what fork() returns.
int fork_tied(int sig);

}  // namespace kg
