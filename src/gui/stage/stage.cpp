#include "stage.h"

#include "x_errors.h"

#include <SDL.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <unistd.h>

#include <atomic>
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
    p_->set_trouble("could not open " + display);
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
    p_->set_trouble("the root window of " + display + " has no size");
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

void Stage::draw(ImVec2 max) {
  Impl* p = p_.get();
  // The tile idiom from the shelf, for the same reasons: an InvisibleButton is
  // the hit target and the thing gamepad navigation can land on, the picture
  // goes on the window draw list underneath it, and the focus ring is the same
  // amber the tiles use so that "this is what your input is going to" means one
  // thing everywhere in the program.
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("stage", ImVec2(max.x > 1 ? max.x : 1, max.y > 1 ? max.y : 1));
  p->focus = ImGui::IsItemFocused() || ImGui::IsItemHovered();

  const Fit f = fit_inside(p->w, p->h, max.x, max.y);
  const ImVec2 a(p0.x + f.x, p0.y + f.y);
  const ImVec2 b(a.x + f.w, a.y + f.h);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  SDL_Texture* tex = nullptr;
  std::string trouble;
  {
    std::lock_guard<std::mutex> lk(p->m);
    tex = p->tex;
    trouble = p->trouble;
  }
  if (tex && f.w > 0) {
    dl->AddImage(reinterpret_cast<ImTextureID>(tex), a, b);
  } else {
    // Before the first frame arrives, a black rectangle the size the installer
    // will be, so the panel does not jump when the picture appears.
    const Fit g = fit_inside(p->w > 0 ? p->w : 4, p->h > 0 ? p->h : 3, max.x, max.y);
    dl->AddRectFilled(ImVec2(p0.x + g.x, p0.y + g.y),
                      ImVec2(p0.x + g.x + g.w, p0.y + g.y + g.h), IM_COL32(0, 0, 0, 255));
  }

  if (p->focus.load() && f.w > 0) {
    dl->AddRect(a, b, IM_COL32(255, 214, 102, 255), 0.0f, 0, 3.0f);
  }
  if (!trouble.empty()) {
    dl->AddText(ImVec2(p0.x + 12, p0.y + 12), IM_COL32(255, 180, 180, 255), trouble.c_str());
  }
  // Focused, or still owed an up. A button dragged off the panel takes the
  // hover with it, and the frame the person lets go on is a frame the panel
  // does not have: forwarding only while focused is how a press got through
  // and its release did not.
  const bool live = p->focus.load();
  if (live || p->buttons.any() || p->keys.any()) forward_input(p0, f, live);
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
