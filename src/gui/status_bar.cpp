// The bottom line of the window, in the manner of tmux or vim and of a
// console's button-hint bar: a back arrow, the program's tag and the page's
// path at the left, the keys the page answers to at the right, drawn as the
// keyboard's keycaps or the pad's buttons depending on which was used last,
// and each hint a button that presses its key.
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "anim.h"
#include "draw.h"
#include "focus.h"
#include "imgui_internal.h"
#include "palette.h"
#include "scale.h"
#include "widgets.h"

namespace kg::gui {

namespace {

std::string g_tag;
// The path a page asked the status bar to show, for this frame only.
std::string g_path;

// One key and what it does, as a hint writes them.
struct Hint {
  std::string key, action;
};

// "arrows move   Enter play   Esc quit": groups three or more spaces apart,
// each a key and then its action.
std::vector<Hint> parse_hint(const char* hint) {
  std::vector<Hint> out;
  std::string h = hint ? hint : "";
  size_t i = 0;
  while (i < h.size()) {
    while (i < h.size() && h[i] == ' ') ++i;
    size_t end = h.find("   ", i);
    if (end == std::string::npos) end = h.size();
    std::string group = h.substr(i, end - i);
    while (!group.empty() && group.back() == ' ') group.pop_back();
    if (!group.empty()) {
      size_t sp = group.find(' ');
      Hint g;
      g.key = group.substr(0, sp);
      if (sp != std::string::npos) g.action = group.substr(sp + 1);
      out.push_back(g);
    }
    i = end;
  }
  return out;
}

// The pad's button for a key, or "" when the pad has none: letters are
// typed, and a pad cannot type.
std::string pad_button(const std::string& key) {
  if (key == "Enter") return "A";
  if (key == "Esc") return "B";
  if (key == "Space") return "X";
  if (key == "Tab") return "LB RB";
  if (key == "arrows") return "dpad";
  return "";
}

// Which hints give way first when the bar is short of room: the way out and
// the way in stay longest, moving next, and the letters first.
int priority(const std::string& key) {
  if (key == "Esc" || key == "Enter") return 0;
  if (key == "arrows" || key == "Space" || key == "Tab") return 1;
  return 2;
}

// What pressing a hint's key sends: a key, or for a letter the text typing it
// sends, which is all the hosts read a letter from.
struct Press {
  SDL_Keycode key = SDLK_UNKNOWN;
  SDL_Scancode scan = SDL_SCANCODE_UNKNOWN;
  char text = 0;
  bool ok() const { return key != SDLK_UNKNOWN || text != 0; }
};

Press press_for(const std::string& key) {
  Press p;
  if (key == "Esc") p = {SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 0};
  else if (key == "Enter") p = {SDLK_RETURN, SDL_SCANCODE_RETURN, 0};
  else if (key == "Space") p = {SDLK_SPACE, SDL_SCANCODE_SPACE, 0};
  else if (key == "Tab") p = {SDLK_TAB, SDL_SCANCODE_TAB, 0};
  else if (key == "Backspace") p = {SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, 0};
  else if (key.size() >= 2 && key.size() <= 3 && key[0] == 'F' && std::isdigit(static_cast<unsigned char>(key[1]))) {
    const int n = std::atoi(key.c_str() + 1);
    if (n >= 1 && n <= 12) {
      p.key = SDLK_F1 + (n - 1);
      p.scan = static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (n - 1));
    }
  } else if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0]))) {
    p.text = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
  }
  return p;
}

// A key pressed from the bar is let go of a frame later: pressed and let go
// in one frame, ImGui would never see it down.
struct Held {
  SDL_Keycode key = SDLK_UNKNOWN;
  SDL_Scancode scan = SDL_SCANCODE_UNKNOWN;
  int frame = -1;
};
Held g_held;

Uint32 window_id() {
  return static_cast<Uint32>(reinterpret_cast<intptr_t>(ImGui::GetMainViewport()->PlatformHandle));
}

void push_key(Uint32 type, SDL_Keycode key, SDL_Scancode scan) {
  SDL_Event ev{};
  ev.type = type;
  ev.key.windowID = window_id();
  ev.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
  ev.key.keysym.sym = key;
  ev.key.keysym.scancode = scan;
  ev.key.keysym.mod = KMOD_NONE;
  SDL_PushEvent(&ev);
}

