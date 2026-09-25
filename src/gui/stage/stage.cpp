#include "stage.h"

#include "x_errors.h"

// Before Xlib, whose macros (None, Status, Bool) would otherwise land in the
// ImGui and widget headers.
#include "../palette.h"
#include "../widgets.h"

#include <SDL.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace kg::gui {

struct Stage::Impl {
  SDL_Renderer* ren = nullptr;
  Display* dpy = nullptr;
  Window root = 0;
  int screen = 0;
  int w = 0, h = 0;
  bool xtest = false;
  KeyCode spare = 0;

  PixelShape shape = PixelShape::Unsupported;
  bool shape_known = false;

  SDL_Texture* tex = nullptr;   // touched only on the UI thread

  std::thread capture;
  std::atomic<bool> stop{false};
  std::atomic<bool> dead{false};
  std::atomic<bool> focus{false};

  // The double buffer. The capture thread fills `back` with no lock held -
  // nothing else ever looks at it - and then swaps it with `front` under the
  // mutex. The UI thread holds the mutex only for the length of one upload,
  // never for the length of an XGetImage, which is the whole point: XGetImage
  // is a synchronous round trip to the X server, not a memcpy, and on the UI
  // thread it would stall the frame for as long as the server took to answer.
  std::mutex m;
  std::vector<unsigned char> front, back;
  int pitch = 0;
  bool have_new = false;
  // Written by the capture thread, read by the UI thread, and therefore under
  // the same lock as everything else the two share. It was the one field that
  // was not, and a std::string read while another thread assigns to it is a
  // torn pointer and a length that do not belong together - undefined
  // behaviour, not a stale sentence.
  std::string trouble;

  void set_trouble(const std::string& t) {
    std::lock_guard<std::mutex> lk(m);
    trouble = t;
  }

  // What the person did, in the order they did it. Motion goes in here with
  // everything else so that a click cannot overtake the characters typed
  // before it - which, into an installer's key box, would mean pressing Next
  // on an empty field.
  struct Ev {
    enum class Kind { Motion, Button, Char, Key } kind = Kind::Motion;
    int a = 0, b = 0;
    bool down = false;
  };
  std::deque<Ev> queue;

  // Touched only on the UI thread, in draw() and forward_input(): what this
  // stage has put down inside the nested server and still owes an up for.
  HeldInput buttons, keys;
};

bool Stage::has_focus() const { return p_->focus.load(); }

std::string Stage::trouble() const {
  std::lock_guard<std::mutex> lk(p_->m);
  return p_->trouble;
}

Stage::Stage(SDL_Renderer* r, const std::string& display) : p_(new Impl) {
  p_->ren = r;
  // Our own connection. The worker thread inside run_in_compositor has none -
  // it talks to wine and to processes - but `kretro input` may have one to the
  // same display, and two Xlib connections is the simplest correct arrangement
  // for two things that want to talk at the same time.
  p_->dpy = XOpenDisplay(display.c_str());
  if (!p_->dpy) {
    p_->dead = true;
    // Said as a display: a bare ":57" reads like a line number.
    p_->set_trouble("could not open X display " + display);
    return;
  }
  // A live connection, whatever address it was handed. The previous install's
  // display died and was freed, and malloc may well have given us the same
  // pointer back; without this the second install of a session would think its
  // own brand new connection was already dead.
  dead_displays().opened(p_->dpy);
  p_->screen = DefaultScreen(p_->dpy);
  p_->root = RootWindow(p_->dpy, p_->screen);

  XWindowAttributes at{};
  if (!XGetWindowAttributes(p_->dpy, p_->root, &at) || at.width <= 0 || at.height <= 0) {
    p_->dead = true;
    p_->set_trouble("the root window of X display " + display + " has no size");
    return;
  }
  p_->w = at.width;
  p_->h = at.height;

  int ev = 0, err = 0, major = 0, minor = 0;
  p_->xtest = XTestQueryExtension(p_->dpy, &ev, &err, &major, &minor) == True;
  if (!p_->xtest) {
    // Worth saying rather than silently swallowing every click: a panel you can
    // watch but not touch is a bug report waiting to be filed.
    p_->set_trouble("no XTEST on this display - you can watch, but not click");
  }

  p_->capture = std::thread([this] { capture_loop(); });
}

