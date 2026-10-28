// Smooth Fades: the game fades the screen in and out by stepping each colour channel one of
// the VDP's eight levels at a time, every few frames, so a fade shows as a handful of jumps.
// FadeSmoother spots those steps in CRAM and eases the colours shown from one level to the
// next over the time the game takes between steps (they show up to one step late). Any
// other palette change (a new palette, a flash) is shown at once.
#pragma once

#include <cstdint>

struct PaletteOverride {
    uint16_t source[64];        // the CRAM value each colour stands in for
    uint32_t rgb[64];           // 0xAARRGGBB
};

class FadeSmoother {
public:
    // Call once per game frame, with CRAM as the frame shows it.
    void update(const uint16_t* cram);
    // The colours to show `back` game frames (0-1) before the current one.
    void fill(PaletteOverride& out, double back = 0.0) const;
    void reset();

private:
    struct Channel {
        uint8_t level = 0;      // the game's level, 0-7
        float from = 0, to = 0; // shown level: eases from `from` to `to`...
        int start = 0, length = 0;  // ...over `length` frames from frame `start`
        int last_step = -1000;  // frame of this channel's last fade step
    };
    float shown(const Channel& c, double frame) const;

    Channel channels[64][3];
    uint16_t source[64] = {};
    int frame = 0;
    int direction = 0;          // the fade under way: +1 in, -1 out, 0 none
    int fade_frame = -1000;     // last frame that looked like a fade
    int interval = 4;           // frames between a channel's steps, last measured
    bool primed = false;
};
