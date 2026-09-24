// What kretro's shelf and a player's launcher draw alike: the page chrome, the
// tile a game is shown as, and the small pieces every page uses - coloured
// sentences and text fields over a std::string.
//
// Both are Dear ImGui over an SDL renderer, with the runtime's own fonts and
// one palette, so a player an author built looks like the kretro they built it
// with. Everything here is drawing; neither program's pages are. The window
// itself is window.h, and pictures are texture.h.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "imgui.h"
#include "texture.h"

namespace kg::gui {

// The page chrome every full-screen screen shares: a big title, a hint at the
// right margin, a rule.
//
// Callers must call ImGui::End() on every path out, including the early
// returns an empty state takes.
void begin_page(const char* title, const char* hint, ImFont* big);

// begin_page, with the End() it needs on every path out done by the
// destructor. A page that throws skips End, as a page calling begin_page by
// hand does: the throw is caught by the caller and ImGui's error recovery ends
// the window at EndFrame, and an End from here on the way out would end
// whatever window happens to be current then.
class PageWindow {
 public:
  PageWindow(const char* title, const char* hint, ImFont* big);
  ~PageWindow();
  PageWindow(const PageWindow&) = delete;
  PageWindow& operator=(const PageWindow&) = delete;

 private:
  int uncaught_;
};

// A colour derived from the game's own name, so a game with no picture yet
// still gets a tile that is recognisably its own rather than a grey box.
ImU32 tile_colour(const std::string& id, float mul = 1.0f);

// One game as a tile: its picture, or its colour and name in large type; a
// caption strip with the name and `sub` under it; a ring when focused; and
// darkened by `dim`, an alpha. The whole tile is one focusable button, which is what
// a gamepad moves between. True on the frame it is activated. `focused`, when
// given, is set to whether it has the keyboard or pad focus.
bool tile(const std::string& id, const std::string& name, const std::string& sub,
          const Texture* art, ImFont* big, float w, float h, uint8_t dim = 0,
          bool* focused = nullptr);

// A wrapped sentence in one colour.
void colored_text(const ImVec4& colour, const std::string& s);
// In palette.h's kWarn and kGood.
void warn_text(const std::string& s);
void good_text(const std::string& s);

// There is no imgui_stdlib in this tree. ImGui keeps its own copy of a field
// while it is being edited, so a buffer made fresh each frame from the string
// is enough, and the string is only written when the text changed. `cap`
// counts the terminating zero, so a field holds cap - 1 bytes.
bool input_string(const char* label, std::string& s, size_t cap, ImGuiInputTextFlags flags = 0);
bool input_string_multiline(const char* label, std::string& s, size_t cap, ImVec2 size);

// Where a file browser opens: the person's home, from HOME, or "/" when HOME
// is unset or empty. Not kg::home_dir(), which is kretro's per-game HOME under
// its state directory; inside kg::gui this name hides that one, so call that
// one qualified.
std::filesystem::path home_dir();

}  // namespace kg::gui
