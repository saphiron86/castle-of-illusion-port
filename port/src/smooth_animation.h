// Smooth Animation (Debug menu, off by default; the GPU renderer only): objects the game turns
// with a few hand-drawn frames are drawn instead as one upright frame turned smoothly. Display
// only: the game's code, RAM and timing are untouched. See docs/notes/smooth-animation.md.
#pragma once

#include <cstddef>
#include <cstdint>

#include "interp.h"
#include "vdp.h"

namespace smooth_animation {

// Once a game frame, before the frame's lines are drawn: finds the table's objects among the
// owners of the sprite table the VDP shows (FrameInterpolator::shown_owners) and fills
// vdp.rotated and vdp.rotated_pictures; clears them when `on` is false.
void update(Vdp& vdp, const FrameInterpolator& interp, const uint8_t* ram, const uint8_t* rom, size_t rom_size, bool on);

// A picture between frames, `back` of a frame before the current one: the current frame's
// rotated objects, each turned back along its rate (positions move with sprite_offset).
void between(Vdp& v, const Vdp& now, double back);

}  // namespace smooth_animation