Stage::~Stage() {
  p_->stop = true;
  if (p_->capture.joinable()) p_->capture.join();
  if (p_->tex) SDL_DestroyTexture(p_->tex);

  // Never on a connection the I/O error handler has already condemned: closing
  // it would walk structures the server has stopped answering for.
  //
  // And armed even when it has not been. A display can be dead without anybody
  // having noticed - the capture thread stopped on a bad frame, or was never
  // reading in the first place, and the compositor went away afterwards with
  // nothing talking to the connection to find out. XCloseDisplay is then the
  // request that finds out, on the UI thread, with the handler unarmed: it
  // returns, and Xlib's contract for a handler that returns is exit(1). The
  // whole shelf disappearing as a wizard step is torn down, out of a
  // destructor, in the middle of a frame.
  //
  // Nothing with a destructor is live across the jump, and dpy is volatile for
  // the reason grab_frame's img is. The half-closed Display is leaked, which
  // is what the server dying under a close leaves behind either way.
  Display* volatile dpy = p_->dpy;
  p_->dpy = nullptr;
  if (!dpy || dead_displays().dead(dpy)) return;
  if (setjmp(g_x_jump)) {
    g_x_armed = false;
    return;
  }
  g_x_armed = true;
  XCloseDisplay(dpy);
  g_x_armed = false;
}

bool Stage::grab_frame() {
  Impl* p = p_.get();
  if (p->dead.load() || dead_displays().dead(p->dpy)) return false;

  // Nothing with a destructor inside the guarded region, and img is not read
  // on the failure path - a longjmp leaves its value indeterminate by
  // definition, and volatile is what stops -Wclobbered being right about it.
  XImage* volatile img = nullptr;
  if (setjmp(g_x_jump)) {
    g_x_armed = false;
    p->dead = true;
    p->set_trouble("the display went away");
    return false;
  }
  g_x_armed = true;
  img = XGetImage(p->dpy, p->root, 0, 0, static_cast<unsigned>(p->w),
                  static_cast<unsigned>(p->h), AllPlanes, ZPixmap);
  g_x_armed = false;

  if (!img) {
    p->dead = true;
    p->set_trouble("the root window of this display cannot be read");
    return false;
  }

  if (!p->shape_known) {
    p->shape = pixel_shape(img->depth, img->bits_per_pixel,
                           static_cast<unsigned long>(img->red_mask),
                           static_cast<unsigned long>(img->blue_mask));
    p->shape_known = true;
    if (p->shape == PixelShape::Unsupported) {
      // Refuse politely. Uploading a layout we did not expect draws something
      // that looks like a picture and is not, and a person cannot tell that
      // from a game with strange colours.
      char b[192];
      std::snprintf(b, sizeof(b),
                    "this display is depth %d at %d bits per pixel, red mask %08lx - "
                    "not a layout the stage can upload",
                    img->depth, img->bits_per_pixel, static_cast<unsigned long>(img->red_mask));
      p->set_trouble(b);
      XDestroyImage(img);
      p->dead = true;
      return false;
    }
  }

  const size_t need =
      static_cast<size_t>(img->bytes_per_line) * static_cast<size_t>(p->h);
  if (p->back.size() != need) p->back.resize(need);
  std::memcpy(p->back.data(), img->data, need);
  {
    std::lock_guard<std::mutex> lk(p->m);
    p->front.swap(p->back);
    p->pitch = img->bytes_per_line;
    p->have_new = true;
  }
  // Every frame, no exceptions. An XImage is a heap allocation the size of the
  // screen; at twenty a second, forgetting this is two gigabytes a minute.
  XDestroyImage(img);
  return true;
}

void Stage::capture_loop() {
  Impl* p = p_.get();
  while (!p->stop.load()) {
    // Four events a turn, not the whole queue. Typing pauses thirty
    // milliseconds a character, and draining twenty of them before the next
    // capture would freeze the picture for two thirds of a second in the
    // middle of somebody entering a CD key - which looks exactly like a crash.
    if (!drain_input(4)) return;
    if (!grab_frame()) return;
    usleep(50000);
  }
}

