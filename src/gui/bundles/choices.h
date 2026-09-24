// The Bundles page's small pieces of wording and choice, shared by its steps.
// Internal to the page: nothing outside src/gui/bundles/ names this namespace.
#pragma once

#include <array>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

#include "../../bundle/meta.h"
#include "../format.h"

// The implementation is compiled into texture.cpp; this is only the
// declarations, for reading a picture's size without drawing it.
#include "stb_image.h"

namespace kg::gui::bundles_detail {

// The words shown for bundle::kBackendNames and bundle::kDisplayModes, index
// for index.
inline const char* const kBackendWords[] = {"auto", "DXVK", "WineD3D on Vulkan", "WineD3D on OpenGL", "cnc-ddraw"};
inline const char* const kDisplayWords[] = {"integer scaling", "fit the screen", "native size"};
static_assert(std::size(kBackendWords) == bundle::kBackendNames.size());
static_assert(std::size(kDisplayWords) == bundle::kDisplayModes.size());

inline const char* step_word(int i) {
  static const char* w[] = {"1  Identity", "2  Games", "3  Each game", "4  Check",
                            "5  Size", "6  Rights", "7  Build", "8  Preview"};
  return w[i];
}

inline bool is_png(const std::string& b) {
  return b.size() > 8 && b.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0;
}

// "1280x400 PNG, 120 KB", or why it is not a picture.
inline std::string picture_words(const std::string& png) {
  int w = 0, h = 0, n = 0;
  if (!stbi_info_from_memory(reinterpret_cast<const unsigned char*>(png.data()), static_cast<int>(png.size()), &w, &h,
                             &n)) {
    return "not a picture this kretro can read";
  }
  return std::to_string(w) + "x" + std::to_string(h) + " PNG, " + human_size(png.size());
}

template <size_t N>
int index_of(const std::array<std::string_view, N>& set, const std::string& v) {
  for (size_t i = 0; i < N; ++i) {
    if (v == set[i]) return static_cast<int>(i);
  }
  return 0;
}

}  // namespace kg::gui::bundles_detail
