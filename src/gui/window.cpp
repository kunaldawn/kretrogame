#include "window.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <system_error>

#include "imgui_impl_sdl2.h"
#include "imgui_internal.h"
#include "imgui_impl_sdlrenderer2.h"
#include "palette.h"
#include "widgets.h"

namespace kg::gui {
namespace fs = std::filesystem;

namespace {

// What the window rebuilds its fonts and style from whenever the scale
// changes. One window per process, so one of these.
struct Look {
  // The runtime's monospace faces, and a proportional one whose symbols fill
  // in the arrows, boxes and blocks the monospace faces lack. Empty when the
  // runtime has none, which leaves ImGui's own font.
  std::string regular, bold, symbols;
  // The style at scale 1, which each rebuild scales afresh: scaling a style
  // that was already scaled would compound.
  ImGuiStyle base;
  // What the fonts and style were last built for; 0 before the first build.
  float scale = 0, fb = 0;
};
Look g_look;

// Latin-1, and the symbols a terminal draws its chrome with: dashes, bullets
// and the ellipsis, arrows, a few operators, the keyboard's own symbols (the
// return key), box drawing, blocks and geometric shapes. The ranges must
// outlive the atlas, so they are static. Pairs, one to a line.
// clang-format off
const ImWchar kTextRanges[] = {
    0x0020, 0x00FF,  // Basic Latin and Latin-1
    0x2010, 0x2027,  // dashes, quotes, bullets, the ellipsis
    0x2030, 0x203A,  // per mille, primes, angle quotes
    0x2190, 0x21FF,  // arrows
    0x2212, 0x221E,  // minus, bullet operator, square root, infinity
    0x2248, 0x2248,  // almost equal
    0x2260, 0x2265,  // not equal, less and greater or equal
    0x2300, 0x23FF,  // miscellaneous technical: the return key, media keys
    0x2500, 0x25FF,  // box drawing, block elements, geometric shapes
    0,
};
// Titles and tile names are only ever words, so their much larger glyphs
// cover less: text, punctuation, and the blocks and triangles a title might
// be dressed with.
const ImWchar kTitleRanges[] = {
    0x0020, 0x00FF,
    0x2010, 0x2027,
    0x2030, 0x203A,
    0x2190, 0x2193,
    0x2580, 0x259F,
    0x25A0, 0x25CF,
    0,
};
// clang-format on

std::string first_font(const rt::Env& e, std::initializer_list<std::string> rels) {
  if (e.root.empty()) return {};
  for (const std::string& rel : rels) {
    fs::path f = e.root / rel;
    std::error_code ec;
    if (fs::exists(f, ec)) return f.string();
  }
  return {};
}

// The smallest the small and body type may be laid out at, in window
// pixels, however far the scale goes down: below these a monospace face at
// the window's usual density stops being readable.
constexpr float kSmallFloorPx = 12.0f, kBodyFloorPx = 14.0f;

// The largest factor the glyphs are rasterised at. Four fonts over the text
// ranges at a scale of 4 already make an atlas of several thousand pixels a
// side; a zoomed window on a doubled framebuffer would ask for 8 or more,
// past the largest texture many GPUs take, and then no text draws at all.
// Beyond this the glyphs are drawn larger than they were rasterised, which
// at that size is hard to see.
constexpr float kMaxRaster = 4.0f;

// The fonts for every role at scale `s` on a framebuffer of scale `fb`: laid
// out at the scale (or the floor), and rasterised at that times the
// framebuffer's own scale, so text is drawn at the size the screen will show
// it and not stretched up from a smaller one - as far as kMaxRaster, or
// `limit` when a texture that size was refused. Returns the factor the
// rasterised sizes were multiplied by to stay within it (1 when they were
// not), or 0 when the renderer would not take the atlas.
float build_fonts(float s, float fb, float limit) {
  const float shrink = std::min(1.0f, limit / (s * fb));
  const float raster = s * fb * shrink;
  ImGuiIO& io = ImGui::GetIO();
  for (int r = 0; r < static_cast<int>(FontRole::Count); ++r) set_font(static_cast<FontRole>(r), nullptr);
  io.FontDefault = nullptr;
  ImGui_ImplSDLRenderer2_DestroyFontsTexture();
  io.Fonts->Clear();

  auto add = [&](FontRole role, bool bold, const ImWchar* ranges) {
    float laid = kFontDesignPx[static_cast<int>(role)] * s;
    if (role == FontRole::Small) laid = std::max(laid, kSmallFloorPx);
    if (role == FontRole::Body) laid = std::max(laid, kBodyFloorPx);
    const float size = std::round(laid * fb * shrink);
    ImFontConfig cfg;
    // Oversampling sharpens small text; large text gains nothing from it but
    // a bigger atlas.
    cfg.OversampleH = raster >= 1.5f ? 1 : 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    const std::string& file = bold && !g_look.bold.empty() ? g_look.bold : g_look.regular;
    ImFont* f = nullptr;
    if (!file.empty()) f = io.Fonts->AddFontFromFileTTF(file.c_str(), size, &cfg, ranges);
    if (f && !g_look.symbols.empty()) {
      ImFontConfig merge = cfg;
      merge.MergeMode = true;
      io.Fonts->AddFontFromFileTTF(g_look.symbols.c_str(), size, &merge, ranges);
    }
    if (!f) {
      cfg.SizePixels = size;
      f = io.Fonts->AddFontDefault(&cfg);
    }
    set_font(role, f);
  };
  add(FontRole::Body, false, kTextRanges);
  add(FontRole::Small, false, kTextRanges);
  add(FontRole::Big, true, kTitleRanges);
  add(FontRole::Display, true, kTitleRanges);
  add(FontRole::Hero, true, kTitleRanges);
  io.FontDefault = font(FontRole::Body);
  return ImGui_ImplSDLRenderer2_CreateFontsTexture() ? shrink : 0.0f;
}

// kretro's look at scale 1: the terminal palette over ImGui's dark style,
// square corners and hairline borders.
ImGuiStyle base_style() {
  ImGuiStyle st;
  ImGui::StyleColorsDark(&st);
  st.WindowPadding = ImVec2(24, 20);
  st.FramePadding = ImVec2(10, 6);
  st.CellPadding = ImVec2(8, 4);
  st.ItemSpacing = ImVec2(10, 8);
  st.ItemInnerSpacing = ImVec2(8, 6);
  st.IndentSpacing = 20;
  st.ScrollbarSize = 10;
  st.GrabMinSize = 10;
  st.WindowRounding = 0;
  st.ChildRounding = 0;
  st.FrameRounding = 2;
  st.PopupRounding = 2;
  st.ScrollbarRounding = 0;
  st.GrabRounding = 1;
  st.TabRounding = 2;
  st.WindowBorderSize = 1;
  st.ChildBorderSize = 1;
  st.PopupBorderSize = 1;
  st.FrameBorderSize = 1;
  st.TabBorderSize = 0;
  st.SeparatorTextBorderSize = 1;
  st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  // ImGui draws a thin line by sampling a strip of the font atlas, which the
  // SDL software renderer samples badly: a hairline comes out stepped and
  // half there. Geometry is exact on every renderer.
  st.AntiAliasedLinesUseTex = false;

  ImVec4* c = st.Colors;
  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = kDim;
  c[ImGuiCol_WindowBg] = kBg0;
  // A child is as often layout - a grid, a scrolling list - as it is a
  // panel, and a panel colour over the rest of a page reads as a box nobody
  // drew. A bordered child still shows its border; a page that wants a
  // panel pushes kBg1.
  c[ImGuiCol_ChildBg] = alpha(kBg1, 0.0f);
  // Opaque: a modal is a pane of its own, and a page showing through it
  // reads as text written over text.
  c[ImGuiCol_PopupBg] = kBg1;
  c[ImGuiCol_Border] = kFrameLine;
  c[ImGuiCol_BorderShadow] = alpha(kBg0, 0.0f);
  c[ImGuiCol_FrameBg] = kBg2;
  c[ImGuiCol_FrameBgHovered] = kBg3;
  c[ImGuiCol_FrameBgActive] = kBgActive;
  c[ImGuiCol_TitleBg] = kBg1;
  c[ImGuiCol_TitleBgActive] = kBg2;
  c[ImGuiCol_TitleBgCollapsed] = kBg1;
  c[ImGuiCol_MenuBarBg] = kBg1;
  c[ImGuiCol_ScrollbarBg] = alpha(kBg0, 0.0f);
  c[ImGuiCol_ScrollbarGrab] = kLine;
  c[ImGuiCol_ScrollbarGrabHovered] = kAccentDim;
  c[ImGuiCol_ScrollbarGrabActive] = kAccent;
  c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_SliderGrab] = kAccentDim;
  c[ImGuiCol_SliderGrabActive] = kAccent;
  c[ImGuiCol_Button] = kBg2;
  c[ImGuiCol_ButtonHovered] = kButtonHover;
  c[ImGuiCol_ButtonActive] = kAccentDim;
  c[ImGuiCol_Header] = alpha(kAccentDim, 0.35f);
  c[ImGuiCol_HeaderHovered] = alpha(kAccentDim, 0.55f);
  c[ImGuiCol_HeaderActive] = alpha(kAccentDim, 0.85f);
  c[ImGuiCol_Separator] = kLine;
  c[ImGuiCol_SeparatorHovered] = kAccentDim;
  c[ImGuiCol_SeparatorActive] = kAccent;
  c[ImGuiCol_ResizeGrip] = alpha(kAccentDim, 0.25f);
  c[ImGuiCol_ResizeGripHovered] = alpha(kAccentDim, 0.6f);
  c[ImGuiCol_ResizeGripActive] = kAccent;
  c[ImGuiCol_Tab] = kBg1;
  c[ImGuiCol_TabHovered] = alpha(kAccentDim, 0.6f);
  c[ImGuiCol_TabSelected] = kBg3;
  c[ImGuiCol_TabSelectedOverline] = kAccent;
  c[ImGuiCol_TabDimmed] = kBg1;
  c[ImGuiCol_TabDimmedSelected] = kBg2;
  c[ImGuiCol_TabDimmedSelectedOverline] = kAccentDim;
  c[ImGuiCol_PlotLines] = kAccent;
  c[ImGuiCol_PlotLinesHovered] = kAmber;
  c[ImGuiCol_PlotHistogram] = kAccentDim;
  c[ImGuiCol_PlotHistogramHovered] = kAmber;
  c[ImGuiCol_TableHeaderBg] = kBg2;
  c[ImGuiCol_TableBorderStrong] = kLine;
  c[ImGuiCol_TableBorderLight] = alpha(kLine, 0.7f);
  c[ImGuiCol_TableRowBg] = alpha(kBg0, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = alpha(kText, 0.025f);
  c[ImGuiCol_TextLink] = kCyan;
  c[ImGuiCol_TextSelectedBg] = alpha(kAccentDim, 0.55f);
  c[ImGuiCol_DragDropTarget] = kAmber;
  // ImGui's own focus rectangle is not drawn: the focus glow (focus.h) is
  // drawn over every item alike once a frame, and would double it.
  c[ImGuiCol_NavCursor] = alpha(kAmber, 0.0f);
  c[ImGuiCol_NavWindowingHighlight] = kAmber;
  c[ImGuiCol_NavWindowingDimBg] = alpha(kBg0, 0.6f);
  c[ImGuiCol_ModalWindowDimBg] = alpha(kBg0, 0.85f);
  return st;
}

// The framebuffer's scale, to the nearest quarter: 1 on most X11 desktops,
// 2 on a doubled Wayland output, 1.25 or 1.5 on a fractional one.
float framebuffer_scale(const Window& w, int lw) {
  int dw = 0, dh = 0;
  if (lw <= 0 || SDL_GetRendererOutputSize(w.ren, &dw, &dh) != 0 || dw <= 0) return 1.0f;
  return std::clamp(std::round(static_cast<float>(dw) / static_cast<float>(lw) * 4.0f) / 4.0f, 1.0f, 4.0f);
}

// The scale the window wants now. A minimized window has no size to fit, so
// it keeps what it had.
void apply_scale(const Window& w) {
  int lw = 0, lh = 0;
  SDL_GetWindowSize(w.win, &lw, &lh);
  if ((SDL_GetWindowFlags(w.win) & SDL_WINDOW_MINIMIZED) || lw <= 0 || lh <= 0) {
    if (g_look.scale > 0) return;
    lw = 1280;
    lh = 800;
  }
  const float fb = framebuffer_scale(w, lw);
  float ddpi = 0;
  const int display = SDL_GetWindowDisplayIndex(w.win);
  if (display < 0 || SDL_GetDisplayDPI(display, &ddpi, nullptr, nullptr) != 0) ddpi = 0;
  const std::optional<float> forced = forced_ui_scale();
  const float base = forced ? *forced : auto_ui_scale(static_cast<float>(lw), static_cast<float>(lh), fb, ddpi);
  set_zoom_base(base);
  const float s = quantize_ui_scale(base * ui_zoom());
  if (s == g_look.scale && fb == g_look.fb) return;

  // A renderer that refuses the atlas gets one rasterised smaller, down to a
  // quarter of the limit, rather than a window with no text in it.
  float shrink = 0;
  for (float limit = kMaxRaster; shrink == 0 && limit >= kMaxRaster / 4; limit *= 0.5f) {
    shrink = build_fonts(s, fb, limit);
  }
  if (shrink == 0) shrink = 1;
  ImGuiIO& io = ImGui::GetIO();
  // The fonts are rasterised for the framebuffer (or smaller, past the
  // limit) and laid out in window coordinates; the backend scales the
  // geometry up by the framebuffer's factor.
  io.FontGlobalScale = 1.0f / (fb * shrink);
  ImGuiStyle& st = ImGui::GetStyle();
  st = g_look.base;
  st.ScaleAllSizes(s);
  // ScaleAllSizes leaves the borders at a pixel. The widgets' own rules and
  // frames are hairline() thick, a whole number of pixels that grows with
  // the scale, so ImGui's borders are made the same weight to match them.
  const float border = std::max(1.0f, std::round(s));
  st.WindowBorderSize = st.ChildBorderSize = st.PopupBorderSize = st.FrameBorderSize = border;
  st.SeparatorTextBorderSize = border;
  set_ui_scale(s);
  g_look.scale = s;
  g_look.fb = fb;
}

}  // namespace

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
  // Every event a frame, not one of each kind: trickled, a mouse move and a
  // wheel notch in one poll each end ImGui's input for the frame, so a wheel
  // turned while the mouse moves falls a frame behind per notch, and a key
  // pressed behind a burst of them waits its turn.
  io.ConfigInputTrickleEventQueue = false;
  // Escape and the pad's B mean back, which the host acts on; they do not
  // also drop the focus, so a page that stays up keeps its place.
  io.ConfigNavEscapeClearFocusItem = false;
  io.ConfigNavCursorVisibleAlways = input_mode() != InputMode::Mouse;
  // No Ctrl+Tab window switcher: its list would name every page's window.
  ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;
  ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;
  io.IniFilename = nullptr;  // no state file; what is remembered is remembered on purpose
  // A page that throws is caught by its caller, but it throws with its window
  // still open. ImGui has recovery for exactly that and ends the window at
  // EndFrame - after an assert, which in this build (nothing defines NDEBUG)
  // aborts, which is the death the catch exists to prevent. The debug log and
  // the red tooltip stay on, so a recovered frame is still loud.
  io.ConfigErrorRecoveryEnableAssert = false;

