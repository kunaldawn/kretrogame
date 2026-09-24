// One screen of a window, and the router that says which one is up.
//
// The shelf and a player's launcher are both a handful of screens, one of
// which is drawn each frame. A Page is one of them; Pages holds one of each,
// indexed by the host's own enum, and knows which is current.
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>

namespace kg::gui {

class Page {
 public:
  virtual ~Page() = default;
  // Draws itself, Begin to End.
  virtual void draw() = 0;
  // Escape, and pad B in the launcher. True when the page used it.
  virtual bool back() { return false; }
  // A file dropped on the window. True when the page used it.
  virtual bool dropped(const std::filesystem::path&) { return false; }
  // Something is running that leaving the page would walk away from.
  virtual bool busy() const { return false; }
};

// No enter() or leave() hooks, deliberately: switching page runs no code
// today, and a hook would be a place for it to start.
template <class Id, std::size_t N>
class Pages {
 public:
  void set(Id id, Page& p) { pages_[static_cast<std::size_t>(id)] = &p; }
  void go(Id id) { current_ = id; }
  Id current() const { return current_; }
  bool at(Id id) const { return current_ == id; }
  Page& page() const { return *pages_[static_cast<std::size_t>(current_)]; }

 private:
  std::array<Page*, N> pages_{};
  Id current_{};
};

}  // namespace kg::gui
