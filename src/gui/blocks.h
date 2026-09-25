// The larger pieces a page is built from, after a console's game library: a
// hero band over a game's art, a big play button with the stats beside it, a
// sidebar of places, rows and grids of tiles that grow as the focus reaches
// them, a stepper over a flow of steps and the footer that moves through it,
// and cards to group a page's sections.
//
// Every piece is one or more ordinary ImGui items under the label the caller
// gives, so a page that moves to one keeps its IDs, and the keyboard, the pad
// and the mouse reach it as they reach any button. Each works with no window
// and no pictures (the tests draw pages that way), and every length goes
// through px().
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "imgui.h"
#include "palette.h"
#include "texture.h"

namespace kg::gui {

// ---- art ---------------------------------------------------------------------

struct HeroSpec {
  // The picture, sharp (a screenshot, a cover), and its blurred copy
  // (Textures::backdrop_file / backdrop_png). Either may be missing.
  const Texture* art = nullptr;
  const Texture* backdrop = nullptr;
  // The game's id (its tint when there is no picture), its title, and the
  // small line over the title ("1996 · example-game").
  std::string id, title, kicker;
  // In design pixels; 0 is the breakpoint's (layout.h, hero_height).
  float height = 0;
  // The sharp picture framed at the right of the band, when there is room.
  bool art_right = true;
  // Design pixels at the band's top and at its foot that the page lays items
  // of its own over (a brand strip and a settings button; a play button and
  // its stats): the framed picture keeps below the top, and the title above
  // the foot.
  float top = 0, foot = 0;
  // The screen x the band's content stops at, for a page that keeps its text
  // to a centred column on a very wide window: the framed picture's right
  // edge. 0 is the band's own right margin.
  float content_right = 0;
  // The game the band showed before this one, and how far (0..1) the change
  // to this one has got: until it is done the old pictures show through the
  // new, so moving along a row of games cross-fades the band.
  const Texture* was_art = nullptr;
  const Texture* was_backdrop = nullptr;
  std::string was_id;
  float mix = 1.0f;
};
// A band across the whole width of the window: the backdrop filling it,
// darkened and scanlined, shading into the page at its foot and behind the
// text at its left; the sharp picture framed at the right; and the title in
// the hero type with the prompt block before it (two lines of the big type
// when it is long), the kicker over it. With no picture, the game's tint and
// scanlines, as its tile has. Takes its height on the page as one item. Fades
// in as its page comes up. Returns the screen x the band's text stops short
// of: the framed picture's left, less a margin, or the band's right margin.
float hero(const HeroSpec& spec);

// ---- actions -------------------------------------------------------------------

// What the big button does: play the game, install it, nothing yet (with the
// reason said beside it by the page), or nothing while something runs.
enum class PlayKind { Play,
                      Install,
                      Blocked,
                      Busy };
// The one big action a game has: a solid block in the accent (Play) or the
// information colour (Install), px(56) tall and at least px(220) wide (or
// `min_w` real pixels), with a drawn glyph and `shown` in bold capitals (by
// default the label in capitals). `label` is the ImGui label and ID, as
// ImGui::Button(label) has it. Blocked and Busy are drawn quiet and are never
// pressed, but still take the focus, so a pad can land on the button and the
// page can say why. Under BeginDisabled it is faded and cannot take the focus.
bool play_button(const char* label, PlayKind kind, float min_w = 0, const char* shown = nullptr);
// A secondary action: no fill, a frame, ordinary text; as tall as a play
// button's neighbours (px(40), px(36) on compact) unless `size` says.
// ImGui::Button's label and ID; `shown`, when given, is drawn instead of the
// label, which stays the ID.
bool ghost_button(const char* label, ImVec2 size = ImVec2(0, 0), const char* shown = nullptr);
// A figure for an action bar: "LAST PLAYED" small and dim in capitals over
// "3 days ago" in `colour`. One item; follow it with SameLine.
void stat(const char* label, const std::string& value, const ImVec4& colour = kText);
// The room stat() takes for `label` and `value`, for a row that lines its
// items up or wraps them.
ImVec2 stat_size(const char* label, const std::string& value);
// A band across the page behind a row of actions and stats, with a hairline
// along its top and room above and below. What is between begin and end is
// laid out as usual (SameLine between items).
void action_bar_begin(const char* id);
// Before each item in an action bar after the first, `size` the item's: puts
// it `gap` real pixels after the one before, centred on the line's height (the
// first item's, so a play button's), or at the start of a new line when it
// would not fit; `right` puts it against the right margin while it stays on
// the line. Returns whether it stayed on the line.
bool action_bar_next(ImVec2 size, float gap, bool right = false);
// The ghost buttons at the right end of an action bar, `n` of `labels`: as
// one group against the right margin while it fits beside what is before it,
// and one after another from the left of a new line when it does not. Each is
// ghost_button(label). Returns the index of the one pressed, or -1.
int action_bar_ghosts(const char* const* labels, int n);
void action_bar_end();

// The rest of the page below its header as one region that scrolls, running
// from one side of the window to the other, for a page of bands (a hero, an
// action bar) that reach both edges. The page's side margins are kept inside
// it, so what is laid out in it lines up with the header, and its scrollbar is
// at the window's edge. Its child window is `id`. Pair with end_page_scroll
// whatever it returns; they do not nest.
bool begin_page_scroll(const char* id);
void end_page_scroll();

// ---- navigation ----------------------------------------------------------------

// One place in a sidebar: its label (the ID too), the host's letter for it
// (drawn as a keycap, or none), a glyph from the text font ("▸", "◆", "+"),
// whether it is where the page is, and a count to show beside it (none when
// negative). `heading` starts a new group of rows with a rule and, beside the
// rail, that caption over it. `opens_here` makes the row the page's default
// focus (default_focus), for a page with nothing better to open on.
struct NavEntry {
  const char* label = "";
  const char* key = nullptr;
  const char* glyph = nullptr;
  bool current = false;
  int badge = -1;
  const char* heading = nullptr;
  bool opens_here = false;
};
// A column of places down the left of a page, `width` real pixels wide (0 is
// the breakpoint's, sidebar_width) and `height` tall (0 is the rest of the
// page): rows px(40) tall, the current one raised with an accent bar at its
// left; Up and Down go round it. On compact it is a rail of glyphs and keys,
// the label in a tooltip. Returns the index activated this frame, or -1. Its
// child window is `id`.
int nav_sidebar(const char* id, const NavEntry* entries, int n, float width = 0, float height = 0);

// ---- collections ---------------------------------------------------------------

// A tile, everything tile() (widgets.h) takes, and a few things more:
// `rest_bright` darkens its picture at rest (0.82 for a console's library
// row), brightening to full as it takes the focus. Every tile grows a little
// with the focus (`grow`, 3.5% unless said) and under the pointer (1.5%);
// inside grid_begin or a carousel the grown tile is drawn over its
// neighbours. With no `caption` a tile with a picture is only its picture,
// for a row whose page names the focused game elsewhere. A picture of
// another shape than the tile is cropped about its middle, never stretched.
struct TileSpec {
  std::string id, name, sub;
  const Texture* art = nullptr;
  ImFont* big = nullptr;
  float w = 0, h = 0;
  uint8_t dim = 0;
  bool* focused = nullptr;
  float rest_bright = 1.0f;
  float grow = 0.035f;
  bool caption = true;
};
bool tile_ex(const TileSpec& spec);

// Around a page's own grid of tiles (laid out with tile_grid and SameLine as
// before): the tile with the focus is drawn over its neighbours, and Right at
// the end of a row goes on to the next. Pairs; they do not nest.
void grid_begin();
void grid_end();
// For a widget that grows the way tiles do: draws on the top layer of the
// grid around it (true), and back (lower_in_grid) when done. False outside a
// grid, where it draws in place.
bool raise_in_grid(ImDrawList* dl);
void lower_in_grid(ImDrawList* dl);

// A row of tiles that runs off the right and scrolls sideways, smoothly, as
// the focus moves along it: the focused tile is brought to the row's left
// edge until the row's end is in view. `item_h` is a tile's height in real
// pixels; the row is a little taller, for a grown tile and its glow, and
// taller still for tiles that grow more than usual (`grow`, as TileSpec
// has it). Its child window is `id`. Pair with carousel_end whatever it
// returns.
bool carousel_begin(const char* id, float item_h, float grow = 0.035f);
// The next tile in the row. True on the frame it is activated.
bool carousel_item(const TileSpec& spec);
void carousel_end();

// ---- flows ---------------------------------------------------------------------

// How a step of a flow stands, and a count to show beside it (warnings).
enum class StepState { Todo,
                       Done,
                       Warn,
                       Fail };
struct StepInfo {
  StepState state = StepState::Todo;
  int count = 0;
};
// The steps of a flow across the page, numbered nodes joined by rules: done
// in the accent with a tick, the current one amber, those to come dim, a
// warning's count in brackets. A step `can_go` allows is a button (reached by
// the keys like any other); returns the index pressed this frame, or -1. On
// compact, when the steps do not fit the width, or when `collapsed` asks (a
// step that wants the room for itself), it is one line, "3/9 · what" and a
// progress bar, and nothing on it is pressed.
int stepper(const char* id, const char* const* labels, int n, int current, const std::function<StepInfo(int)>& info,
            const std::function<bool(int)>& can_go, bool collapsed = false);

// A step's own title at the head of its content, as a page's title is drawn
// at the head of the page: the prompt block, the title in the big type (cut
// short when it is too long for the line) and the cursor after it. For a flow
// whose page header names the flow rather than the step.
void step_heading(const std::string& title);

// The foot of a step: Back at the left (a ghost button), the step's own
// action at the right as a solid block with a play glyph after it, a
// secondary action beside it, and when the action cannot be taken yet, why,
// to its left. Labels are the ImGui labels and IDs; a null one is not drawn.
// An action that cannot be taken is disabled, as a disabled button is: the
// keys pass it by. `note` is a word about the step said in the same place
// while the action can be taken ("saved"). `primary_default` makes the action
// the page's default focus (default_focus) while it can be taken. The action
// is laid out before Back, so on a step with nothing of its own to press, the
// first item the page opens on (default_focus_next) is the way on rather than
// the way back.
struct FlowFooter {
  const char* back = nullptr;
  const char* primary = nullptr;
  bool primary_enabled = true;
  const char* reason = nullptr;
  const char* secondary = nullptr;
  const char* note = nullptr;
  bool primary_default = false;
};
enum class FlowAction { None,
                        Back,
                        Primary,
                        Secondary };
// Pinned to the foot of the window's content: a step's body goes above it in
// a region flow_footer_height() short of the page's height.
FlowAction flow_footer(const FlowFooter& f);
float flow_footer_height();

// ---- cards ---------------------------------------------------------------------

// A panel in the page's flow for one section of it: the panels' colour, a
// hairline frame, room inside, and `title` as its section() heading when
// given. As tall as what it holds. Pairs; they may nest.
void card_begin(const char* id, const char* title = nullptr);
void card_end();

// A card alone in the middle of the page, `design_w` design pixels wide (or
// the page's width, when that is less), for a screen that is only a message
// and what to do about it: centred across the page and, by its height the
// frame before, down it. Pairs; they do not nest.
void centred_card_begin(const char* id, float design_w, const char* title = nullptr);
void centred_card_end();

// A snapshot of a game's saves, as a card in a column of them: a picture at
// its left (the game as it was left, or the game's tint with the snapshot's
// number), "snapshot 0004 · 3 days ago", what the session played and wrote,
// its note, and a Restore button at its right. The cards hang on a rail down
// their left, a node for each, the newest lit. The fields the journal does
// not know are left empty and not said.
struct SnapshotCard {
  std::string gen, when, played, files, note;
  // The game's id, for the tint when there is no picture.
  std::string id;
  const Texture* art = nullptr;
  // The top of the column (the newest, lit) and its foot: the rail runs from
  // the first card's node to the last one's.
  bool first = false, last = false;
  // Real pixels; 0 is the rest of the line.
  float width = 0;
};
// One card, as tall as it needs and at least px(96). `button` is the ImGui
// label and ID of its ghost button; push an ID per card around it, as a
// column of buttons of one label needs. True on the frame it is pressed.
bool snapshot_card(const SnapshotCard& card, const char* button = "Restore");

// ---- settings ------------------------------------------------------------------

// One row of a settings page, as a console's settings list them: the
// setting's name at the left in `colour`, `help` small and dim under it when
// given, and its control at the right in a column px(360) wide (on compact,
// under the name and as wide as the row). Between begin and end go the
// control and anything that goes with it, laid out down that column; the next
// item's width is set to the column's. At least px(52) tall, a hairline under
// it. Pairs; they do not nest.
void setting_begin(const char* name, const char* help = nullptr, const ImVec4& colour = kText);
void setting_end();
// ImGui::Combo(label, current, items, n), and one more way to change it: while
// it has the keyboard or pad focus and the focus is shown, Left and Right
// step to the choice before or after, as picking that one from its list
// does, without opening it. True on the frame the choice changed, either way.
bool step_combo(const char* label, int* current, const char* const* items, int n);

// Under a SliderInt over 0..`top` just drawn, the name of each step (`names`,
// `top` + 1 of them) centred under the part of the slider it stands for, the
// `chosen` one in the accent: an integer slider's grab is one step wide, so
// the steps are equal cells and the slider reads as the row of choices it is
// rather than as a progress bar.
void step_labels(const char* const* names, int top, int chosen);

// ---- misc ----------------------------------------------------------------------

// One of the pad's buttons drawn as the hint bar draws it ("A", "B", "X",
// "Y", "LB", "RB", "LB RB", "Start", "dpad"), as an item on the line of text,
// for a page that names a button in its text.
void pad_glyph(const char* button);

}  // namespace kg::gui