void release_held() {
  if (g_held.key == SDLK_UNKNOWN || g_held.frame >= ImGui::GetFrameCount()) return;
  push_key(SDL_KEYUP, g_held.key, g_held.scan);
  g_held = Held{};
}

// Sends what pressing the key would: the program cannot tell the two apart,
// so a hint does exactly what the key does, and nothing a key would not.
void send(const Press& p) {
  if (p.text) {
    SDL_Event ev{};
    ev.type = SDL_TEXTINPUT;
    ev.text.windowID = window_id();
    ev.text.text[0] = p.text;
    SDL_PushEvent(&ev);
    return;
  }
  if (g_held.key != SDLK_UNKNOWN) {
    push_key(SDL_KEYUP, g_held.key, g_held.scan);
    g_held = Held{};
  }
  // The keys that act on the focused item act on it even when the mouse had
  // hidden the focus: the click was aimed at them.
  if (p.key == SDLK_RETURN || p.key == SDLK_SPACE) ImGui::SetNavCursorVisible(true);
  push_key(SDL_KEYDOWN, p.key, p.scan);
  g_held = {p.key, p.scan, ImGui::GetFrameCount()};
}

// Whether the bar may be clicked: not under a modal or any popup, which has
// the screen, and only with the pointer over the bar itself.
bool clickable() {
  if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return false;
  const ImGuiContext& g = *GImGui;
  return g.HoveredWindow == nullptr && ImGui::IsMousePosValid();
}

// A region of the bar the pointer is over and may press: marked as hovered
// for ImGui, so a click there is not a click on nothing (which would take the
// focus away from the page), and shown with the hand.
bool hot(ImVec2 a, ImVec2 b, ImGuiID id, bool can_click) {
  if (!can_click || !ImGui::IsMouseHoveringRect(a, b, false)) return false;
  ImGui::SetHoveredID(id);
  ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  return true;
}

// The zoom as it was last seen, and when it last changed, so the bar can say
// the new one for a moment and then give its room back to the hints.
float g_seen_scale = 0;
double g_scale_changed = -100.0;

// A path cut short from the left, so the end of it - the page one is on -
// survives: "~/…/example-collection/identity", then "~/…/identity", then the
// last part itself cut from the left, "…entity".
std::string elide_path(ImFont* f, const std::string& path, float max_w) {
  if (text_w(f, path.c_str()) <= max_w) return path;
  const std::string ell = ellipsis(f);
  const std::string home = "~/";
  if (path.rfind(home, 0) == 0) {
    for (size_t cut = path.find('/', home.size()); cut != std::string::npos; cut = path.find('/', cut + 1)) {
      const std::string s = home + ell + path.substr(cut);
      if (text_w(f, s.c_str()) <= max_w) return s;
    }
  }
  // The longest tail that fits after an ellipsis, on a character boundary.
  const float ell_w = text_w(f, ell.c_str());
  if (ell_w > max_w) return std::string();
  size_t from = path.size();
  while (from > 0) {
    size_t prev = from - 1;
    while (prev > 0 && (static_cast<unsigned char>(path[prev]) & 0xC0) == 0x80) --prev;
    if (text_w(f, path.c_str() + prev) + ell_w > max_w) break;
    from = prev;
  }
  return ell + path.substr(from);
}

}  // namespace

void set_chrome_tag(const std::string& tag) { g_tag = tag; }

void set_chrome_path(const std::string& path) { g_path = path; }

void clear_chrome_path() { g_path.clear(); }

void press_escape() {
  if (ImGui::GetCurrentContext()) send(press_for("Esc"));
}

float status_bar_height() { return std::round(font_px(font(FontRole::Small)) + px(14)); }

std::string chrome_path(const char* title) {
  if (!g_path.empty()) return g_path;
  const std::string t = title ? title : "";
  if (t.empty() || t == g_tag) return "~/";
  std::string out = "~/";
  bool gap = false;
  for (char ch : t) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || c >= 0x80) {
      if (gap && out.size() > 2) out += '-';
      gap = false;
      out += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
    } else {
      gap = true;
    }
  }
  return out;
}

