// What kretro's shelf and a player's launcher draw alike: the window and its
// look, the page chrome, pictures, and the tile a game is shown as.
//
// Both are Dear ImGui over an SDL renderer, with the runtime's own fonts and
// one palette, so a player an author built looks like the kretro they built it
// with. Everything here is drawing; neither program's pages are.
#pragma once

#include <SDL.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../rt/env.h"
#include "imgui.h"

namespace kg::gui {

// The page chrome every full-screen screen shares: a big title, a hint at the
// right margin, a rule.
//
// Callers must call ImGui::End() on every path out, including the early
// returns an empty state takes.
void begin_page(const char* title, const char* hint, ImFont* big);

// A colour derived from the game's own name, so a game with no picture yet
// still gets a tile that is recognisably its own rather than a grey box.
ImU32 tile_colour(const std::string& id, float mul = 1.0f);

struct Texture {
  SDL_Texture* tex = nullptr;
  int w = 0, h = 0;
};

// PNGs turned into textures once and kept, from a file or from bytes held in
// memory - a player's covers and banner are inside bundle.meta, not on disk.
class Textures {
 public:
  explicit Textures(SDL_Renderer* r) : ren_(r) {}
  ~Textures();
  Textures(const Textures&) = delete;
  Textures& operator=(const Textures&) = delete;

  const Texture* file(const std::filesystem::path& p);
  // `key` names the picture in the cache; the bytes are decoded only the
  // first time it is asked for.
  const Texture* png(const std::string& key, const std::string& bytes);
  // Drops every picture whose key contains `needle`, so a new screenshot of a
  // game is loaded rather than the one from before it was played.
  //
  // Dropped from the cache at once, destroyed a frame later. It is called
  // from the middle of a frame - Play returns there, after the game - and
  // that frame's draw list already names the texture, to be drawn when the
  // frame ends. Destroyed here, the frame drew a freed texture.
  void forget_matching(const std::string& needle);
  // Destroys what forget_matching dropped in a frame before this one. Every
  // lookup does it; it is public for a caller with nothing to look up.
  void collect();

 private:
  const Texture* decoded(const std::string& key, const unsigned char* data, int len, bool from_file);
  SDL_Renderer* ren_;
  std::map<std::string, Texture> cache_;
  // Dropped, and the ImGui frame they were dropped in.
  std::vector<std::pair<SDL_Texture*, int>> retired_;
};

// One game as a tile: its picture, or its colour and name in large type; a
// caption strip with the name and `sub` under it; a ring when focused; and
// darkened by `dim`, an alpha. The whole tile is one focusable button, which is what
// a gamepad moves between. True on the frame it is activated. `focused`, when
// given, is set to whether it has the keyboard or pad focus.
bool tile(const std::string& id, const std::string& name, const std::string& sub,
          const Texture* art, ImFont* big, float w, float h, uint8_t dim = 0,
          bool* focused = nullptr);

// The window both programs draw in.
struct Window {
  SDL_Window* win = nullptr;
  SDL_Renderer* ren = nullptr;
  ImFont* body = nullptr;
  ImFont* big = nullptr;
};

struct WindowSpec {
  const char* title = "kretro";
  int x = -1, y = -1;  // -1 is centred
  uint32_t width = 1280, height = 800;
  bool borderless = false;
  bool fullscreen = false;
};

// SDL, the window, a renderer (accelerated if the driver allows, software if
// not - a window must come up on a machine whose graphics are broken, because
// that is when somebody needs to read what is wrong), ImGui with kretro's look
// and the runtime's fonts, and every gamepad already plugged in. Empty, with
// `why` set, when there is no window to be had.
std::optional<Window> open_window(const rt::Env& e, const WindowSpec& spec, std::string* why);
void begin_frame(const Window& w);
void end_frame(const Window& w);
// ImGui first, then SDL: the reverse of how they were opened.
void close_window(Window& w);

// Whether this process has a Wayland or X11 session to draw on at all.
bool have_display();

}  // namespace kg::gui
