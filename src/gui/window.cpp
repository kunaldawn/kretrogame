#include "window.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

namespace kg::gui {
namespace fs = std::filesystem;

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
  // SDL spells "centred" as an unsigned mask that fits an int, which is what
  // the position is; said once, so each side of the choice is an int.
  const int centred = static_cast<int>(SDL_WINDOWPOS_CENTERED);
  w.win = SDL_CreateWindow(spec.title, spec.x >= 0 ? spec.x : centred, spec.y >= 0 ? spec.y : centred,
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
