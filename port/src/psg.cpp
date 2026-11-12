#include "psg.h"

#include <algorithm>

// ---------------------------------------------------------------------------
// SN76489 PSG
// ---------------------------------------------------------------------------

void Psg::write(uint8_t value)
{
    if (value & 0x80)
        latch = (value >> 4) & 7;
    int channel = latch >> 1;
    bool is_volume = latch & 1;
    if (is_volume) {
        volume[channel] = value & 0x0F;
    } else if (channel < 3) {
        if (value & 0x80)
            tone[channel] = (tone[channel] & 0x3F0) | (value & 0x0F);
        else
            tone[channel] = (tone[channel] & 0x00F) | ((value & 0x3F) << 4);
    } else {
        noise = value & 0x07;
        lfsr = 0x8000;
    }
}

int16_t Psg::sample(double steps)
{
    // Attenuation in 2 dB steps; 15 is silent.
    static const int16_t kLevels[16] = {2000, 1589, 1262, 1002, 796, 632, 502, 399,
                                        317, 252, 200, 159, 126, 100, 79, 0};
    phase += steps;
    int ticks = (int)phase;
    phase -= ticks;
    int32_t sum = 0;
    for (int t = 0; t < std::max(ticks, 1); t++) {
        for (int ch = 0; ch < 3; ch++) {
            if (--counter[ch] <= 0) {
                counter[ch] = std::max<int>(tone[ch], 1);
                output[ch] = !output[ch];
            }
        }
        if (--counter[3] <= 0) {
            static const int kNoisePeriods[3] = {0x10, 0x20, 0x40};
            int period = (noise & 3) == 3 ? std::max<int>(tone[2], 1) : kNoisePeriods[noise & 3];
            counter[3] = period;
            output[3] = !output[3];
            if (output[3]) {                        // shift on the rising edge
                bool white = noise & 4;
                uint16_t feedback = white ? ((lfsr ^ (lfsr >> 3)) & 1) : (lfsr & 1);
                lfsr = (lfsr >> 1) | (feedback << 15);
            }
        }
        for (int ch = 0; ch < 3; ch++)
            sum += output[ch] ? kLevels[volume[ch]] : -kLevels[volume[ch]];
        sum += (lfsr & 1) ? kLevels[volume[3]] : -kLevels[volume[3]];
    }
    return (int16_t)(sum / std::max(ticks, 1));
}
