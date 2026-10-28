// Smooth Fades (see fade.h).
//
// The game's fades (sub_058E48 and its relatives in code_58.asm) move each red, green and
// blue level of the palette buffer one step towards black or towards the target palette at
// $FFF800, a channel every few frames, and VInt copies the buffer to CRAM. A frame counts as
// part of a fade when at least three colours moved by one level, all the same way; from then
// on, one-level steps that way are eased, each over the time since that channel's previous
// step (about 4-5 frames in the game's fades).

#include "fade.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kVotes = 3;           // colours that must step together to start a fade
constexpr int kFadeGap = 16;        // frames without fade steps that end a fade
constexpr int kMaxLength = 10;      // longest ease, in frames

int level(uint16_t cram, int channel) { return (cram >> (1 + channel * 4)) & 7; }

}  // namespace

void FadeSmoother::reset()
{
    primed = false;
    direction = 0;
    fade_frame = -1000;
}

float FadeSmoother::shown(const Channel& c, double at) const
{
    if (c.length <= 0 || at >= c.start + c.length)
        return c.to;
    const double t = std::max(0.0, (at - c.start) / c.length);
    return (float)(c.from + (c.to - c.from) * t);
}

void FadeSmoother::update(const uint16_t* cram)
{
    frame++;
    if (!primed) {
        for (int i = 0; i < 64; i++) {
            source[i] = cram[i];
            for (int ch = 0; ch < 3; ch++) {
                Channel& c = channels[i][ch];
                c.level = (uint8_t)level(cram[i], ch);
                c.from = c.to = c.level;
                c.length = 0;
                c.last_step = -1000;
            }
        }
        primed = true;
        return;
    }

    // Does this frame look like a fade step?
    int up = 0, down = 0;
    for (int i = 0; i < 64; i++) {
        if (cram[i] == source[i])
            continue;
        int sign = 0;
        bool single = true;
        for (int ch = 0; ch < 3; ch++) {
            const int d = level(cram[i], ch) - channels[i][ch].level;
            if (d == 0)
                continue;
            if (d < -1 || d > 1 || (sign && d != sign))
                single = false;
            sign = d;
        }
        if (single)
            (sign > 0 ? up : down)++;
    }
    if (up >= kVotes && !down) {
        direction = 1;
        fade_frame = frame;
    } else if (down >= kVotes && !up) {
        direction = -1;
        fade_frame = frame;
    }
    const bool fading = direction && frame - fade_frame <= kFadeGap;

    for (int i = 0; i < 64; i++) {
        if (cram[i] == source[i])
            continue;
        source[i] = cram[i];
        for (int ch = 0; ch < 3; ch++) {
            Channel& c = channels[i][ch];
            const int now = level(cram[i], ch);
            const int d = now - c.level;
            if (d == 0)
                continue;
            c.level = (uint8_t)now;
            if (fading && d == direction) {
                const int since = frame - c.last_step;
                if (since <= kMaxLength + 2)
                    interval = std::clamp(since, 2, kMaxLength);
                c.from = shown(c, frame);
                c.to = (float)now;
                c.start = frame - 1;
                c.length = interval;
                c.last_step = frame;
            } else {
                c.from = c.to = (float)now;
                c.length = 0;
            }
        }
    }
}

void FadeSmoother::fill(PaletteOverride& out, double back) const
{
    const double at = frame - back;
    for (int i = 0; i < 64; i++) {
        out.source[i] = source[i];
        uint32_t rgb = 0xFF000000;
        for (int ch = 0; ch < 3; ch++) {
            const int v = (int)std::lround(shown(channels[i][ch], at) * 255.0 / 7.0);
            rgb |= (uint32_t)std::clamp(v, 0, 255) << (16 - ch * 8);
        }
        out.rgb[i] = rgb;
    }
}