  // Monospace throughout: DejaVu Sans Mono, or Liberation Mono in a runtime
  // without it, or ImGui's own font in no runtime at all.
  const std::string dejavu = "usr/share/fonts/truetype/dejavu/";
  const std::string liberation = "usr/share/fonts/truetype/liberation/";
  g_look = Look{};
  g_look.regular = first_font(e, {dejavu + "DejaVuSansMono.ttf", liberation + "LiberationMono-Regular.ttf"});
  g_look.bold = first_font(e, {dejavu + "DejaVuSansMono-Bold.ttf", liberation + "LiberationMono-Bold.ttf"});
  if (!g_look.regular.empty()) g_look.symbols = first_font(e, {dejavu + "DejaVuSans.ttf"});
  g_look.base = base_style();
  set_chrome_tag(spec.title ? spec.title : "");

  ImGui_ImplSDL2_InitForSDLRenderer(w.win, w.ren);
  ImGui_ImplSDLRenderer2_Init(w.ren);
  // The first fonts and style, before the first frame, so a window never
  // shows a frame in ImGui's defaults.
  apply_scale(w);

  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
  }
  return w;
}

void begin_frame(const Window& w) {
  // Between frames, where nothing holds a font: the atlas may be rebuilt here.
  apply_scale(w);
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  ImGui::PushFont(font(FontRole::Body));
}

