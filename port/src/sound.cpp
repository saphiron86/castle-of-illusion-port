#include "sound.h"

#include <algorithm>
#include <cmath>

#include "bus.h"

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

// ---------------------------------------------------------------------------
// Z80, YM2612 and mixing
// ---------------------------------------------------------------------------

Sound::Sound(Bus& bus) : bus(bus), ym(*this)
{
    z80_cycles_per_sample = kYmClocksPerSample * kZ80Clock / kYmClock;
    ym.reset();
    set_reset(true);
}

void Sound::power_on()
{
    set_reset(true);
    ym.reset();
    psg = Psg();
    bank = 0;
    bus_requested = false;
    timer_clocks[0] = timer_clocks[1] = -1;
    z80_budget = 0;
    dc_in[0] = dc_in[1] = dc_out[0] = dc_out[1] = 0;
}

void Sound::set_reset(bool held)
{
    if (held && !reset_held) {
        // nothing to do until the line is released
    } else if (!held && reset_held) {
        z80_init(&cpu);
        cpu.read_byte = z80_read;
        cpu.write_byte = z80_write;
        cpu.port_in = z80_in;
        cpu.port_out = z80_out;
        cpu.userdata = this;
        ym.reset();                                 // the Z80 reset line also resets the YM2612
        timer_clocks[0] = timer_clocks[1] = -1;
    }
    reset_held = held;
}

void Sound::ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks)
{
    if (tnum < 2)
        timer_clocks[tnum] = duration_in_clocks;
}

void Sound::run(double z80_cycles, std::vector<int16_t>& out)
{
    const double psg_steps_per_sample = (kZ80Clock / 16.0) / sample_rate();
    z80_budget += z80_cycles;
    while (z80_budget >= z80_cycles_per_sample) {
        z80_budget -= z80_cycles_per_sample;

        // Run the Z80 for one sample's worth of cycles, unless it is held.
        if (!reset_held && !bus_requested) {
            // cyc is 32 bits on Windows and wraps after about 20 minutes, so compare the
            // difference rather than the values, or the Z80 stops for good at the wrap.
            unsigned long target = cpu.cyc + (unsigned long)z80_cycles_per_sample;
            while ((long)(cpu.cyc - target) < 0)
                z80_step(&cpu);
        }

        // Advance the YM2612 timers by one sample (144 clocks).
        for (int t = 0; t < 2; t++) {
            if (timer_clocks[t] < 0)
                continue;
            timer_clocks[t] -= kYmClocksPerSample;
            if (timer_clocks[t] <= 0) {
                int32_t overshoot = timer_clocks[t];
                timer_clocks[t] = -1;
                timer_expirations[t]++;
                m_engine->engine_timer_expired(t);  // sets the status flag and re-arms via ymfm_set_timer
                if (timer_clocks[t] > 0)
                    timer_clocks[t] += overshoot;
            }
        }

        ymfm::ym2612::output_data ym_out;
        ym.generate(&ym_out);
        int32_t psg_out = psg.sample(psg_steps_per_sample);
        for (int c = 0; c < 2; c++) {
            // First-order high-pass (about 5 Hz) removes the constant offset of the chips' outputs.
            double in = ym_out.data[c] + psg_out;
            dc_out[c] = in - dc_in[c] + 0.9994 * dc_out[c];
            dc_in[c] = in;
            out.push_back((int16_t)std::clamp((int)dc_out[c], -32768, 32767));
        }
    }
}

uint8_t Sound::z80_read(void* user, uint16_t address)
{
    Sound& s = *static_cast<Sound*>(user);
    if (address < 0x4000)
        return s.bus.z80_ram[address & 0x1FFF];
    if (address < 0x6000)
        return s.ym.read(address & 3);
    if (address < 0x8000)
        return 0xFF;
    return s.bus.read8((uint32_t)s.bank << 15 | (address & 0x7FFF));
}

void Sound::z80_write(void* user, uint16_t address, uint8_t value)
{
    Sound& s = *static_cast<Sound*>(user);
    if (address < 0x4000)
        s.bus.z80_ram[address & 0x1FFF] = value;
    else if (address < 0x6000)
        s.ym.write(address & 3, value);
    else if (address < 0x6100)
        s.bank = ((s.bank >> 1) | ((value & 1) << 8)) & 0x1FF;   // one bit per write, LSB first
    else if (address == 0x7F11)
        s.psg.write(value);
    else if (address >= 0x8000)
        s.bus.write8((uint32_t)s.bank << 15 | (address & 0x7FFF), value);
}