bool Stage::refresh() {
  Impl* p = p_.get();
  std::lock_guard<std::mutex> lk(p->m);
  if (p->have_new && p->w > 0 && p->h > 0 && !p->front.empty()) {
    if (!p->tex) {
      // Streaming, because this texture is replaced in its entirety twenty
      // times a second; a static texture is for art that is uploaded once.
      p->tex = SDL_CreateTexture(
          p->ren,
          p->shape == PixelShape::Abgr8888 ? SDL_PIXELFORMAT_ABGR8888 : SDL_PIXELFORMAT_ARGB8888,
          SDL_TEXTUREACCESS_STREAMING, p->w, p->h);
    }
    if (p->tex) SDL_UpdateTexture(p->tex, nullptr, p->front.data(), p->pitch);
    p->have_new = false;
  }
  return !p->dead.load() && !dead_displays().dead(p->dpy);
}

namespace {

// A frame of thickness t drawn just outside the rectangle a..b, as four filled
// bars rather than AddRect: the software renderer steps a thick outlined
// rectangle, and outside rather than on the edge so the ring never covers a
// pixel of the installer's own picture.
void outer_frame(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, ImU32 c) {
  dl->AddRectFilled(ImVec2(a.x - t, a.y - t), ImVec2(b.x + t, a.y), c);
  dl->AddRectFilled(ImVec2(a.x - t, b.y), ImVec2(b.x + t, b.y + t), c);
  dl->AddRectFilled(ImVec2(a.x - t, a.y), ImVec2(a.x, b.y), c);
  dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(b.x + t, b.y), c);
}

// The stage's trouble, as a strip across the top of the panel: a boot-log chip
// and the sentence wrapped under the panel's width. A display that has died is
// a failure; anything short of that (no XTEST, so watch but not click) is a
// warning. Drawn on the list, not as items, because the panel is one item.
//
// It has a band of the panel to itself, above the picture rather than over it:
// laid across the picture it hid the top of the installer's own window and ran
// through the focus ring. trouble_strip_height is that band's height.
struct StripText {
  ImFont* f;
  float fs, chip_w, wrap;
  ImVec2 pad, size;
};

StripText strip_text(float width, const std::string& text) {
  StripText t;
  t.f = ImGui::GetFont();
  t.fs = ImGui::GetFontSize();
  t.pad = px(12, 8);
  t.chip_w = t.f->CalcTextSizeA(t.fs, FLT_MAX, 0.0f, "[WARN]").x + px(10);
  t.wrap = std::max(px(40), width - 2.0f * t.pad.x - t.chip_w);
  t.size = t.f->CalcTextSizeA(t.fs, FLT_MAX, t.wrap, text.c_str());
  return t;
}

float trouble_strip_height(float width, const std::string& text) {
  const StripText t = strip_text(width, text);
  return std::round(t.size.y + 2.0f * t.pad.y);
}

void trouble_strip(ImDrawList* dl, ImVec2 box0, ImVec2 box1, float hair, const std::string& text,
                   bool dead) {
  const StripText t = strip_text(box1.x - box0.x, text);
  ImFont* f = t.f;
  const float fs = t.fs;
  const char* word = dead ? "FAIL" : "WARN";
  const ImVec4 colour = badge_colour(dead ? BadgeKind::Fail : BadgeKind::Warn);
  const float bracket_w = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, "[").x;

  const float bottom = std::min(box1.y, box0.y + std::round(t.size.y + 2.0f * t.pad.y));
  dl->PushClipRect(box0, box1, true);
  dl->AddRectFilled(box0, ImVec2(box1.x, bottom), u32(kBg0));
  dl->AddRectFilled(ImVec2(box0.x, bottom - hair), ImVec2(box1.x, bottom), u32(colour, 0.35f));

  const ImVec2 at(box0.x + t.pad.x, box0.y + t.pad.y);
  const float word_w = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, word).x;
  const float cells = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, "WARN").x;
  dl->AddText(f, fs, at, u32(kDim), "[");
  dl->AddText(f, fs, ImVec2(at.x + bracket_w + (cells - word_w) * 0.5f, at.y), u32(colour), word);
  dl->AddText(f, fs, ImVec2(at.x + bracket_w + cells, at.y), u32(kDim), "]");
  // The sentence in the colour the wizard gives its own: a failure's kWarn,
  // a warning's kWarm.
  dl->AddText(f, fs, ImVec2(at.x + t.chip_w, at.y), u32(dead ? kWarn : kWarm), text.c_str(), nullptr,
              t.wrap);
  dl->PopClipRect();
}

