#include "anim.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "imgui_internal.h"

namespace kg::gui {

namespace {

// -1 until read; then 0 or 1.
int g_reduce = -1;

// What a window's storage holds for a key never set: a value no widget can
// have, so the first frame is told apart from a value that happens to be 0.
constexpr float kUnset = -FLT_MAX;

ImGuiID salted(ImGuiID key, ImGuiID salt) { return key ^ (salt * 0x9E3779B9u); }

}  // namespace

bool reduce_motion() {
  if (g_reduce < 0) {
    const char* v = std::getenv("KRETRO_REDUCE_MOTION");
    g_reduce = v && *v && std::strcmp(v, "0") != 0 ? 1 : 0;
  }
  return g_reduce == 1;
}

void set_reduce_motion(bool on) { g_reduce = on ? 1 : 0; }

float anim_dt() {
  if (!ImGui::GetCurrentContext()) return 0.0f;
  return std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.05f);
}

float ease_out_cubic(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const float u = 1.0f - t;
  return 1.0f - u * u * u;
}

float ease_in_out_cubic(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  if (t < 0.5f) return 4.0f * t * t * t;
  const float u = -2.0f * t + 2.0f;
  return 1.0f - u * u * u * 0.5f;
}

float approach(float current, float target, float dt, float tau, float snap) {
  if (tau <= 0.0f) return target;
  if (dt <= 0.0f) return current;
  const float next = current + (target - current) * (1.0f - std::exp(-dt / tau));
  return std::fabs(target - next) <= snap ? target : next;
}

float spring(float current, float target, float& velocity, float dt, float tau) {
  if (tau <= 0.0f) {
    velocity = 0.0f;
    return target;
  }
  if (dt <= 0.0f) return current;
  // The exact step of a critically damped oscillator, which stays stable at
  // any frame time where a plain Euler step would not.
  const float w = 2.0f / tau;
  const float x = current - target;
  const float e = std::exp(-w * dt);
  const float temp = (velocity + w * x) * dt;
  velocity = (velocity - w * temp) * e;
  return target + (x + temp) * e;
}

float anim_to(ImGuiID key, float target, float tau) {
  if (!ImGui::GetCurrentContext() || !ImGui::GetCurrentWindowRead()) return target;
  ImGuiStorage* st = ImGui::GetStateStorage();
  float* v = st->GetFloatRef(salted(key, 1), kUnset);
  if (*v == kUnset || reduce_motion()) *v = target;
  else *v = approach(*v, target, anim_dt(), tau);
  return *v;
}

float anim01(ImGuiID key, bool on, float dur) {
  if (!ImGui::GetCurrentContext() || !ImGui::GetCurrentWindowRead()) return on ? 1.0f : 0.0f;
  ImGuiStorage* st = ImGui::GetStateStorage();
  float* p = st->GetFloatRef(salted(key, 2), kUnset);
  if (*p == kUnset || reduce_motion() || dur <= 0.0f) {
    *p = on ? 1.0f : 0.0f;
  } else {
    const float step = anim_dt() / dur;
    *p = std::clamp(*p + (on ? step : -step), 0.0f, 1.0f);
  }
  return ease_out_cubic(*p);
}

float fade_in(ImGuiID key, bool restart, float dur) {
  if (!ImGui::GetCurrentContext() || !ImGui::GetCurrentWindowRead()) return 1.0f;
  ImGuiStorage* st = ImGui::GetStateStorage();
  float* p = st->GetFloatRef(salted(key, 3), kUnset);
  if (*p == kUnset || restart) *p = 0.0f;
  if (reduce_motion() || dur <= 0.0f) *p = 1.0f;
  else *p = std::min(1.0f, *p + anim_dt() / dur);
  return ease_out_cubic(*p);
}

}  // namespace kg::gui
