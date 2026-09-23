#include "widgets.h"

#include <cstdlib>

#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

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

// ---- pictures ---------------------------------------------------------------

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

// ---- the window -------------------------------------------------------------

bool have_display() { return std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY"); }

std::optional<Window> open_window(const rt::Env& e, const WindowSpec& spec, std::string* why) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
    if (why) *why = std::string("cannot open a window: ") + SDL_GetError();
    return std::nullopt;
  }
  Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
  if (spec.borderless) flags |= SDL_WINDOW_BORDERLESS;
  if (spec.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  Window w;
  w.win = SDL_CreateWindow(spec.title, spec.x >= 0 ? spec.x : SDL_WINDOWPOS_CENTERED,
                           spec.y >= 0 ? spec.y : SDL_WINDOWPOS_CENTERED,
                           static_cast<int>(spec.width), static_cast<int>(spec.height), flags);
  if (!w.win) {
    if (why) *why = std::string("cannot open a window: ") + SDL_GetError();
    SDL_Quit();
    return std::nullopt;
  }
  w.ren = SDL_CreateRenderer(w.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!w.ren) w.ren = SDL_CreateRenderer(w.win, -1, SDL_RENDERER_SOFTWARE);
  if (!w.ren) {
    if (why) *why = std::string("cannot draw: ") + SDL_GetError();
    SDL_DestroyWindow(w.win);
    SDL_Quit();
    return std::nullopt;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.IniFilename = nullptr;  // no state file; what is remembered is remembered on purpose
  // A page that throws is caught by its caller, but it throws with its window
  // still open. ImGui has recovery for exactly that and ends the window at
  // EndFrame - after an assert, which in this build (nothing defines NDEBUG)
  // aborts, which is the death the catch exists to prevent. The debug log and
  // the red tooltip stay on, so a recovered frame is still loud.
  io.ConfigErrorRecoveryEnableAssert = false;

  for (const char* rel : {"usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                          "usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
    fs::path f = e.root / rel;
    std::error_code ec;
    if (e.root.empty() || !fs::exists(f, ec)) continue;
    w.body = io.Fonts->AddFontFromFileTTF(f.c_str(), 20.0f);
    w.big = io.Fonts->AddFontFromFileTTF(f.c_str(), 34.0f);
    break;
  }
  if (!w.body) {
    w.body = io.Fonts->AddFontDefault();
    w.big = w.body;
  }

  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 0;
  st.FrameRounding = 6;
  st.WindowPadding = ImVec2(28, 22);
  st.ItemSpacing = ImVec2(12, 10);
  st.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 1.0f);
  st.Colors[ImGuiCol_NavHighlight] = ImVec4(1.0f, 0.84f, 0.40f, 1.0f);

  ImGui_ImplSDL2_InitForSDLRenderer(w.win, w.ren);
  ImGui_ImplSDLRenderer2_Init(w.ren);

  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
  }
  return w;
}

void begin_frame(const Window& w) {
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  ImGui::PushFont(w.body);
}

void end_frame(const Window& w) {
  ImGui::PopFont();
  ImGui::Render();
  SDL_SetRenderDrawColor(w.ren, 18, 18, 20, 255);
  SDL_RenderClear(w.ren);
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), w.ren);
  SDL_RenderPresent(w.ren);
}

void close_window(Window& w) {
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  if (w.ren) SDL_DestroyRenderer(w.ren);
  if (w.win) SDL_DestroyWindow(w.win);
  SDL_Quit();
  w = Window{};
}

}  // namespace kg::gui