// Words centred in the rectangle a..b on the window's font: a turning spinner
// in the accent and `text` dim beside it, the way the rest of the program
// says it is waiting.
void waiting_line(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* text) {
  ImFont* f = ImGui::GetFont();
  const float fs = ImGui::GetFontSize();
  const char* glyph = spinner_glyph();
  const float gw = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, glyph).x;
  const float sp = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, " ").x;
  const std::string shown = elide(f, text, std::max(1.0f, b.x - a.x - gw - sp - px(16)));
  const float tw = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, shown.c_str()).x;
  const float x = std::round((a.x + b.x - gw - sp - tw) * 0.5f);
  const float y = std::round((a.y + b.y - fs) * 0.5f);
  dl->AddText(f, fs, ImVec2(x, y), u32(kAccent), glyph);
  dl->AddText(f, fs, ImVec2(x + gw + sp, y), u32(kDim), shown.c_str());
}

}  // namespace

void Stage::draw(ImVec2 max) {
  Impl* p = p_.get();
  // The tile idiom from the shelf, for the same reasons: an InvisibleButton is
  // the hit target and the thing gamepad navigation can land on, the picture
  // goes on the window draw list underneath it, and the focus ring is the same
  // amber the tiles use so that "this is what your input is going to" means one
  // thing everywhere in the program. ImGui leaves an invisible button out of
  // navigation unless it is asked in, and then only hovering gave the stage
  // the input: a keyboard alone could never type into an installer.
  //
  // Keys go to the installer while the stage has the focus, as they did while
  // the mouse was over it. A modal over the stage takes the focus, and the
  // hover, with it, so nothing typed into a question reaches the installer.
  // Tab stays the stage's while it has the focus, so walking a dialog with it
  // does not also walk ImGui's focus off the stage.
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 box(max.x > 1 ? max.x : 1, max.y > 1 ? max.y : 1);
  // ImGui's own focus rectangle is left out, as the tile's is: the ring
  // below is the stage's focus.
  ImGui::PushStyleColor(ImGuiCol_NavCursor, alpha(kAmber, 0.0f));
  ImGui::InvisibleButton("stage", box, ImGuiButtonFlags_EnableNav);
  ImGui::PopStyleColor();
  // The page's glow is not drawn round the panel: the stage draws its own
  // round the installer's picture, which is what the input goes to.
  own_focus_ring();
  own_tab_while_focused();
  const bool keys = nav_focused();
  p->focus = ImGui::IsItemFocused() || ImGui::IsItemHovered();

  SDL_Texture* tex = nullptr;
  std::string trouble;
  {
    std::lock_guard<std::mutex> lk(p->m);
    tex = p->tex;
    trouble = p->trouble;
  }

  // The picture is fitted inside the panel less the trouble strip's band and
  // the focus ring's width all round, so neither the strip nor the ring ever
  // covers a pixel of the installer's picture or crosses the panel's edge.
  // Input is unmapped against the same area, so a click lands where it looks.
  const float hair = std::max(1.0f, std::round(px(1)));
  const float ring = std::max(1.0f, std::round(px(2)));
  const float strip = trouble.empty() ? 0.0f : trouble_strip_height(box.x, trouble);
  const ImVec2 area0(p0.x + ring, p0.y + strip + ring);
  const ImVec2 area(std::max(0.0f, box.x - ring * 2), std::max(0.0f, box.y - strip - ring * 2));
  const Fit f = fit_inside(p->w, p->h, area.x, area.y);
  const ImVec2 a(area0.x + f.x, area0.y + f.y);
  const ImVec2 b(a.x + f.w, a.y + f.h);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // The letterbox is a panel one step up from the page with a hairline round
  // it, so the installer reads as a screen set into the page rather than as a
  // picture floating on it, and the bars either side of a 4:3 installer have
  // an edge.
  const ImVec2 box1(p0.x + box.x, p0.y + box.y);
  dl->AddRectFilled(p0, box1, u32(kBg1));
  outer_frame(dl, p0, box1, hair, u32(kLine));

  if (tex && f.w > 0) {
    // In pieces, as every large picture is: the software renderer breaks one
    // drawn whole.
    Texture t;
    t.tex = tex;
    t.w = p->w;
    t.h = p->h;
    draw_image(dl, t, a, b);
  } else {
    // Before the first frame arrives, a dark rectangle the size the installer
    // will be, so the panel does not jump when the picture appears. While
    // nothing has gone wrong it says it is waiting, in the words and the
    // spinner the page uses before the stage exists, so a slow installer does
    // not look like a hung one; once something has, it says there is no
    // picture, rather than leaving a black box that looks like one.
    const Fit g = fit_inside(p->w > 0 ? p->w : 4, p->h > 0 ? p->h : 3, area.x, area.y);
    const ImVec2 g0(area0.x + g.x, area0.y + g.y);
    const ImVec2 g1(g0.x + g.w, g0.y + g.h);
    dl->AddRectFilled(g0, g1, u32(kBg0));
    if (g.w > 0) outer_frame(dl, g0, g1, hair, u32(kLine));
    if (g.w > 0) {
      if (trouble.empty()) {
        waiting_line(dl, g0, g1, "waiting for the installer's first frame...");
      } else {
        const char* none = "no picture from the installer";
        const ImVec2 ts = ImGui::CalcTextSize(none);
        dl->AddText(ImVec2(std::round((g0.x + g1.x - ts.x) * 0.5f), std::round((g0.y + g1.y - ts.y) * 0.5f)),
                    u32(kDim), none);
      }
    }
  }

  if (!trouble.empty()) trouble_strip(dl, p0, box1, hair, trouble, p->dead.load());
  // Last, so nothing is drawn over it.
  // The keys' ring is the glow every focused item has, so the stage reads
  // as one more thing the focus can be on; under the mouse alone, the
  // quieter frame a hovered item has, as when a click put the focus there.
  if (keys && f.w > 0) focus_glow(dl, ImVec2(a.x - ring, a.y - ring), ImVec2(b.x + ring, b.y + ring), 1.0f);
  else if (p->focus.load() && f.w > 0) outer_frame(dl, a, b, ring, u32(kAccentDim));
  // Focused, or still owed an up. A button dragged off the panel takes the
  // hover with it, and the frame the person lets go on is a frame the panel
  // does not have: forwarding only while focused is how a press got through
  // and its release did not.
  const bool live = p->focus.load();
  if (live || p->buttons.any() || p->keys.any()) forward_input(area0, f, live);
}

