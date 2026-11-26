#include "sound.h"

#include <algorithm>
#include <cmath>

#include "bus.h"
#include "state_io.h"

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
    lp[0] = lp[1] = 0;
}

void Sound::save_state(StateWriter& w)
{
    w.put<uint64_t>(cpu.cyc);
    for (uint16_t v : {cpu.pc, cpu.sp, cpu.ix, cpu.iy, cpu.mem_ptr})
        w.put(v);
    for (uint8_t v : {cpu.a, cpu.b, cpu.c, cpu.d, cpu.e, cpu.h, cpu.l, cpu.a_, cpu.b_, cpu.c_, cpu.d_, cpu.e_, cpu.h_,
                      cpu.l_, cpu.f_, cpu.i, cpu.r})
        w.put(v);
    for (bool v : {(bool)cpu.sf, (bool)cpu.zf, (bool)cpu.yf, (bool)cpu.hf, (bool)cpu.xf, (bool)cpu.pf, (bool)cpu.nf,
                   (bool)cpu.cf})
        w.put(v);
    w.put(cpu.iff_delay);
    w.put(cpu.interrupt_mode);
    w.put(cpu.int_data);
    for (bool v : {(bool)cpu.iff1, (bool)cpu.iff2, (bool)cpu.halted, (bool)cpu.int_pending, (bool)cpu.nmi_pending})
        w.put(v);
    std::vector<uint8_t> chip;
    ymfm::ymfm_saved_state saved(chip, true);
    ym.save_restore(saved);
    w.put<uint32_t>((uint32_t)chip.size());
    w.bytes(chip.data(), chip.size());
    psg.save_state(w);
    w.put(bank);
    w.put(reset_held);
    w.put(bus_requested);
    w.put_array(timer_clocks);
    w.put(z80_budget);
    w.put_array(dc_in);
    w.put_array(dc_out);
    w.put_array(lp);
}

bool Sound::load_state(StateReader& r)
{
    // (In a game that never released the Z80 from reset, it has no memory to run on yet.)
    cpu.read_byte = z80_read;
    cpu.write_byte = z80_write;
    cpu.port_in = z80_in;
    cpu.port_out = z80_out;
    cpu.userdata = this;
    cpu.cyc = (unsigned long)r.get<uint64_t>();
    for (uint16_t* v : {&cpu.pc, &cpu.sp, &cpu.ix, &cpu.iy, &cpu.mem_ptr})
        r.get(*v);
    for (uint8_t* v : {&cpu.a, &cpu.b, &cpu.c, &cpu.d, &cpu.e, &cpu.h, &cpu.l, &cpu.a_, &cpu.b_, &cpu.c_, &cpu.d_, &cpu.e_,
                       &cpu.h_, &cpu.l_, &cpu.f_, &cpu.i, &cpu.r})
        r.get(*v);
    cpu.sf = r.get<bool>(), cpu.zf = r.get<bool>(), cpu.yf = r.get<bool>(), cpu.hf = r.get<bool>();
    cpu.xf = r.get<bool>(), cpu.pf = r.get<bool>(), cpu.nf = r.get<bool>(), cpu.cf = r.get<bool>();
    r.get(cpu.iff_delay);
    r.get(cpu.interrupt_mode);
    r.get(cpu.int_data);
    cpu.iff1 = r.get<bool>(), cpu.iff2 = r.get<bool>(), cpu.halted = r.get<bool>();
    cpu.int_pending = r.get<bool>(), cpu.nmi_pending = r.get<bool>();
    const uint32_t size = r.get<uint32_t>();
    if (!r.ok() || size > r.left())
        return false;
    std::vector<uint8_t> chip(size);
    r.bytes(chip.data(), size);
    ymfm::ymfm_saved_state saved(chip, false);
    ym.save_restore(saved);
    psg.load_state(r);
    r.get(bank);
    r.get(reset_held);
    r.get(bus_requested);
    r.get_array(timer_clocks);
    r.get(z80_budget);
    r.get_array(dc_in);
    r.get_array(dc_out);
    r.get_array(lp);
    return r.ok();
}

void Opn2::generate_sample(output_data* out)
{
    if (ladder) {
        generate(out);
        return;
    }
    // As ymfm::ym3438::generate: the channels summed with no DAC discontinuity.
    m_fm.clock(fm_engine::ALL_CHANNELS);
    if (!m_dac_enable) {
        m_fm.output(out->clear(), 5, 256, fm_engine::ALL_CHANNELS);
    } else {
        int32_t dacval = int16_t(m_dac_data << 7) >> 7;
        out->data[0] = m_fm.regs().ch_output_0(0x102) ? dacval : 0;
        out->data[1] = m_fm.regs().ch_output_1(0x102) ? dacval : 0;
        m_fm.output(*out, 5, 256, fm_engine::ALL_CHANNELS ^ (1 << 5));
    }
    out->data[0] = (out->data[0] * 128) / 6;
    out->data[1] = (out->data[1] * 128) / 6;
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
        ym.ladder = !clear_fm;
        ym.generate_sample(&ym_out);
        int32_t psg_out = clean_psg ? psg.sample_clean(psg_steps_per_sample) : psg.sample(psg_steps_per_sample);
        // Model 1 Low-Pass: first order at 3390 Hz (BlastEm's default for a Model 1).
        static const double kLowPass = 1.0 - std::exp(-2.0 * 3.14159265358979 * 3390.0 / (kYmClock / kYmClocksPerSample));
        for (int c = 0; c < 2; c++) {
            // First-order high-pass (about 5 Hz) removes the constant offset of the chips' outputs.
            double in = ym_out.data[c] + psg_out;
            dc_out[c] = in - dc_in[c] + 0.9994 * dc_out[c];
            dc_in[c] = in;
            lp[c] += kLowPass * (dc_out[c] - lp[c]);
            out.push_back((int16_t)std::clamp((int)(low_pass ? lp[c] : dc_out[c]), -32768, 32767));
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
