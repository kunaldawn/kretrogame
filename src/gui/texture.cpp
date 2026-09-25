#include "texture.h"

#include <algorithm>
#include <cmath>

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
  int w = 0, h = 0, n = 0;
  unsigned char* px = from_file ? stbi_load(key.c_str(), &w, &h, &n, 4)
                                : stbi_load_from_memory(data, len, &w, &h, &n, 4);
  const Texture* t = upload(key, px, w, h, false);
  if (px) stbi_image_free(px);
  return t;
}

const Texture* Textures::upload(const std::string& key, const unsigned char* rgba, int w, int h, bool linear) {
  Texture t;
  if (rgba && w > 0 && h > 0) {
    t.w = w;
    t.h = h;
    t.tex = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, t.w, t.h);
    if (t.tex) {
      SDL_UpdateTexture(t.tex, nullptr, rgba, t.w * 4);
      SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
      if (linear) SDL_SetTextureScaleMode(t.tex, SDL_ScaleModeLinear);
    }
  }
  // A picture that would not decode is remembered as none, so it is not
  // decoded again sixty times a second.
  cache_[key] = t;
  return t.tex ? &cache_[key] : nullptr;
}

const Texture* Textures::blurred(const std::string& key, const std::string& source, const unsigned char* data, int len,
                                 bool from_file) {
  int w = 0, h = 0, n = 0;
  unsigned char* px = from_file ? stbi_load(source.c_str(), &w, &h, &n, 4)
                                : stbi_load_from_memory(data, len, &w, &h, &n, 4);
  int bw = 0, bh = 0;
  std::vector<unsigned char> out;
  if (px) {
    out = backdrop_pixels(px, w, h, 160, 3, &bw, &bh);
    stbi_image_free(px);
  }
  return upload(key, out.empty() ? nullptr : out.data(), bw, bh, true);
}

const Texture* Textures::backdrop_file(const fs::path& p) {
  if (!retired_.empty()) collect();
  if (p.empty()) return nullptr;
  const std::string key = "backdrop:" + p.string();
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second.tex ? &it->second : nullptr;
  return blurred(key, p.string(), nullptr, 0, true);
}

const Texture* Textures::backdrop_png(const std::string& key, const std::string& bytes) {
  if (!retired_.empty()) collect();
  if (bytes.empty()) return nullptr;
  const std::string k = "backdrop:" + key;
  auto it = cache_.find(k);
  if (it != cache_.end()) return it->second.tex ? &it->second : nullptr;
  return blurred(k, key, reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), false);
}

std::vector<unsigned char> backdrop_pixels(const unsigned char* rgba, int w, int h, int max_w, int radius, int* out_w,
                                           int* out_h) {
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!rgba || w <= 0 || h <= 0) return {};
  // Down by a whole factor, each output pixel the average of its block: far
  // cheaper than blurring the full picture, and a blur hides the blockiness.
  const int f = std::max(1, (w + std::max(1, max_w) - 1) / std::max(1, max_w));
  const int ow = std::max(1, w / f), oh = std::max(1, h / f);
  std::vector<float> img(static_cast<size_t>(ow) * oh * 4, 0.0f);
  for (int y = 0; y < oh; ++y) {
    for (int x = 0; x < ow; ++x) {
      float sum[4] = {0, 0, 0, 0};
      int n = 0;
      for (int by = y * f; by < std::min(h, (y + 1) * f); ++by) {
        for (int bx = x * f; bx < std::min(w, (x + 1) * f); ++bx) {
          const unsigned char* p = rgba + (static_cast<size_t>(by) * w + bx) * 4;
          for (int c = 0; c < 4; ++c) sum[c] += static_cast<float>(p[c]);
          ++n;
        }
      }
      for (int c = 0; c < 4; ++c) img[(static_cast<size_t>(y) * ow + x) * 4 + c] = sum[c] / static_cast<float>(n);
    }
  }
  // Box blurs, across and then down, the edges held at their last pixel so
  // the border does not darken.
  std::vector<float> tmp(img.size());
  auto pass = [&](bool across) {
    const int len = across ? ow : oh, lines = across ? oh : ow;
    for (int l = 0; l < lines; ++l) {
      for (int i = 0; i < len; ++i) {
        float sum[4] = {0, 0, 0, 0};
        for (int k = -radius; k <= radius; ++k) {
          const int j = std::clamp(i + k, 0, len - 1);
          const size_t at = across ? (static_cast<size_t>(l) * ow + j) : (static_cast<size_t>(j) * ow + l);
          for (int c = 0; c < 4; ++c) sum[c] += img[at * 4 + c];
        }
        const size_t to = across ? (static_cast<size_t>(l) * ow + i) : (static_cast<size_t>(i) * ow + l);
        for (int c = 0; c < 4; ++c) tmp[to * 4 + c] = sum[c] / static_cast<float>(radius * 2 + 1);
      }
    }
    img.swap(tmp);
  };
  for (int i = 0; i < 3 && radius > 0; ++i) {
    pass(true);
    pass(false);
  }
  std::vector<unsigned char> out(img.size());
  for (size_t i = 0; i < img.size(); ++i) out[i] = static_cast<unsigned char>(std::clamp(std::lround(img[i]), 0L, 255L));
  if (out_w) *out_w = ow;
  if (out_h) *out_h = oh;
  return out;
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
