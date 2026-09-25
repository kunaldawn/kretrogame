// Motion: the few ways anything on screen moves, all of them short, all of them
// the same speed whatever the frame rate, and all of them off when the person
// asks for no motion at all.
//
// Nothing here animates by itself. A widget asks, each frame it is drawn, for
// the value it should be drawn at now, and the answer eases towards where it
// is going. A step is worked out from the frame's own time, held to 50 ms so
// a frame that stalled does not jump, so a 30 fps software renderer and a
// 144 Hz screen see the same motion in the same time.
#pragma once

#include "imgui.h"

namespace kg::gui {

// ---- turning motion off -----------------------------------------------------

// KRETRO_REDUCE_MOTION=1 (read once): every value snaps to where it is going,
// nothing glides, fades or breathes. For a person motion bothers, and for
// tools that take screenshots and want the frame they asked for.
bool reduce_motion();
// Sets it for the rest of the process, for tests and tools; it overrides the
// environment.
void set_reduce_motion(bool on);

// The time one step of motion covers this frame: ImGui's frame time, never more
// than 50 ms, and 0 with no ImGui frame.
float anim_dt();

// ---- the arithmetic, which needs no window ----------------------------------

// Eases in and out, 0..1 to 0..1.
float ease_out_cubic(float t);
float ease_in_out_cubic(float t);
// One step of an exponential approach from `current` to `target`: after `tau`
// seconds it has covered 63% of the way, after three `tau` 95%. It lands
// exactly on the target once it is within `snap` of it, so a value at rest is
// its target and not a hair off it.
float approach(float current, float target, float dt, float tau, float snap = 0.001f);
// A critically damped spring, for a value that should arrive without
// overshooting even when its target keeps moving: `velocity` is kept by the
// caller between steps. `tau` is roughly the time to cover two thirds of the
// way.
float spring(float current, float target, float& velocity, float dt, float tau);

// ---- values kept for a widget ------------------------------------------------

// Each of these keeps its state in the current window's storage under `key`,
// so a widget that stops being drawn forgets it with its window. Ask once per
// frame per key: each call takes a step.

// The value eased towards `target`; the first time it is asked for, it starts
// at the target, so nothing slides in from zero.
float anim_to(ImGuiID key, float target, float tau = 0.045f);
// 0..1, rising over `dur` seconds while `on` and falling back while not,
// eased out as it is read. Starts at where `on` says.
float anim01(ImGuiID key, bool on, float dur = 0.12f);
// 0..1 over `dur` seconds from the frame `restart` is true (or the first
// frame the key is asked for), eased out: a fade in.
float fade_in(ImGuiID key, bool restart, float dur = 0.12f);

}  // namespace kg::gui