void Stage::forward_input(ImVec2 origin, const Fit& f, bool live) {
  Impl* p = p_.get();
  if (p->dead.load() || !p->xtest) return;
  ImGuiIO& io = ImGui::GetIO();

  std::vector<Impl::Ev> out;
  int x = 0, y = 0;
  const bool over =
      live && unmap_point(io.MousePos.x - origin.x, io.MousePos.y - origin.y, f, p->w, p->h,
                          &x, &y);
  if (over) {
    Impl::Ev m;
    m.kind = Impl::Ev::Kind::Motion;
    m.a = x;
    m.b = y;
    out.push_back(m);
  }

  // ImGui's 0/1/2 are left/right/middle; X's 1/2/3 are left/middle/right.
  // Getting this pair the wrong way round puts the context menu on every
  // click, which looks like the installer misbehaving rather than us.
  //
  // The press is asked of the ledger, which answers no unless the pointer is
  // over the picture. The release is asked of the ledger too, and it answers
  // yes wherever the pointer has got to, so long as the press was ours: a
  // scrollbar dragged into the letterbox comes back up.
  static const int kXButton[3] = {1, 3, 2};
  for (int i = 0; i < 3; ++i) {
    if (ImGui::IsMouseClicked(i) && p->buttons.press(kXButton[i], over)) {
      Impl::Ev e;
      e.kind = Impl::Ev::Kind::Button;
      e.a = kXButton[i];
      e.down = true;
      out.push_back(e);
    }
    if (ImGui::IsMouseReleased(i) && p->buttons.release(kXButton[i])) {
      Impl::Ev e;
      e.kind = Impl::Ev::Kind::Button;
      e.a = kXButton[i];
      e.down = false;
      out.push_back(e);
    }
  }
  // A wheel is buttons 4 and 5 to every X program written before 2005, which
  // is all of them here. Capped at three notches a frame so that a flick of a
  // modern high-resolution wheel does not send forty. Down and up in the same
  // breath, so there is nothing for the ledger to remember.
  if (over && io.MouseWheel != 0.0f) {
    const int button = io.MouseWheel > 0 ? 4 : 5;
    int n = static_cast<int>(io.MouseWheel > 0 ? io.MouseWheel : -io.MouseWheel);
    if (n < 1) n = 1;
    if (n > 3) n = 3;
    for (int i = 0; i < n; ++i) {
      Impl::Ev down;
      down.kind = Impl::Ev::Kind::Button;
      down.a = button;
      down.down = true;
      out.push_back(down);
      Impl::Ev up = down;
      up.down = false;
      out.push_back(up);
    }
  }

  // Characters only while the panel has the input. A frame we are only here
  // for the sake of an owed release is not a frame anybody is typing into.
  for (int i = 0; i < (live ? io.InputQueueCharacters.Size : 0); ++i) {
    const unsigned long ks = keysym_for_char(static_cast<unsigned int>(io.InputQueueCharacters[i]));
    if (!ks) continue;
    Impl::Ev e;
    e.kind = Impl::Ev::Kind::Char;
    e.a = static_cast<int>(ks);
    out.push_back(e);
  }

  // The keys that are not characters. Space is deliberately absent: it arrives
  // as a character too, and sending it twice puts two spaces in the key box.
  //
  // Escape is absent because Escape is how you leave the stage. It is also how
  // you back out of an InstallShield dialog, so this is a real cost and the
  // hint line under the panel has to say so.
  static const struct {
    ImGuiKey key;
    unsigned long ks;
  } kNamed[] = {
      {ImGuiKey_Enter, XK_Return},        {ImGuiKey_KeypadEnter, XK_KP_Enter},
      {ImGuiKey_Tab, XK_Tab},             {ImGuiKey_Backspace, XK_BackSpace},
      {ImGuiKey_Delete, XK_Delete},       {ImGuiKey_Insert, XK_Insert},
      {ImGuiKey_LeftArrow, XK_Left},      {ImGuiKey_RightArrow, XK_Right},
      {ImGuiKey_UpArrow, XK_Up},          {ImGuiKey_DownArrow, XK_Down},
      {ImGuiKey_Home, XK_Home},           {ImGuiKey_End, XK_End},
      {ImGuiKey_PageUp, XK_Prior},        {ImGuiKey_PageDown, XK_Next},
      {ImGuiKey_F1, XK_F1},               {ImGuiKey_F2, XK_F2},
      {ImGuiKey_F3, XK_F3},               {ImGuiKey_F4, XK_F4},
  };
  // The same ledger, for the same reason. A key held while the pointer wanders
  // out of the panel - Tab held down to walk an InstallShield dialog, an arrow
  // held to run a list - is a key the nested server goes on repeating until
  // somebody lifts it, and only we can.
  for (const auto& n : kNamed) {
    const int ks = static_cast<int>(n.ks);
    if (ImGui::IsKeyPressed(n.key, false) && p->keys.press(ks, live)) {
      Impl::Ev e;
      e.kind = Impl::Ev::Kind::Key;
      e.a = ks;
      e.down = true;
      out.push_back(e);
    }
    if (ImGui::IsKeyReleased(n.key) && p->keys.release(ks)) {
      Impl::Ev e;
      e.kind = Impl::Ev::Kind::Key;
      e.a = ks;
      e.down = false;
      out.push_back(e);
    }
  }

  if (out.empty()) return;
  std::lock_guard<std::mutex> lk(p->m);
  for (const Impl::Ev& e : out) p->queue.push_back(e);
}

