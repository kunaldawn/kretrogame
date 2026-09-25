// Where the keyboard and the pad are: which item a page opens on, where the
// focus goes back to when a page comes up again, which device was used last,
// and the keys a page, a modal and a panel of text answer.
//
// Dear ImGui moves the focus between items by itself. What it does not know is
// what a page is for, so on its own a page opens on whatever item happens to
// be first, forgets where the focus was the moment the page goes, and shows no
// focus at all until an arrow has been pressed once. Everything here is drawing
// state for one window; none of it changes what any page does.
#pragma once

#include <string>

#include "imgui.h"

namespace kg::gui {

// ---- the device used last ---------------------------------------------------

// The keyboard or the pad shows the focus at all times, as a console does; the
// mouse shows it only once a key or a button has been pressed again. The event
// loop sets this from every event (event_loop.h, note_input).
enum class InputMode { Mouse,
                       Keyboard,
                       Pad };
InputMode input_mode();
void set_input_mode(InputMode mode);

// ---- which item a page opens on ---------------------------------------------

// True while a page is drawn on the frame it came up: its window appeared, or
// the key it gave set_page_focus_key() changed. Also what a page's copy of
// something it reads from disk goes by (Refresh, below).
bool page_appearing();

// Splits a page's focus memory, for a page that is several views under one
// window, as the Bundles editor's steps are: a change of key counts as the
// page coming up again. Set before the page's PageWindow, and it holds for
// that one frame, as set_chrome_path does.
void set_page_focus_key(const std::string& key);

// Right after the item a page should open on. When the page comes up, the
// focus goes back to the item it was on when the page was last shown, if that
// item is still there, and to this one otherwise. `over_memory` makes this
// item win even then, for a page whose right answer changes while it is away,
// as the shelf's tile for the game last opened does.
void default_focus(bool over_memory = false);
// Before a run of items: the first item after this that the keyboard can
// reach is the default, as though default_focus() followed it. For a view
// whose first item is not known where the view is chosen.
void default_focus_next();

// Forgets every page's remembered focus, so every page next opens as it does
// the first time. For tests and tools.
void forget_focus();

// ---- a page's sections ----------------------------------------------------------

// Around each of a page's parts that the shoulder buttons and Tab move
// between, as a console's library moves between its sidebar, its row of
// recent games and its grid: with two or more on a page, the pad's RB and Tab
// take the focus to the next section and LB and Shift+Tab to the one before,
// going round. The focus lands on the item it was last on in that section, or
// on the section's first item. Only the focus moves; nothing is pressed. Not
// while a field is being typed in or a popup is up. Sections do not nest.
void nav_section_begin(const char* id);
void nav_section_end();

// Called by begin_page right after the page's window begins, and by
// PageWindow just before it ends. Nothing else calls them.
void page_focus_begin();
void page_focus_end();

// ---- drawing the focus ------------------------------------------------------

// Whether the last item has the keyboard or pad focus and the focus is being
// shown. The amber that means "this is where your keys go" is drawn only
// then; a mouse hovering an item is shown some other way.
bool nav_focused();

// The same for an item by its ID, for a widget drawn by hand: whether it has
// the keyboard or pad focus and the focus is shown, and in `t`, when given, a
// 0..1 that eases in over about 100 ms as it takes the focus and out as it
// loses it, for the item's own grow or brighten. The ring round it is not the
// item's to draw: draw_focus_glow draws it.
bool focus_state(ImGuiID id, float* t = nullptr);

// The glow round whatever has the keyboard or pad focus, drawn once a frame by
// the window (end_frame) over every item alike, so no widget draws its own:
// an amber frame with a soft halo that breathes, gliding from the item the
// focus left to the one it went to over about 120 ms. It snaps instead when
// the focus moves to another window or across more than 40% of the screen.
void draw_focus_glow();
// Right after an item that is drawn larger or smaller than the rectangle
// ImGui knows it by (a tile that grows while it has the focus): the glow goes
// round `a`..`b` this frame instead.
void focus_glow_rect(ImVec2 a, ImVec2 b);
// Right after an item that draws a focus ring of its own (the installer's
// stage): no glow round it.
void own_focus_ring();
// The ring itself, `a`..`b` on `dl`, at strength `t` (0..1): for a widget that
// wants the same look somewhere the glow is not drawn.
void focus_glow(ImDrawList* dl, ImVec2 a, ImVec2 b, float t);

// How many frames ago the page being drawn came up: 0 on the frame it
// appeared. Scrolling snaps rather than glides for the first few, so a page
// opens already scrolled to where its focus is.
int page_age();

// In a grid, after its items: Right at the end of a row goes on to the start
// of the next one, and Left at the start of a row back to the end of the one
// before, as in a console's library.
void wrap_rows();

// Around the tiles of a region that scrolls, as the shelf's main column does:
// while the focus is on one of them, Home and End take it to the first tile
// and the last, and PageUp and PageDown scroll the region by most of its
// height and take it to the tile nearest where it was on the screen. Not while
// a popup is up or a field is typed in. tiles_begin() right after the region
// begins, tiles_item() right after each tile, and tiles_end() before the
// region ends. They do not nest.
void tiles_begin();
void tiles_item();
void tiles_end();

// While the last item has the focus, Tab belongs to it and not to ImGui's own
// moving between items. For the installer's stage, which passes Tab on to an
// installer walking its dialog.
void own_tab_while_focused();

// ---- the keys a page and a modal answer ---------------------------------------

// Whether Escape, or the pad's B, is the page's to act on: not while a modal
// or any other popup is up, which answers the key itself, and not while a
// field is being typed in, where Escape only leaves the field.
bool back_allowed();
// Inside a modal: true on the frame Escape or the pad's B is pressed for it,
// for the modal to close itself with its safe answer. Not while a field in it
// is being typed in, nor while a popup of its own (a combo's list) is open.
bool modal_cancelled();

// Inside a child window of text with nothing in it to press - a log, a report,
// the licences - right after it begins. The window is one stop for the keys as
// a whole; while it has the focus, Up and Down scroll it a couple of lines,
// PageUp and PageDown a window's height, Home and End to either end, and the
// sticks smoothly. Up or Down at the end the text is scrolled to moves on as
// they do anywhere else. PageUp, PageDown, Home, End and the sticks also
// scroll it while the mouse is over it and no focus is shown.
void scroll_with_keys();
// Right before the EndChild of a window scroll_with_keys() began.
void scroll_with_keys_end();

// ---- reading from disk ------------------------------------------------------

// When a page's copy of something it reads from disk - a journal, a listing,
// a config - is due to be read again. Read on every frame instead, a long
// journal or a big folder is thousands of file reads a second and the page
// stutters. It is due on the frame the page comes up, when `key` names
// something else than last time (another game, a collection read again after
// a session or a build), and after invalidate() (the page changed it itself).
// With `every` above 0, also every that many seconds, for what changes under
// a page from outside it, as a folder being listed does.
class Refresh {
 public:
  bool due(const std::string& key, double every = 0.0);
  void invalidate() { read_at_ = -1.0; }

 private:
  std::string key_;
  double read_at_ = -1.0;
};

}  // namespace kg::gui
