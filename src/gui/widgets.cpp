#include "widgets.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

#include "palette.h"

namespace kg::gui {
namespace fs = std::filesystem;

void begin_page(const char* title, const char* hint, ImFont* big) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin(title, nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
  ImGui::PushFont(big);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(hint).x - 30);
  ImGui::TextDisabled("%s", hint);
  ImGui::Separator();
  ImGui::Spacing();
}

ImU32 tile_colour(const std::string& id, float mul) {
  uint32_t h = 2166136261u;
  for (char c : id) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
  float hue = static_cast<float>(h % 360) / 360.0f;
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(hue, 0.45f, 0.34f * mul, r, g, b);
  return ImGui::GetColorU32(ImVec4(r, g, b, 1.0f));
}

PageWindow::PageWindow(const char* title, const char* hint, ImFont* big)
    : uncaught_(std::uncaught_exceptions()) {
  begin_page(title, hint, big);
}

PageWindow::~PageWindow() {
  if (std::uncaught_exceptions() == uncaught_) ImGui::End();
}

// ---- the tile ---------------------------------------------------------------

bool tile(const std::string& id, const std::string& name, const std::string& sub,
          const Texture* art, ImFont* big, float w, float h, uint8_t dim, bool* focused_out) {
  ImGui::PushID(id.c_str());
  ImGui::BeginGroup();

  ImVec2 p0 = ImGui::GetCursorScreenPos();
  // An invisible button underneath makes the whole tile focusable, which is
  // what gamepad navigation moves between.
  bool activated = ImGui::InvisibleButton("tile", ImVec2(w, h));
  bool focused = ImGui::IsItemFocused() || ImGui::IsItemHovered();
  if (focused_out) *focused_out = ImGui::IsItemFocused();
  ImVec2 p1 = ImVec2(p0.x + w, p0.y + h);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  if (art) {
    dl->AddImageRounded(reinterpret_cast<ImTextureID>(art->tex), p0, p1, ImVec2(0, 0),
                        ImVec2(1, 1), IM_COL32_WHITE, 8.0f);
  } else {
    dl->AddRectFilled(p0, p1, tile_colour(id), 8.0f);
    // No picture, so the game's name carries the tile.
    ImGui::PushFont(big);
    ImVec2 ts = ImGui::CalcTextSize(name.c_str(), nullptr, false, w - 32);
    dl->PushClipRect(p0, ImVec2(p1.x, p1.y - 52), true);
    dl->AddText(big, big->FontSize, ImVec2(p0.x + 16, p0.y + (h - 52 - ts.y) * 0.5f),
                IM_COL32(255, 255, 255, 220), name.c_str(), nullptr, w - 32);
    dl->PopClipRect();
    ImGui::PopFont();
  }

  // A caption strip, so the tile still says what it is when the art is a
  // screenshot of somewhere deep inside the game.
  dl->AddRectFilled(ImVec2(p0.x, p1.y - 52), p1, IM_COL32(0, 0, 0, 190), 8.0f,
                    ImDrawFlags_RoundCornersBottom);
  dl->PushClipRect(ImVec2(p0.x + 12, p1.y - 52), ImVec2(p1.x - 12, p1.y), true);
  dl->AddText(ImVec2(p0.x + 12, p1.y - 46), IM_COL32(255, 255, 255, 235), name.c_str());
  dl->AddText(ImVec2(p0.x + 12, p1.y - 24), IM_COL32(210, 210, 210, 190), sub.c_str());
  dl->PopClipRect();

  if (focused) dl->AddRect(p0, p1, IM_COL32(255, 214, 102, 255), 8.0f, 0, 3.0f);
  if (dim) dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, dim), 8.0f);

  ImGui::EndGroup();
  ImGui::PopID();
  return activated;
}

// ---- small pieces -----------------------------------------------------------

void colored_text(const ImVec4& colour, const std::string& s) {
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextWrapped("%s", s.c_str());
  ImGui::PopStyleColor();
}

void warn_text(const std::string& s) { colored_text(kWarn, s); }

void good_text(const std::string& s) { colored_text(kGood, s); }

bool input_string(const char* label, std::string& s, size_t cap, ImGuiInputTextFlags flags) {
  std::vector<char> buf(cap, '\0');
  std::snprintf(buf.data(), cap, "%s", s.c_str());
  if (!ImGui::InputText(label, buf.data(), cap, flags)) return false;
  s = buf.data();
  return true;
}

bool input_string_multiline(const char* label, std::string& s, size_t cap, ImVec2 size) {
  std::vector<char> buf(cap, '\0');
  std::snprintf(buf.data(), cap, "%s", s.c_str());
  if (!ImGui::InputTextMultiline(label, buf.data(), cap, size)) return false;
  s = buf.data();
  return true;
}

fs::path home_dir() {
  if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
  return fs::path("/");
}

}  // namespace kg::gui
