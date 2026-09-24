// The log modal a Job runs under on kretro's pages: its title, its log, and a
// Close button once it is over. The shelf and the wizard share its popup ID,
// "busy", because only one of them runs a job at a time.
//
// A player's launcher has its own, smaller "working" modal and does not use
// this one.
#pragma once

#include <functional>

#include "imgui.h"
#include "job.h"

namespace kg::gui {

enum class JobModalAction {
  None,
  Close,
};

// What differs between the pages that raise the modal.
struct JobModalHooks {
  // Drawn under the log while the job runs: "working... stop", or the
  // wizard's question about abandoning the install.
  std::function<void()> footer_while_running;
  // Called on every frame the modal shows a finished job, before its Close
  // button is drawn.
  std::function<void()> on_finished_frame;
  // Called when Close is pressed, inside the popup and before it is closed.
  std::function<void()> on_close;
};

// Opens and draws the "busy" popup for `job`. Close when its Close button was
// pressed this frame; on_close has run by then.
JobModalAction draw_job_modal(Job& job, ImFont* big, const JobModalHooks& hooks);

}  // namespace kg::gui