unsigned char Stage::find_spare_keycode() {
  Impl* p = p_.get();
  int lo = 0, hi = 0;
  XDisplayKeycodes(p->dpy, &lo, &hi);
  int per = 0;
  KeySym* map = XGetKeyboardMapping(p->dpy, static_cast<KeyCode>(lo), hi - lo + 1, &per);
  if (!map) return 0;
  unsigned char found = 0;
  for (int i = 0; i <= hi - lo && !found; ++i) {
    bool empty = true;
    for (int j = 0; j < per; ++j) {
      if (map[i * per + j] != NoSymbol) empty = false;
    }
    if (empty) found = static_cast<unsigned char>(lo + i);
  }
  XFree(map);
  return found;
}

void Stage::type_char(unsigned long ks) {
  Impl* p = p_.get();
  if (!p->spare) p->spare = find_spare_keycode();
  if (!p->spare) return;   // a full keymap; nothing sensible to do but drop it

  // Always through a spare keycode, never through the current map.
  //
  // A character that is on the map may need Shift, or AltGr, to reach - and
  // synthesising a modifier state that the installer's own idea of the
  // keyboard agrees with is exactly the thing that fails invisibly: you type
  // ABCD-1234 into the key box, the installer receives abcd-1234, and it tells
  // you the CD key is wrong. Binding the keysym itself to an unused keycode
  // means there is no modifier to get wrong, and it is also what makes a key
  // with characters that are on no key at all typeable.
  KeySym k = static_cast<KeySym>(ks);
  XChangeKeyboardMapping(p->dpy, p->spare, 1, &k, 1);
  XSync(p->dpy, False);
  XTestFakeKeyEvent(p->dpy, p->spare, True, 0);
  XTestFakeKeyEvent(p->dpy, p->spare, False, 0);
  XSync(p->dpy, False);
  KeySym none = NoSymbol;
  XChangeKeyboardMapping(p->dpy, p->spare, 1, &none, 1);
  XSync(p->dpy, False);
  // Installers of this era drop keys sent faster than a person types them -
  // the same thirty milliseconds `kretro type` used, from the same discovery.
  usleep(30000);
}

