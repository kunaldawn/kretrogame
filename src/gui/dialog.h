// Dialogs: every modal question kretro and a player ask, drawn one way.
//
// A dialog is ImGui's modal popup under the popup ID the page already uses,
// centred over the dimmed page with a soft shadow, fading in, headed by its
// question in large type, and closed by a band along its foot that holds its
// buttons against the right edge. One button is the safe answer: the focus
// starts on it, and Escape and the pad's B press it. Nothing about what a
// dialog does changes here; the page still opens it, answers each button and
// closes it.
#pragma once

#include <string>

#include "imgui.h"

namespace kg::gui {

// ImGui::BeginPopupModal(popup_id) with the dialog's look, `design_w` design
// pixels wide and as tall as what it holds, headed by `title` (none when
// empty). True while it is open; then everything up to dialog_end is its
// contents. The popup is opened by the page, as before, with OpenPopup.
bool dialog_begin(const char* popup_id, float design_w, const std::string& title = std::string());

// The same, `w` real pixels wide and at most `max_h` tall, for a dialog that
// sizes itself from the window rather than in design pixels (the job modal).
bool dialog_begin_px(const char* popup_id, float w, float max_h, const std::string& title = std::string());

// Starts the band at the dialog's foot. Whatever follows up to dialog_end is
// in the band: dialog_buttons, and any line of its own a dialog puts there (a
// running job's "working... stop").
void dialog_footer();

// What a dialog button says about itself: the answer the dialog is for (in
// the accent, "[ Restore ]"), an ordinary one, or one that throws something
// away (in the error colour, so the green always means "go ahead").
enum class DialogButton { Primary,
                          Secondary,
                          Danger };
// A button in the band. The buttons on a line sit together against the right
// edge in the order they are drawn. `label` is the ImGui label and so the ID,
// unchanged. `safe` marks the answer that loses nothing: the focus starts on
// it when the dialog opens, and it is also pressed by Escape and the pad's B
// (modal_cancelled, focus.h). True on the frame it is pressed. It is laid out
// once the dialog has been drawn a frame, which ImGui's own first, hidden
// frame of a dialog covers.
bool dialog_button(const char* label, DialogButton kind = DialogButton::Secondary, bool safe = false);

// Ends what dialog_begin began, when it returned true.
void dialog_end();

}  // namespace kg::gui
