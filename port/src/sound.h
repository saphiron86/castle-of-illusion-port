// Sound: the Z80 running the game's sound driver, the YM2612 (ymfm) and the
// SN76489 PSG. See docs/sound.md for how the game and the driver talk.
#pragma once

#include <cstdint>
#include <vector>

#include "ymfm_opn.h"

extern "C" {
#include "z80.h"
}

class Bus;

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

class Sound : public ymfm::ymfm_interface {
public:
    static constexpr double kZ80Clock = 3579545.0;
    static constexpr double kYmClock = 7670453.0;
    static constexpr int kYmClocksPerSample = 144;

    explicit Sound(Bus& bus);

    void power_on();                // Z80 held in reset, chips cleared

    int sample_rate() const { return (int)(kYmClock / kYmClocksPerSample); }

    // Advance by `z80_cycles` of time, appending stereo samples to `out`.
    void run(double z80_cycles, std::vector<int16_t>& out);

    // Z80 control lines, driven by the 68000 through $A11100/$A11200.
    void set_reset(bool held);
    void set_bus_requested(bool requested) { bus_requested = requested; }
    void raise_irq() { if (!reset_held) z80_gen_int(&cpu, 0xFF); }
    void clear_irq() { cpu.int_pending = false; }

    void psg_write(uint8_t value) { psg.write(value); }

    // Statistics for testing: how often each YM2612 timer has expired.
    unsigned timer_expirations[2] = {};

private:
    // ymfm_interface
    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override;

    static uint8_t z80_read(void* user, uint16_t address);
    static void z80_write(void* user, uint16_t address, uint8_t value);
    static uint8_t z80_in(z80*, uint8_t) { return 0xFF; }
    static void z80_out(z80*, uint8_t, uint8_t) {}

    Bus& bus;
    z80 cpu{};
    ymfm::ym2612 ym;
    Psg psg;
    uint16_t bank = 0;              // 9-bit bank register: Z80 $8000-$FFFF = 68000 (bank << 15)
    bool reset_held = true;
    bool bus_requested = false;
    int32_t timer_clocks[2] = {-1, -1};   // YM2612 clocks until each timer expires; -1 = stopped
    double z80_budget = 0;          // Z80 cycles owed to the current sample
    double z80_cycles_per_sample;
    double dc_in[2] = {}, dc_out[2] = {};   // DC-blocking filter state, like the console's output capacitors
};
