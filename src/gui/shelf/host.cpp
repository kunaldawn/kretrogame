// The shelf: kretro's big-picture interface.
//
// Four screens and no settings screen, because everything configurable is
// per-game data. Navigable entirely by gamepad or by keyboard, and you can
// still type to filter, because that instinct is the right one.
#include "host.h"

#include <SDL.h>

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

#include "../../util/text.h"
#include "../focus.h"
#include "../job_modal.h"
#include "imgui.h"

namespace kg::gui::shelf {
namespace fs = std::filesystem;

ShelfHost::ShelfHost(const rt::Env& e, const Window& w)
    : ctx_(e, w.win, w.ren, w.fonts()),
      wizard_(ctx_, library_, w.ren),
      shelf_(ctx_),
      game_(ctx_, wizard_),
      import_(ctx_, wizard_),
      doctor_(ctx_),
      library_(ctx_, wizard_),
      settings_(ctx_),
      timeline_(ctx_),
      bundles_(ctx_) {
  ctx_.go = [this](Screen s) { pages_.go(s); };
  ctx_.nav = [this](char c) {
    if (!is_busy()) shortcut(c);
  };
  pages_.set(Screen::Shelf, shelf_);
  pages_.set(Screen::Game, game_);
  pages_.set(Screen::Create, wizard_);
  pages_.set(Screen::Import, import_);
  pages_.set(Screen::Doctor, doctor_);
  pages_.set(Screen::Library, library_);
  pages_.set(Screen::Settings, settings_);
  pages_.set(Screen::Timeline, timeline_);
  pages_.set(Screen::Bundles, bundles_);
  ctx_.reload();
}

ShelfHost::~ShelfHost() {
  // Nothing joined this before. A job on the worker outlived the window and
  // went on writing into the members of a half-destroyed shelf, and a Build
  // on it went on holding gigabytes of staging tree. Joined here, before any
  // page goes, because the jobs capture pages.
  ctx_.job.join();
}

// Three ways in, in the order the arguments are allowed to disagree: a game
// to open beats a wizard to open, and a wizard with a manifest behind it is
// open_for rather than create - the only difference is whether the draft
// starts from draft_from_meta.
void ShelfHost::start(const Startup& entry) {
  if (!entry.game.empty()) ctx_.open_game(entry.game);
  else if (entry.create && entry.preset.empty()) wizard_.create();
  else if (entry.create) wizard_.open_for(entry.preset);
}

void ShelfHost::frame() {
  ctx_.hide_for_game();
  pages_.page().draw();
  if (ctx_.job.running()) modal();
}

// Closing the window during an install must not take a running installer and
// a multi-gigabyte staging tree down without a word. The wizard asks first.
void ShelfHost::request_quit() {
  if (pages_.at(Screen::Create) && wizard_.busy()) {
    ctx_.quit_after_wizard = true;
    wizard_.ask_abandon();
    return;
  }
  // A game still being got ready stops at its next step; the host joins the
  // worker on the way out. One already playing is not stopped by this: it
  // ends when its player quits it, as it did when play held this thread.
  if (ctx_.playing()) ctx_.job.cancel();
  ctx_.quit = true;
}

// A page that threw in the middle of drawing itself. Nothing here can put
// the window down: the message goes where every other failure goes, the
// status line, and the person is left with a program to read it in.
void ShelfHost::page_failed(const std::string& what) { ctx_.status = what; }

// What the event loop hands on once it has done its own part: a dropped
// file, Escape and the pad's B, Backspace and typing.
void ShelfHost::on_event(const SDL_Event& ev) {
  if (ev.type == SDL_DROPFILE) {
    std::string path = ev.drop.file ? ev.drop.file : "";
    if (ev.drop.file) SDL_free(ev.drop.file);
    if (!path.empty()) dropped(path);
  }
  if (ev.type == SDL_KEYDOWN) {
    switch (ev.key.keysym.sym) {
      case SDLK_ESCAPE:
        // Escape reaches the shelf even while a job is running, because on
        // the wizard's install step it is the only way to abandon one.
        // back() knows which screens may act on it and which may not. A
        // modal answers Escape itself, and in a field it only leaves the
        // field.
        if (!back_allowed()) break;
        if (filtering()) clear_filter();
        else back();
        break;
      case SDLK_BACKSPACE:
        if (!is_busy()) type('\b');
        break;
      default: break;
    }
  }
  // The pad's B is Escape, but never quits: on the shelf itself it only
  // clears the filter, as a console's library does nothing on B at its root.
  if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_B && back_allowed()) {
    if (filtering()) clear_filter();
    else if (!pages_.at(Screen::Shelf)) back();
  }
  // Typing filters the shelf, the way dmenu does. Letters that are also
  // shortcuts only act as shortcuts when nothing is being typed.
  if (ev.type == SDL_TEXTINPUT && !is_busy()) {
    char c = ev.text.text[0];
    const bool acted = pages_.at(Screen::Shelf) && !filtering() && shortcut(c);
    if (!acted) type(c);
  }
}

