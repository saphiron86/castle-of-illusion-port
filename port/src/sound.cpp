#include "sound.h"

#include <algorithm>
#include <cmath>

#include "bus.h"

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
