// What kretro's shelf and a player's launcher draw alike: the page chrome, the
// tile a game is shown as, and the small pieces every page uses - coloured
// sentences and text fields over a std::string.
//
// Both are Dear ImGui over an SDL renderer, with the runtime's own fonts and
// one palette, so a player an author built looks like the kretro they built it
// with. Everything here is drawing; neither program's pages are. The window
// itself is window.h, and pictures are texture.h.
//
// This header brings in everything a page draws with. Beside what is declared
// below:
//   focus.h   where the keyboard and the pad are: default_focus, the focus
//             glow (focus_state, focus_glow_rect, own_focus_ring), input_mode,
//             back_allowed, modal_cancelled, text panels' keys, Refresh.
//   anim.h    motion: anim_to, anim01, fade_in, the easings, reduce_motion
//             (KRETRO_REDUCE_MOTION=1).
//   scroll.h  smooth scrolling: begin_scroll / end_scroll, edge_fades,
//             scroll_to_item.
//   layout.h  size classes: breakpoint (compact / regular / wide),
//             content_max_w, centre_column, hero_height, sidebar_width.
//   dialog.h  every modal question: dialog_begin, dialog_footer,
//             dialog_button (the safe one takes Escape and B), dialog_end.
//   blocks.h  the Steam-like structure: hero, play_button, ghost_button,
//             stat, action_bar, nav_sidebar, carousel, grid_begin, stepper,
//             flow_footer, card, pad_glyph.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>

#include "anim.h"
#include "blocks.h"
#include "dialog.h"
#include "focus.h"
#include "imgui.h"
#include "layout.h"
#include "palette.h"
#include "scale.h"
#include "scroll.h"
#include "texture.h"

namespace kg::gui {

// The page chrome every full-screen screen shares: a prompt, the title in big
// type and a blinking cursor, a rule under them, and along the bottom of the
// window a status bar with `hint` drawn as keys and what they do, and the
// current zoom. The hint is written as groups three or more spaces apart,
// each a key and then its action: "arrows move   Enter play   Esc quit".
// The page's window stops above the status bar, so nothing is drawn under it.
//
// Callers must call ImGui::End() on every path out, including the early
// returns an empty state takes.
void begin_page(const char* title, const char* hint, ImFont* big);
// Set before a page's PageWindow, for a page whose hero band says its title:
// the header is then one line of ordinary type, `trail` ("kretro › Example
// Game", the last step in the text colour and the rest dim) after the prompt
// block, rather than the title in big type. The window keeps the title as its
// name, and the status bar its path. It holds for that one frame.
void set_page_trail(const std::string& trail);
// Set before a page's PageWindow, for a page whose hero band runs up to the
// window's top edge and names the page itself: no header at all, the cursor
// left at the window's padding. The status bar is drawn as ever. It holds for
// that one frame.
void set_page_bare();

// The name the status bar shows at its left, in a chip of its own: the
// program, or the player's bundle. open_window sets it from the window title.
void set_chrome_tag(const std::string& tag);
// Where the page is, shown after the tag as a path the way a shell prompt
// shows its directory: "~/" on the page whose title is the tag itself (the
// program's home screen), otherwise "~/" and the title in lower case with
// dashes for spaces. A page that wants a different path sets it before
// begin_page; it holds for that one frame, so a page that stops setting it
// goes back to the derived one.
void set_chrome_path(const std::string& path);
// The height of the status bar begin_page draws, at the current scale.
float status_bar_height();
// Presses Escape, as the status bar's back arrow does: for a button that is
// another way to do what Escape does on the page, whoever answers it.
void press_escape();

// begin_page, with the End() it needs on every path out done by the
// destructor. A page that throws skips End, as a page calling begin_page by
// hand does: the throw is caught by the caller and ImGui's error recovery ends
// the window at EndFrame, and an End from here on the way out would end
// whatever window happens to be current then.
class PageWindow {
 public:
  PageWindow(const char* title, const char* hint, ImFont* big);
  ~PageWindow();
  PageWindow(const PageWindow&) = delete;
  PageWindow& operator=(const PageWindow&) = delete;