bool Stage::drain_input(int budget) {
  // One setjmp for the whole batch rather than one per event, and p volatile
  // for the same reason grab_frame's img is: p is read on the failure path,
  // and a longjmp leaves an ordinary local's value indeterminate by
  // definition. Nothing else may be live here - the loop counter is declared
  // below this point precisely so that it is not, which is what -Wclobbered is
  // telling you when you get it wrong. The jump lands before the loop, so a
  // display that dies on the third event returns false exactly as one that
  // dies on the first does.
  Impl* volatile p = p_.get();
  if (setjmp(g_x_jump)) {
    g_x_armed = false;
    p->dead = true;
    p->set_trouble("the display went away");
    return false;
  }
  for (int i = 0; i < budget && !p->stop.load(); ++i) {
    Impl::Ev ev;
    {
      std::lock_guard<std::mutex> lk(p->m);
      if (p->queue.empty()) return true;
      ev = p->queue.front();
      p->queue.pop_front();
    }
    g_x_armed = true;
    switch (ev.kind) {
      case Impl::Ev::Kind::Motion:
        XTestFakeMotionEvent(p->dpy, p->screen, ev.a, ev.b, 0);
        break;
      case Impl::Ev::Kind::Button:
        XTestFakeButtonEvent(p->dpy, static_cast<unsigned>(ev.a), ev.down ? True : False, 0);
        break;
      case Impl::Ev::Kind::Key: {
        KeyCode kc = XKeysymToKeycode(p->dpy, static_cast<KeySym>(ev.a));
        if (kc) XTestFakeKeyEvent(p->dpy, kc, ev.down ? True : False, 0);
        break;
      }
      case Impl::Ev::Kind::Char:
        type_char(static_cast<unsigned long>(ev.a));
        break;
    }
    XFlush(p->dpy);
    g_x_armed = false;
  }
  return true;
}

}  // namespace kg::gui