void draw_status_bar(const char* title, const char* hint) {
  release_held();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImDrawList* dl = ImGui::GetBackgroundDrawList(vp);
  ImFont* f = font(FontRole::Small);
  const float bar_h = status_bar_height();
  const ImVec2 p0(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - bar_h);
  const ImVec2 p1(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
  dl->AddRectFilled(p0, p1, u32(kBg1));
  dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + hairline()), u32(kLine));

  const float margin = std::round(px(12));
  const float gap = std::round(px(18));
  const float cap_h = std::round(font_px(f) + px(5));
  const float cap_y = std::round(p0.y + (bar_h - cap_h) * 0.5f);
  // Every word on the bar shares the keycaps' line, so a key and what it
  // does read as one.
  const float text_y = ink_centred_y(f, cap_y, cap_h);
  const float chip_top = p0.y + hairline();
  const bool can_click = clickable();
  const ImGuiID bar_id = ImHashStr("##status-bar");
  const bool pad = input_mode() == InputMode::Pad;
  const double now = ImGui::GetTime();

  // The zoom, at the right, for two seconds after it changes and while the
  // pointer is over that end of the bar; the rest of the time its room goes
  // to the hints.
  char zoom[32];
  std::snprintf(zoom, sizeof zoom, "%d%%", static_cast<int>(std::lround(ui_scale() * 100.0f)));
  const char* zoom_label = "ui ";
  const float zoom_w = text_w(f, zoom_label) + text_w(f, zoom);
  const float zoom_x = p1.x - margin - zoom_w;
  if (g_seen_scale != ui_scale()) {
    if (g_seen_scale != 0) g_scale_changed = now;
    g_seen_scale = ui_scale();
  }
  const bool zoom_hovered = ImGui::IsMouseHoveringRect(ImVec2(zoom_x - margin, p0.y), p1, false);
  float zoom_a = zoom_hovered ? 1.0f : 0.0f;
  const double since = now - g_scale_changed;
  if (since < 2.0) zoom_a = 1.0f;
  else if (since < 2.15 && !reduce_motion()) zoom_a = std::max(zoom_a, static_cast<float>(1.0 - (since - 2.0) / 0.15));
  float limit = p1.x - margin;
  if (zoom_a > 0) {
    dl->AddText(f, font_px(f), ImVec2(zoom_x, text_y), u32(kDim, zoom_a), zoom_label);
    dl->AddText(f, font_px(f), ImVec2(zoom_x + text_w(f, zoom_label), text_y), u32(kText, zoom_a), zoom);
    limit = zoom_x - gap;
  }
  if (zoom_hovered && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
    ImGui::SetTooltip("Ctrl + and Ctrl - zoom, Ctrl 0 resets");
  }

  // The hints the device in hand has keys for, measured before anything is
  // drawn, so the path can have whatever room they leave.
  struct Shown {
    Hint h;
    std::string glyph;  // the pad's button, in pad mode
    float w = 0;
    bool keep = true;
  };
  std::vector<Shown> hints;
  const float inner = std::round(px(6));
  bool back = false;
  for (const Hint& h : parse_hint(hint)) {
    if (h.key == "Esc" && h.action.rfind("back", 0) == 0) back = true;
    Shown s;
    s.h = h;
    if (pad) {
      s.glyph = pad_button(h.key);
      if (s.glyph.empty()) continue;
    }
    const float kw = pad ? pad_glyph_w(f, s.glyph, cap_h) : keycap_w(f, h.key);
    s.w = kw + (h.action.empty() ? 0.0f : inner + text_w(f, h.action.c_str()));
    hints.push_back(s);
  }
  auto hints_w = [&] {
    float w = 0;
    for (const Shown& s : hints) {
      if (s.keep) w += (w > 0 ? gap : 0.0f) + s.w;
    }
    return w;
  };

  float x = p0.x;
  // Back, as a console's bar and a browser both have it: the arrow that does
  // what Escape does on a page where Escape goes back.
  if (back) {
    const float bw = std::round(bar_h * 1.1f);
    const ImVec2 a(x, chip_top), b(x + bw, p1.y);
    const bool over = hot(a, b, ImHashStr("##status-back", 0, bar_id), can_click);
    dl->AddRectFilled(a, b, u32(over ? kButtonHover : kBg2));
    const float gs = std::round(cap_h * 0.55f);
    const ImVec2 g0(std::round(x + (bw - gs * 0.9f) * 0.5f), std::round(chip_top + (p1.y - chip_top - gs) * 0.5f));
    draw_back_glyph(dl, g0, ImVec2(g0.x + gs, g0.y + gs), u32(over ? kAccent : kText));
    if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) send(press_for("Esc"));
    x += bw;
  }

  // The tag and the page's path, powerline fashion: a solid accent block the
  // height of the bar, and a darker one after it with the path in green, so
  // the program's home screen reads "kretro ~" rather than its name twice.
  const float avail = std::max(0.0f, limit - x);
  if (!g_tag.empty()) {
    // The tag is the same on every page, so it gets no more than a quarter.
    const std::string tag = elide(f, g_tag, std::max(px(60), avail * 0.25f) - margin * 2);
    const float tw = std::round(text_w(f, tag.c_str()) + margin * 2);
    dl->AddRectFilled(ImVec2(x, chip_top), ImVec2(x + tw, p1.y), u32(kAccentDim));
    dl->AddText(f, font_px(f), ImVec2(x + margin, text_y), u32(kBg0), tag.c_str());
    x += tw;
  }
  {
    // The path takes what the hints leave, and never less than enough to
    // say which page this is; past that, hints give way to it.
    const std::string full = chrome_path(title);
    const float least = std::min(text_w(f, full.c_str()), std::round(px(150)));
    const float room = std::max(least, limit - x - margin * 3 - hints_w());
    const std::string path = elide_path(f, full, std::min(room, limit - x - margin * 2));
    const float pw = std::round(text_w(f, path.c_str()) + margin * 2);
    dl->AddRectFilled(ImVec2(x, chip_top), ImVec2(x + pw, p1.y), u32(kBg3));
    dl->AddText(f, font_px(f), ImVec2(x + margin, text_y), u32(kAccent), path.c_str());
    x += pw;
  }
  x += margin;

  // The hints against the right edge, in the order the page wrote them, so
  // the way out (written last) sits in the corner a console puts it. When
  // they do not all fit, the letters give way first, then moving; an
  // ellipsis at the left says some are missing.
  const char* ell = ellipsis(f);
  const float ell_w = text_w(f, ell);
  const float room = limit - x;
  bool cut = false;
  for (int p = 2; p >= 0 && hints_w() > room; --p) {
    for (Shown& s : hints) {
      if (s.keep && priority(s.h.key) == p && hints_w() + gap + ell_w > room) {
        s.keep = false;
        cut = true;
      }
    }
  }
  if (cut) {
    while (hints_w() + gap + ell_w > room) {
      // Still too wide: drop from the left, whatever they are.
      auto it = std::find_if(hints.begin(), hints.end(), [](const Shown& s) { return s.keep; });
      if (it == hints.end()) break;
      it->keep = false;
    }
  }
  float hx = limit - hints_w();
  if (cut && hints_w() > 0 && hx - gap - ell_w >= x) {
    dl->AddText(f, font_px(f), ImVec2(hx - gap - ell_w, text_y), u32(kDim), ell);
  }
  for (const Shown& s : hints) {
    if (!s.keep) continue;
    const Press p = press_for(s.h.key);
    const float pad_x = std::round(px(4));
    const ImVec2 a(hx - pad_x, chip_top), b(hx + s.w + pad_x, p1.y);
    const bool over = p.ok() && hot(a, b, ImHashStr(s.h.key.c_str(), 0, bar_id), can_click);
    // Lifted under the pointer, as a button is, so it reads as one.
    if (over) dl->AddRectFilled(a, b, u32(kBg3));
    float kw;
    if (pad) {
      kw = draw_pad_glyph(dl, f, ImVec2(hx, cap_y), cap_h, s.glyph);
    } else {
      // Escape is the way out of every page, so it is the one key in amber.
      const ImVec4& bg = s.h.key == "Esc" ? kAmber : kCyan;
      kw = draw_keycap(dl, f, ImVec2(hx, cap_y), cap_h, s.h.key, u32(bg));
    }
    if (!s.h.action.empty()) {
      dl->AddText(f, font_px(f), ImVec2(hx + kw + inner, text_y), u32(over ? kText : kDim), s.h.action.c_str());
    }
    if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) send(p);
    hx += s.w + gap;
  }
}

}  // namespace kg::gui