 private:
  int uncaught_;
};

// A colour derived from the game's own name, so a game with no picture yet
// still gets a tile that is recognisably its own rather than a grey box.
ImU32 tile_colour(const std::string& id, float mul = 1.0f);

// One game as a tile: its picture, or its colour and name in large type; a
// caption strip with the name and `sub` under it, each cut short with an
// ellipsis; an amber frame and corner brackets when it has the keyboard or pad
// focus and the focus is shown, a quieter green frame under the mouse; and
// darkened by `dim`, an alpha. The whole tile is one focusable button, which
// is what the arrows and a gamepad move between. True on the frame it is
// activated. `focused`, when given, is set to whether it has the keyboard or
// pad focus.
bool tile(const std::string& id, const std::string& name, const std::string& sub,
          const Texture* art, ImFont* big, float w, float h, uint8_t dim = 0,
          bool* focused = nullptr);

// tile_ex, the same tile from a TileSpec with its picture dimmed at rest, is
// in blocks.h with the grids and rows it goes in.

// A picture stretched over the rectangle from `a` to `b`, the part of it from
// `uv0` to `uv1`. Use it, or image(), in place of ImDrawList::AddImage and
// ImGui::Image for anything that may be drawn large: it stays whole on the
// software renderer, which breaks a big picture drawn in one piece.
void draw_image(ImDrawList* dl, const Texture& tex, ImVec2 a, ImVec2 b, ImVec2 uv0 = ImVec2(0, 0),
                ImVec2 uv1 = ImVec2(1, 1), ImU32 tint = IM_COL32_WHITE);
// The same as an item on the line, `size` pixels, as ImGui::Image lays one out.
void image(const Texture& tex, ImVec2 size, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1));

// `s` cut short with an ellipsis so that it fits `max_w` pixels in `f` (the
// current font when null), on a character boundary. Unchanged when it fits.
std::string elide(ImFont* f, const std::string& s, float max_w);

// A key drawn as a keycap - a small filled chip in the small font, `bg`
// behind dark text - as one item on the line, for a page that names a key in
// its text. keycap_size is how much room one takes.
ImVec2 keycap_size(const char* key);
void keycap(const char* key, const ImVec4& bg);

// The heading of a section inside a page, in the terminal's manner: a short
// rule, the label in the accent colour, and a rule to the right margin.
void section(const char* label);

// ImGui::Button, marked as the thing to do on the page: accent text between
// brackets, "[ Play ]", on a green-tinted fill. The label and so the ID are the
// caller's, unchanged; the brackets are drawn, not added to the text. With no
// width given it is wide enough for the brackets too.
bool primary_button(const char* label, ImVec2 size = ImVec2(0, 0));

// The next popup's position and size for a modal: centred, `design_w` wide
// (px() is applied here), and no wider or taller than the window leaves room
// for. A `design_h` of 0 lets the height follow the contents.
void next_modal(float design_w, float design_h = 0);

// ImGui::SmallButton with room round its label: a little padding above and
// below and more at the sides, so the frame's border never touches a glyph.
// The label and so the ID are the caller's, unchanged, and it still sits on
// the baseline of the text beside it.
bool small_button(const char* label);
// How wide small_button(label) comes out, for making room for one before it
// is drawn.
float small_button_width(const char* label);

// ---- layout -----------------------------------------------------------------

// Vertical room between blocks, `design_px` design pixels on whole pixels.
void vgap(float design_px);
// SameLine, when the next item, `next_w` pixels wide, still fits on the line;
// otherwise the next item starts a new line. The label form measures a button
// labelled `next`.
void same_line_if_fits(float next_w);
void same_line_if_fits(const char* next);
// After SameLine: moves along the line so that the next item, `w` pixels wide,
// ends at the window's right margin. It never moves back left, so an item that
// no longer fits sits straight after what the line already holds rather than
// over it. The position is set on the screen, so it holds inside a group too.
void align_right(float w);
// Text flush with the right of the column it is in, so a column of sizes and
// counts lines up on its last character.
void right_aligned(const std::string& text, const ImVec4& colour = kText);
// One line of text in `colour`, cut short with an ellipsis to the room left on
// the line.
void elided_text(const std::string& text, const ImVec4& colour = kText);
// The width for a field that reads best `design` design pixels wide, and
// takes what room there is when the window is narrower than that.
float field_width(float design);
// A form: the labels in a dim column as wide as the longest of `labels`, and
// each row's fields in the column beside it, which takes the rest of the
// width. Measured every frame, so the column follows the font at any scale.
// A table: EndTable only when it returned true.
bool form_begin(const char* id, std::initializer_list<const char*> labels);
// The next row of a form or of any two-column table of labels and fields, left
// in its field column. `framed` lowers the label to the text inside a field or
// button, for a row that starts with one.
void form_row(const char* label, bool framed = true);

// A bordered child window in the panels' colour, for a listing or a list to
// pick from, which the page's own children leave transparent. `size` is
// BeginChild's. `padding` is the room inside the border, in real pixels; a
// negative one keeps the style's. `border` is the outline's colour: kLine for
// a listing, the frames' kFrameLine for a list to pick from. The keyboard and
// the pad move through its items as though they were the page's own. Pair it
// with end_panel whatever it returns, as BeginChild is paired with EndChild.
bool begin_panel(const char* id, ImVec2 size, ImVec2 padding = ImVec2(-1, -1),
                 const ImVec4& border = kLine, ImGuiWindowFlags flags = 0);