// One list of the shelf's letters, for typing and for the sidebar alike.
bool ShelfHost::shortcut(char c) {
  struct Shortcut {
    char lower, upper;
    std::function<void()> act;
  };
  const Shortcut shortcuts[] = {
      {'a', 'A', [this] { wizard_.create(); }},
      {'i', 'I', [this] { pages_.go(Screen::Import); }},
      {'d', 'D', [this] { pages_.go(Screen::Doctor); }},
      {'l', 'L', [this] { pages_.go(Screen::Library); }},
      {'s', 'S', [this] { pages_.go(Screen::Settings); }},
      {'b', 'B', [this] { bundles_.open(); }},
  };
  for (const Shortcut& s : shortcuts) {
    if (c == s.lower || c == s.upper) {
      s.act();
      return true;
    }
  }
  return false;
}

void ShelfHost::back() {
  // The wizard is eight screens inside one Screen, so Escape has to ask it
  // first: dropping to the Shelf from the install step would walk away from
  // an installer that is halfway through writing a game. The Bundles page
  // has screens of its own too; every other page leaves it to the host.
  if (pages_.page().back()) return;
  if (ctx_.job.running()) return;  // a job of our own is up; the modal owns the screen
  if (!pages_.at(Screen::Shelf)) pages_.go(Screen::Shelf);
  else ctx_.quit = true;
}

// Dropping something on the shelf should do the obvious thing with it, and
// say what it decided rather than acting silently. A program that ignores
// what you hand it teaches you not to hand it anything.
void ShelfHost::dropped(const std::string& path) {
  std::error_code ec;
  fs::path p = path;
  // While the wizard's first step is up, anything dropped is a source for
  // it. Sending a disc off to the Library screen at that moment would throw
  // away the thing the user was in the middle of doing. Only the wizard's
  // page takes a drop.
  if (pages_.page().dropped(p)) return;
  if (fs::is_directory(p, ec)) {
    library_.add_folder(p);
    return;
  }
  std::string ext = to_lower(p.extension().string());
  if (ext == ".kgpack") {
    import_.open_file(p);
    return;
  }
  if (ext == ".iso" || ext == ".cue" || ext == ".bin" || ext == ".zip" || ext == ".7z" ||
      ext == ".exe" || ext == ".img" || ext == ".mdf") {
    library_.scan_only(p);
    return;
  }
  library_.note("I do not know what to do with " + p.filename().string());
}

// The filter is typed only on the shelf, and kept on every other screen.
void ShelfHost::type(char c) {
  if (!pages_.at(Screen::Shelf)) return;
  if (c == '\b') {
    if (!ctx_.filter.empty()) ctx_.filter.pop_back();
  } else if (c == ' ' && ctx_.filter.empty()) {
    // Space is also the key that opens the focused tile, so it only ever
    // goes on the end of a filter already being typed.
    return;
  } else if (c >= 32 && c < 127) ctx_.filter.push_back(c);
}

void ShelfHost::modal() {
  JobModalHooks hooks;
  hooks.footer_while_running = [this] {
    // On the baseline of the stop button, which sits at the right of the
    // dialog's band.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("working...");
    ImGui::SameLine();
    if (dialog_button("stop")) ctx_.job.cancel();
  };
  hooks.on_finished_frame = [this] { ctx_.after_play(); };
  hooks.on_close = [this] {
    ctx_.job.join();
    ctx_.reload();
  };
  draw_job_modal(ctx_.job, ctx_.fonts.big(), hooks);
}

}  // namespace kg::gui::shelf
