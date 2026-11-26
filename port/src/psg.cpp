#include "psg.h"

#include "state_io.h"

#include <algorithm>
#include <cmath>

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

int32_t Psg::tick()
{
    // Attenuation in 2 dB steps; 15 is silent.
    static const int16_t kLevels[16] = {2000, 1589, 1262, 1002, 796, 632, 502, 399,
                                        317, 252, 200, 159, 126, 100, 79, 0};
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
        if (output[3]) {                            // shift on the rising edge
            bool white = noise & 4;
            uint16_t feedback = white ? ((lfsr ^ (lfsr >> 3)) & 1) : (lfsr & 1);
            lfsr = (lfsr >> 1) | (feedback << 15);
        }
    }
    int32_t sum = 0;
    for (int ch = 0; ch < 3; ch++)
        sum += output[ch] ? kLevels[volume[ch]] : -kLevels[volume[ch]];
    sum += (lfsr & 1) ? kLevels[volume[3]] : -kLevels[volume[3]];
    return sum;
}

int16_t Psg::sample(double steps)
{
    phase += steps;
    int ticks = (int)phase;
    phase -= ticks;
    int32_t sum = 0;
    for (int t = 0; t < std::max(ticks, 1); t++)
        sum += tick();
    return (int16_t)(sum / std::max(ticks, 1));
}

namespace {

// Clean PSG's filter: a Kaiser-windowed sinc over the last kTaps ticks (clock/16, about
// 224 kHz), low-pass at 20 kHz, so that the square waves' harmonics no longer fold back
// into the audible range at the output rate (53 kHz). It is stopped by about 60 dB from
// 33 kHz up, where folding would land below 20 kHz. One row of weights for each of
// kPhases positions of the output sample between two ticks; each row sums to 1.
constexpr int kTaps = Psg::kHistory;
constexpr int kPhases = 64;
constexpr double kPi = 3.14159265358979323846;

struct CleanFilter {
    float w[kPhases][kTaps];
    CleanFilter()
    {
        const double fc = 20000.0 / (3579545.0 / 16.0);    // cycles per tick
        const double beta = 5.65;                           // Kaiser window, about 60 dB
        auto bessel_i0 = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 30; k++) {
                term *= (x / (2 * k)) * (x / (2 * k));
                sum += term;
            }
            return sum;
        };
        const double half = kTaps / 2.0;
        for (int p = 0; p < kPhases; p++) {
            double total = 0;
            for (int j = 0; j < kTaps; j++) {
                // Distance from the filter's centre (half the history back from the output
                // sample) to tick j, oldest first.
                double u = half + (double)p / kPhases - j - 0.5;
                double r = u / half;
                double win = std::fabs(r) < 1 ? bessel_i0(beta * std::sqrt(1 - r * r)) / bessel_i0(beta) : 0;
                double x = 2 * fc * u;
                double sinc = x == 0 ? 1 : std::sin(kPi * x) / (kPi * x);
                w[p][j] = (float)(2 * fc * sinc * win);
                total += w[p][j];
            }
            for (int j = 0; j < kTaps; j++)
                w[p][j] = (float)(w[p][j] / total);
        }
    }
};

}   // namespace

int16_t Psg::sample_clean(double steps)
{
    static const CleanFilter filter;
    phase += steps;
    int ticks = (int)phase;
    phase -= ticks;
    for (int t = 0; t < ticks; t++) {
        history[head] = (float)tick();
        head = (head + 1) % kHistory;
    }
    // The output sample sits `phase` of a tick after the newest tick, and the filter is
    // centred half the history before it (a delay of about 0.14 ms).
    const float* w = filter.w[std::min((int)(phase * kPhases), kPhases - 1)];
    float sum = 0;
    for (int j = 0; j < kHistory; j++)
        sum += w[j] * history[(head + j) % kHistory];
    return (int16_t)std::clamp((int)std::lround(sum), -32768, 32767);
}

void Psg::save_state(StateWriter& w) const
{
    w.put_array(tone);
    w.put_array(volume);
    w.put(noise);
    w.put(latch);
    for (int c : counter)
        w.put<int32_t>(c);
    w.put_array(output);
    w.put(lfsr);
    w.put(phase);
    w.put_array(history);
    w.put<int32_t>(head);
}

void Psg::load_state(StateReader& r)
{
    r.get_array(tone);
    r.get_array(volume);
    r.get(noise);
    r.get(latch);
    for (int& c : counter)
        c = r.get<int32_t>();
    r.get_array(output);
    r.get(lfsr);
    r.get(phase);
    r.get_array(history);
    head = r.get<int32_t>() & (kHistory - 1);
}
