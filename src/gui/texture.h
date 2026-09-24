// Pictures as textures: PNGs from a file or from bytes held in memory, decoded
// once and kept for as long as the window is up.
#pragma once

#include <SDL.h>

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace kg::gui {

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

}  // namespace kg::gui
