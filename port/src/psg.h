// The SN76489 PSG (used by sound.cpp).
#pragma once

#include <cstdint>

// Texas Instruments SN76489 as found in the Genesis VDP.
class Psg {
public:
    void write(uint8_t value);
    int16_t sample(double steps);   // advance by `steps` clock/16 ticks, return the averaged output

private:
    uint16_t tone[3] = {};
    uint8_t volume[4] = {0x0F, 0x0F, 0x0F, 0x0F};
    uint8_t noise = 0;
    uint8_t latch = 0;
    int counter[4] = {};
    bool output[4] = {};
    uint16_t lfsr = 0x8000;
    double phase = 0;
};
