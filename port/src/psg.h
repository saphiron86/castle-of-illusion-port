// The SN76489 PSG (used by sound.cpp).
#pragma once

#include <cstdint>

class StateWriter;
class StateReader;

// Texas Instruments SN76489 as found in the Genesis VDP.
class Psg {
public:
    static constexpr int kHistory = 64;     // ticks kept for sample_clean's filter

    void write(uint8_t value);
    // Advance by `steps` clock/16 ticks and return one output sample: the ticks' average
    // (as the port has always done), or with Clean PSG a band-limited filter's output.
    int16_t sample(double steps);
    int16_t sample_clean(double steps);

    // Save states (savestate.h).
    void save_state(StateWriter& w) const;
    void load_state(StateReader& r);

private:
    int32_t tick();                         // one clock/16 tick; returns the four channels' sum

    uint16_t tone[3] = {};
    uint8_t volume[4] = {0x0F, 0x0F, 0x0F, 0x0F};
    uint8_t noise = 0;
    uint8_t latch = 0;
    int counter[4] = {};
    bool output[4] = {};
    uint16_t lfsr = 0x8000;
    double phase = 0;
    float history[kHistory] = {};           // the last ticks' outputs, oldest at `head`
    int head = 0;
};