// The same panel for text with nothing in it to press: a log, a report, the
// licences. It is one stop for the keyboard and the pad, and the keys scroll it
// while it has the focus (scroll_with_keys, focus.h). Paired with end_panel.
bool begin_text_panel(const char* id, ImVec2 size, ImVec2 padding = ImVec2(-1, -1),
                      const ImVec4& border = kLine, ImGuiWindowFlags flags = 0);
// A panel for a short list: as tall as `lines` rows `row_h` high, but never
// more than `max_lines` of them, past which it scrolls. Paired with end_panel.
bool begin_list_panel(const char* id, float lines, float max_lines, float row_h);
void end_panel();

// A rule across the window with a short accent segment at its left, as under
// a page's title, on whole pixels.
void title_rule();
// The head of a modal drawn without ImGui's title bar: the title in big accent
// type, wrapped when it is long, over a title_rule.
void modal_title(const std::string& title);

// ---- terminal pieces --------------------------------------------------------

// A status word in brackets, in the manner of a boot log: "[ OK ]", "[WARN]",
// "[FAIL]", "[INFO]", as one item on the line. The brackets are dim and the
// word is in the kind's colour; `text`, when given, replaces the word and is
// centred between the brackets the same way.
enum class BadgeKind { Ok,
                       Warn,
                       Fail,
                       Info };
void badge(BadgeKind kind, const char* text = nullptr);
// The colour a kind of badge is drawn in, for text that goes with one.
ImVec4 badge_colour(BadgeKind kind);
// The room between a badge and what follows it on the line.
float badge_gap();
// A badge and a sentence beside it in `colour`. ImGui lays wrapped text out as
// one block from where it starts, so the sentence's later lines hang under
// its first rather than running back under the badge.
void badge_line(BadgeKind kind, const std::string& text, const ImVec4& colour = kText);
// A badge and a group beside it, for more than one sentence or a button or
// two under the first line, all hanging under it. Paired with end_badge_block.
void begin_badge_block(BadgeKind kind, const char* text = nullptr);
void end_badge_block();
// A section() heading with a verdict at its right end, a badge (`text` in
// place of its word, when given), so a report of sections can be read down
// its right edge before any of it is read in full.
void section(const char* label, BadgeKind kind, const char* text = nullptr);

// A progress bar the way a terminal draws one: "[######....]  42%", as cells
// the width of a character, the done ones in the accent colour. `fraction` is
// 0..1; a negative one means "working, amount unknown", and a short block
// slides back and forth instead. `width` is the whole item's, the percentage
// included; 0 takes the rest of the line. `overlay`, when given, is said
// after the percentage (or in its place when the amount is unknown).
void block_progress(float fraction, float width = 0, const char* overlay = nullptr);

// A key and its value, with the keys in a dim column of their own: "size
// 412 MiB", "path /home/...". Between kv_begin and kv_end the rows are a
// table whose key column is as wide as its longest key, or `label_w` pixels
// when that is given; kv_end only when kv_begin returned true. Outside one, a
// row stands on its own and its key column is px(150) wide. The value wraps;
// `colour` is the value's.
bool kv_begin(const char* id, float label_w = 0);
void kv(const char* label, const std::string& value, const ImVec4& colour = kText);
void kv_end();

// The frame of a text spinner for now, "|", "/", "-" or "\", turning about
// eight times a second. The runtime's fonts have no braille, so it is the
// ASCII one, which every font has. spinner() draws it as a text item in the
// accent colour.
const char* spinner_glyph();
void spinner();

// What a page shows when it has nothing to list: `glyph` large and dim,
// centred in the space left (or `height` pixels of it when that is given),
// and `message` wrapped and centred under it. The glyph is drawn in the big
// font, so it must be in the title ranges: Latin-1, punctuation, blocks and
// the geometric shapes. With no glyph, it is a prompt and a blinking cursor.
void empty_state(const char* message, const char* glyph = nullptr, float height = 0);

// A wrapped sentence in one colour.
void colored_text(const ImVec4& colour, const std::string& s);
// In palette.h's kWarn and kGood.
void warn_text(const std::string& s);
void good_text(const std::string& s);

// There is no imgui_stdlib in this tree. ImGui keeps its own copy of a field
// while it is being edited, so a buffer made fresh each frame from the string
// is enough, and the string is only written when the text changed. `cap`
// counts the terminating zero, so a field holds cap - 1 bytes.
bool input_string(const char* label, std::string& s, size_t cap, ImGuiInputTextFlags flags = 0);
bool input_string_multiline(const char* label, std::string& s, size_t cap, ImVec2 size);

// Where a file browser opens: the person's home, from HOME, or "/" when HOME
// is unset or empty. Not kg::home_dir(), which is kretro's per-game HOME under
// its state directory; inside kg::gui this name hides that one, so call that
// one qualified.
std::filesystem::path home_dir();

}  // namespace kg::gui