void end_frame(const Window& w) {
  ImGui::PopFont();
  // ImGui fades a modal's dimming in over a few frames, so the first frames
  // of every modal show the page at full brightness behind it, competing with
  // it. Dimmed at once, a modal always looks the same.
  if (ImGui::GetTopMostPopupModal()) ImGui::GetCurrentContext()->DimBgRatio = 1.0f;
  // Last, over every window's items, so it is drawn once and on top.
  draw_focus_glow();
  ImGui::Render();
  // The window's own background, so the edge of a resize never flashes a
  // different colour.
  SDL_SetRenderDrawColor(w.ren, static_cast<Uint8>(std::lround(kBg0.x * 255.0f)),
                         static_cast<Uint8>(std::lround(kBg0.y * 255.0f)), static_cast<Uint8>(std::lround(kBg0.z * 255.0f)),
                         255);
  SDL_RenderClear(w.ren);
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), w.ren);
  SDL_RenderPresent(w.ren);
}

void close_window(Window& w) {
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  for (int r = 0; r < static_cast<int>(FontRole::Count); ++r) set_font(static_cast<FontRole>(r), nullptr);
  g_look = Look{};
  set_ui_scale(1.0f);
  if (w.ren) SDL_DestroyRenderer(w.ren);
  if (w.win) SDL_DestroyWindow(w.win);
  SDL_Quit();
  w = Window{};
}

}  // namespace kg::gui
