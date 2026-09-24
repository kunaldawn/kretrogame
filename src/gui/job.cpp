#include "job.h"

#include <exception>
#include <utility>

namespace kg::gui {

Job::~Job() {
  if (worker_.joinable()) worker_.join();
}

bool Job::start(std::string title, std::function<void()> fn) {
  if (busy_) return false;
  busy_ = true;
  title_ = std::move(title);
  {
    std::lock_guard<std::mutex> lk(mu_);
    error_.clear();
    log_.clear();
  }
  done_ = false;
  worker_ = std::thread([this, fn = std::move(fn)] {
    try {
      fn();
    } catch (const std::exception& ex) {
      std::lock_guard<std::mutex> lk(mu_);
      error_ = ex.what();
      log_.push_back(std::string("failed: ") + ex.what());
    }
    done_ = true;
  });
  return true;
}

std::string Job::join() {
  if (worker_.joinable()) worker_.join();
  busy_ = false;
  return error();
}

void Job::log(std::string line) {
  std::lock_guard<std::mutex> lk(mu_);
  log_.push_back(std::move(line));
}

std::vector<std::string> Job::lines() const {
  std::lock_guard<std::mutex> lk(mu_);
  return log_;
}

std::optional<std::string> Job::last_line() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (log_.empty()) return std::nullopt;
  return log_.back();
}

std::string Job::error() const {
  std::lock_guard<std::mutex> lk(mu_);
  return error_;
}

}  // namespace kg::gui
