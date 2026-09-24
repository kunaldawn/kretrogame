#include "texture.h"

#include "imgui.h"

// stb_image's one definition in each program that draws: this file is in
// both kretro's and the player's GUI objects, and nothing else defines it.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace kg::gui {
namespace fs = std::filesystem;

Textures::~Textures() {
  for (auto& [k, t] : cache_) {
    if (t.tex) SDL_DestroyTexture(t.tex);
  }
  for (auto& [tex, frame] : retired_) SDL_DestroyTexture(tex);
}

namespace {
// The frame being built, or -1 with no ImGui to build one: then there is no
// draw list that could still name a texture.
int imgui_frame() { return ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1; }
}  // namespace

void Textures::collect() {
  const int now = imgui_frame();
  for (auto it = retired_.begin(); it != retired_.end();) {
    if (now < 0 || it->second < now) {
      SDL_DestroyTexture(it->first);
      it = retired_.erase(it);
    } else {
      ++it;
    }
  }
}

const Texture* Textures::decoded(const std::string& key, const unsigned char* data, int len,
                                 bool from_file) {
  Texture t;
  int n = 0;
  unsigned char* px = from_file ? stbi_load(key.c_str(), &t.w, &t.h, &n, 4)
                                : stbi_load_from_memory(data, len, &t.w, &t.h, &n, 4);
  if (px) {
    t.tex = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, t.w, t.h);
    if (t.tex) {
      SDL_UpdateTexture(t.tex, nullptr, px, t.w * 4);
      SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
    }
    stbi_image_free(px);
  }
  // A picture that would not decode is remembered as none, so it is not
  // decoded again sixty times a second.
  cache_[key] = t;
  return t.tex ? &cache_[key] : nullptr;
}

const Texture* Textures::file(const fs::path& p) {
  if (!retired_.empty()) collect();
  if (p.empty()) return nullptr;
  auto it = cache_.find(p.string());
  if (it != cache_.end()) return it->second.tex ? &it->second : nullptr;
  return decoded(p.string(), nullptr, 0, true);
}

const Texture* Textures::png(const std::string& key, const std::string& bytes) {
  if (!retired_.empty()) collect();
  if (bytes.empty()) return nullptr;
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second.tex ? &it->second : nullptr;
  return decoded(key, reinterpret_cast<const unsigned char*>(bytes.data()),
                 static_cast<int>(bytes.size()), false);
}

void Textures::forget_matching(const std::string& needle) {
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->first.find(needle) != std::string::npos) {
      if (it->second.tex) retired_.emplace_back(it->second.tex, imgui_frame());
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace kg::gui
