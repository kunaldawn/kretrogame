#include "scale.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace kg::gui {

namespace {

// One window per process, drawn on one thread, so the factor and the fonts
// are plain globals rather than something threaded through every page.
float g_scale = 1.0f;
float g_zoom = 1.0f;
// The automatic factor the zoom multiplies, which decides how far the zoom
// can go before the scale reaches its own limits.
float g_zoom_base = 1.0f;
ImFont* g_fonts[static_cast<int>(FontRole::Count)] = {};

constexpr float kZoomMin = 0.5f, kZoomMax = 3.0f, kZoomStep = 0.1f;

// The scale itself stops at 0.5 and 4 (quantize_ui_scale), so a zoom past
// either end of that for the current base would change nothing on screen, and
// the next step back would change nothing either. The limits are on whole
// tenths inside that range, and never exclude 100%.
float zoom_max() { return std::clamp(std::floor(4.0f / g_zoom_base * 10.0f) / 10.0f, 1.0f, kZoomMax); }
float zoom_min() { return std::clamp(std::ceil(0.5f / g_zoom_base * 10.0f) / 10.0f, kZoomMin, 1.0f); }

// Rounded to the nearest tenth, so ten steps up and ten down land exactly
// where they started rather than a float's worth off it.
void set_zoom(float z) { g_zoom = std::clamp(std::round(z * 10.0f) / 10.0f, zoom_min(), zoom_max()); }

}  // namespace

float ui_scale() { return g_scale; }
float px(float design_px) { return design_px * g_scale; }
ImVec2 px(float x, float y) { return ImVec2(x * g_scale, y * g_scale); }
void set_ui_scale(float s) {
  if (s > 0 && std::isfinite(s)) g_scale = s;
}

float ui_zoom() { return g_zoom; }
// A zoom left past the limits by a base that has since grown (a window
// dragged to a bigger screen) steps from the limit, so the first press shows.
void zoom_in() { set_zoom(std::min(g_zoom, zoom_max()) + kZoomStep); }
void zoom_out() { set_zoom(std::min(g_zoom, zoom_max()) - kZoomStep); }
void zoom_reset() { g_zoom = 1.0f; }
void set_zoom_base(float base) {
  if (base > 0 && std::isfinite(base)) g_zoom_base = base;
}

std::optional<float> forced_ui_scale() {
  static const std::optional<float> forced = []() -> std::optional<float> {
    const char* v = std::getenv("KRETRO_UI_SCALE");
    if (!v || !*v) return std::nullopt;
    char* end = nullptr;
    float f = std::strtof(v, &end);
    if (end == v || !std::isfinite(f) || f <= 0) return std::nullopt;
    return std::clamp(f, 0.5f, 4.0f);
  }();
  return forced;
}

float auto_ui_scale(float logical_w, float logical_h, float fb_scale, float ddpi) {
  if (logical_w <= 0 || logical_h <= 0) return 1.0f;
  float fit = std::clamp(std::min(logical_w / 1280.0f, logical_h / 800.0f), 0.75f, 3.0f);
  // A window only a little short of the design - a 1366x768 laptop panel, a
  // maximised window under a desktop's panels - stays at 100%: shrinking
  // every glyph for the sake of a few dozen rows costs more than it gains.
  fit = std::max(fit, std::min({1.0f, logical_w / 1280.0f, logical_h / 720.0f}));
  // On a dense screen the smallest the fit allows is physically smaller than
  // on an ordinary one, so the floor rises with the density: text is never
  // physically smaller than 75% is on a 96 dpi screen. A floor of the
  // density itself would stop a small window on an ordinary screen shrinking
  // at all. A scaled framebuffer already makes a logical pixel physically
  // larger, so there the density would count twice.
  //
  // The floor still leaves the window a design area of at least 1024x640,
  // which the pages are laid out to hold; a small dense screen (a handheld)
  // gets larger text than a small ordinary one, but not so large that its
  // pages stack or overflow.
  if (fb_scale <= 1.0f && ddpi > 0 && std::isfinite(ddpi)) {
    const float room = std::max(0.75f, std::min(logical_w / 1024.0f, logical_h / 640.0f));
    fit = std::max(fit, std::min(0.75f * std::clamp(ddpi / 96.0f, 1.0f, 3.0f), room));
  }
  return fit;
}

float quantize_ui_scale(float s) {
  if (!(s > 0) || !std::isfinite(s)) return 1.0f;
  return std::clamp(std::round(s * 16.0f) / 16.0f, 0.5f, 4.0f);
}

ImFont* font(FontRole r) {
  ImFont* f = g_fonts[static_cast<int>(r)];
  return f ? f : ImGui::GetFont();
}

void set_font(FontRole r, ImFont* f) { g_fonts[static_cast<int>(r)] = f; }

float font_px(ImFont* f) {
  if (!f) return ImGui::GetFontSize();
  return f->FontSize * ImGui::GetIO().FontGlobalScale;
}

Grid tile_grid(float avail_w, float min_w, float aspect, float gap) {
  Grid g;
  g.gap = gap < 0 ? px(16.0f) : gap;
  min_w = std::max(min_w, 1.0f);
  avail_w = std::max(avail_w, 1.0f);
  g.cols = std::max(1, static_cast<int>((avail_w + g.gap) / (min_w + g.gap)));
  // Floored, so the row's rounding never adds up to a pixel more than the
  // row has and pushes the last tile past the edge.
  g.w = std::floor((avail_w - g.gap * static_cast<float>(g.cols - 1)) / static_cast<float>(g.cols));
  g.w = std::max(g.w, 1.0f);
  g.h = std::floor(g.w * aspect);
  return g;
}

}  // namespace kg::gui
