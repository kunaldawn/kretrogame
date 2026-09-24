// A job on a thread of its own, and the log it writes, so the window keeps
// drawing through the minutes a scan, an install or an unpack may take.
//
// The shelf, the wizard and a player's launcher each had their own copy of
// this; they differed only in what they did around it, and that stays with
// them. Clearing a cancel request when a job starts, what happens once a job
// is over and what a page says about a failure are the caller's.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kg::gui {

class Job {
 public:
  Job() = default;
  // Joins. The body captures its owner, so nothing may outlive the thread.
  ~Job();
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;

  // False, and nothing started, while a job is running. Otherwise the log and
  // the last error are cleared and `fn` starts on the thread. What it throws
  // becomes error() and a "failed: ..." line in the log.
  bool start(std::string title, std::function<void()> fn);

  // Whether a job was started and not yet joined. The UI thread's own flag.
  bool running() const { return busy_; }
  // Whether the body has returned. Set by the worker, read by the UI thread.
  bool finished() const { return done_; }
  // Waits for the worker, clears running(), and returns what the body threw,
  // or an empty string. Safe to call with no thread to join.
  std::string join();

  void log(std::string line);
  // A copy of the log, taken under the lock the worker writes it under.
  std::vector<std::string> lines() const;
  // The newest line, or nothing when the log is empty.
  std::optional<std::string> last_line() const;
  // What the last job threw, copied: a page may read it while the worker is
  // still running, and a reference would be a torn read.
  std::string error() const;

  // A request the body may look at. Starting a job does not clear it.
  void cancel() { cancel_ = true; }
  void clear_cancel() { cancel_ = false; }
  bool cancelled() const { return cancel_; }

  const std::string& title() const { return title_; }

  // Runs `f` under the lock the log is written under, so a body can publish
  // its results to the UI thread the same way it publishes its lines.
  template <class F>
  auto locked(F&& f) {
    std::lock_guard<std::mutex> lk(mu_);
    return f();
  }

 private:
  std::thread worker_;
  mutable std::mutex mu_;
  std::vector<std::string> log_;
  std::string error_;
  std::string title_;
  std::atomic<bool> done_{false};
  std::atomic<bool> cancel_{false};
  bool busy_ = false;
};

}  // namespace kg::gui
