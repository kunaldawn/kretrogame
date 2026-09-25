// How wide things are: the window's size class, the widest a column of each
// kind of content reads well at, and a column centred in the page.
//
// A page is laid out for the room it has in design pixels (the window's width
// over the UI scale), so a zoomed-in window and a small one behave alike: a
// 1024x600 window at 80% has the room of a 1280-wide one, a 1280x800 window at
// 150% the room of an 853-wide one.
#pragma once

#include "imgui.h"

namespace kg::gui {

// Compact below 1100 design pixels (a small window, or a zoomed one): one
// column, icon rails, stacked buttons. Regular up to 2200. Wide beyond it (an
// ultrawide screen, or a scale forced low): columns stop growing and centre.
enum class Breakpoint { Compact,
                        Regular,
                        Wide };
// For a width in design pixels, with no window: what the tests check.
Breakpoint breakpoint_for(float design_w);
// For the window as it is now: its work area's width over ui_scale(). Regular
// with no ImGui frame.
Breakpoint breakpoint();

// The kinds of column a page lays out.
enum class Content { Reading,  // prose and reports: doctor, licences, about
                     Form,     // settings, wizard steps, bundle steps
                     Grid,     // the shelf's and the launcher's tiles
                     Dialog };
// The widest a column of `c` should be, in real pixels, given `avail` real
// pixels of room at scale `scale` on breakpoint `bp`. Never more than avail.
float content_max_w_for(Content c, Breakpoint bp, float avail, float scale);
// The same for the current window's content region at the current scale.
float content_max_w(Content c);

// The heights and widths the Steam-like pieces take on each breakpoint, in
// real pixels: a hero band, and a navigation sidebar (an icon rail on
// compact). On compact the band is also held to under a third of the
// window's height, for a window that is short as well as narrow.
float hero_height();
// The same for a window `window_h` real pixels tall at scale `scale`, with no
// window: what the tests check.
float hero_height_for(Breakpoint bp, float window_h, float scale);
float sidebar_width();

// A column no wider than `max_w` real pixels, centred in the room left on the
// line: what is drawn up to end_centre_column() is laid out, wrapped and
// right-aligned within it. When the room is narrower, the column is the room.
// Every begin needs its end; they do not nest.
void centre_column(float max_w);
void end_centre_column();

}  // namespace kg::gui
