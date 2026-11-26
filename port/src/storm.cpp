// Weather - Stage 4 (see storm.h).

#include "storm.h"

#include <algorithm>
#include <cmath>

#include "boss_rush.h"                              // nemesis_decode
#include "fade.h"                                   // PaletteOverride

namespace storm {
namespace {

constexpr int kRainDelay = 600;         // frames from the night's end to the rain (10 s)
constexpr int kRainFadeIn = 180;        // frames the rain takes to fade in
constexpr int kRainFadeOut = 60;
constexpr int kStageIndex = 1;          // stage 4
// The final boss's flash (stage 28, palette line 2 colours 0-6): a level a frame up to white,
// held, then a level every 3 frames back.
constexpr double kFlashUp = 6, kFlashHold = 5, kFlashDown = 18;

// The water-entry splash (object $93 subtype 5, MickeyEnterWater): graphics set $5A, Nemesis
// data at $06CE48, loaded to VRAM $6200 (tile $310) in stage 10. Its animation ($0E, mapping
// $06D304) as the sweep saw it: per frame its sprites (first tile, cells across and down, and
// their place against the object, whose Y is the water's surface), and how long it shows.
constexpr uint32_t kSplashArt = 0x06CE48;
constexpr int kSplashFirstTile = 0x310;
struct SplashPiece { int tile, w, h, dx, dy; };
struct SplashFrame { SplashPiece pieces[2]; int count, frames; };
constexpr SplashFrame kSplash[kSplashFrames] = {
    {{{0x310, 4, 1, -16, -8}, {}}, 1, 5},
    {{{0x314, 3, 2, -24, -16}, {0x31A, 3, 2, 0, -16}}, 2, 6},
    {{{0x320, 3, 2, -24, -16}, {0x326, 3, 2, 0, -16}}, 2, 6},
    {{{0x32C, 3, 2, -24, -16}, {0x332, 3, 2, 0, -16}}, 2, 6},
};

struct Flash { int start; float peak; };
struct Thunder { int at; float volume; };

struct State {
    bool active = false;
    int frame = 0;                  // game frames of stage 4 play (not counting pauses)
    int night = -1;                 // frames since the night was seen done (-1: not done)
    float strength = 0;             // the rain's, eased
    float sky = 0;                  // the share of the view open to the sky, eased
    int next_flash = 0;             // frame of the next lightning (0: not chosen yet)
    std::vector<Flash> flashes;
    std::vector<Thunder> thunders;  // due
    uint32_t random = 0x5EED1234u;
    int camera_x = 0, camera_y = 0;
    // Paused ($FFF41D bit 0): the clock stands still, and pictures between frames show the same
    // moment (not part of the way back to the frame before, which made the drops flicker).
    bool paused = false;
};
State g;

// The level's roofs, two rows of a word per 16-pixel column: in the first, the Y of its first
// solid block from the top (low 15 bits; bit 15: it is a slope) and of the first ledge above that (high 16 bits); in
// the second, the next two ledges (0xFFFF: none). A ledge is the top of a run of type 2 blocks.
std::vector<uint32_t> g_heights;
uint32_t g_heights_version = 0, g_layout_key = 0;
int g_level_height = 0;

std::vector<uint32_t> g_splash;
uint32_t g_splash_version = 0;
bool g_splash_tried = false;
float g_splash_scale = 0.25f;

// The thunder playing: its samples (interleaved stereo, -1 to 1) and how far it has got.
std::vector<float> g_voice;
size_t g_voice_at = 0;

float next_random()                 // 0-1, the port's own numbers (the game's are left alone)
{
    uint32_t x = g.random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g.random = x;
    return (float)(x >> 8) / 16777216.0f;
}

uint16_t word(const uint8_t* ram, int address) { return (uint16_t)(ram[address & 0xFFFF] << 8 | ram[(address + 1) & 0xFFFF]); }

uint8_t byte_at(const uint8_t* ram, const std::vector<uint8_t>& rom, uint32_t address)
{
    if (address >= 0xFF0000)
        return ram[address & 0xFFFF];
    return address < rom.size() ? rom[address] : 0;
}

// The roofs from the level's blocks (BlockAt, $9454): the layout at $FFFA88, a byte a 16 x
// 16 block, $FFFA8E blocks across and $FFFA8C down; each block's collision type in the table at
// $FF1400. Types 1 (solid) and 5 (slopes) stop the rain. Type 2 (the thin ledges on the trees,
// which Mickey stands on) doesn't, but the drops splash on it. Type 6 (in stage 4 a row across
// the top of the open pit at X 1664-1823) is not a roof.
void build_heights(const uint8_t* ram, const std::vector<uint8_t>& rom)
{
    const uint32_t layout = (uint32_t)word(ram, 0xFA88) << 16 | word(ram, 0xFA8A);
    const int across = word(ram, 0xFA8E), down = word(ram, 0xFA8C);
    const uint32_t key = layout ^ (uint32_t)across << 20 ^ (uint32_t)down << 8;
    if (key == g_layout_key && !g_heights.empty())
        return;
    if (across <= 0 || across > 1024 || down <= 0 || down > 1024)
        return;
    g_layout_key = key;
    g_level_height = down * 16;
    g_heights.assign((size_t)across * 2, 0xFFFFFFFFu);
    for (int x = 0; x < across; x++) {
        uint16_t found[4] = {(uint16_t)g_level_height, 0xFFFF, 0xFFFF, 0xFFFF};   // the roof, up to 3 ledges
        int ledges = 0;
        for (int y = 0; y < down; y++) {
            const uint8_t type = ram[0x1400 + byte_at(ram, rom, layout + y * across + x)];
            if (type == 1 || type == 5) {
                // A slope's surface runs across its block, so the block's top is no place for a
                // splash: bit 15 marks the column (rain still stops at the block).
                found[0] = (uint16_t)(y * 16 | (type == 5 ? 0x8000 : 0));
                break;
            }
            const uint8_t above = y ? ram[0x1400 + byte_at(ram, rom, layout + (y - 1) * across + x)] : 0;
            if (type == 2 && above != 2 && ledges < 3)
                found[1 + ledges++] = (uint16_t)(y * 16);
        }
        g_heights[x] = found[0] | (uint32_t)found[1] << 16;
        g_heights[across + x] = found[2] | (uint32_t)found[3] << 16;
    }
    g_heights_version++;
}

void build_splash(const std::vector<uint8_t>& rom)
{
    g_splash_tried = true;
    const std::vector<uint8_t> tiles = nemesis_decode(rom, kSplashArt);
    if (tiles.size() < (size_t)(0x338 - kSplashFirstTile) * 32)
        return;
    g_splash.assign(kSplashWidth * kSplashHeight, 0);
    for (int f = 0; f < kSplashFrames; f++)
        for (int p = 0; p < kSplash[f].count; p++) {
            const SplashPiece& piece = kSplash[f].pieces[p];
            for (int cx = 0; cx < piece.w; cx++)
                for (int cy = 0; cy < piece.h; cy++) {
                    const uint8_t* tile = &tiles[(size_t)(piece.tile - kSplashFirstTile + cx * piece.h + cy) * 32];
                    for (int py = 0; py < 8; py++)
                        for (int px = 0; px < 8; px++) {
                            const uint8_t b = tile[py * 4 + px / 2];
                            const int index = px & 1 ? b & 15 : b >> 4;
                            if (!index)
                                continue;
                            const int x = f * kSplashFrameWidth + kSplashFrameWidth / 2 + piece.dx + cx * 8 + px;
                            const int y = kSplashHeight + piece.dy + cy * 8 + py;
                            if (x < 0 || x >= kSplashWidth || y < 0 || y >= kSplashHeight)
                                continue;
                            // Its white (1) and lavender (12, palette line 0's $C88), in the
                            // night's cold light.
                            const uint32_t rgb = index == 1 ? 0xFAE4DCu : 0xBE8278u;    // (0xBBGGRR)
                            g_splash[y * kSplashWidth + x] = 0xFF000000u | rgb;
                        }
                }
        }
    g_splash_version++;
}

double flash_curve(double t)
{
    if (t < 0)
        return 0;
    if (t < kFlashUp)
        return (t + 1) / kFlashUp;
    t -= kFlashUp;
    if (t < kFlashHold)
        return 1;
    t -= kFlashHold;
    return std::max(0.0, 1 - (t + 1) / kFlashDown);
}

double flash_at(double frame)
{
    double f = 0;
    for (const Flash& flash : g.flashes)
        f = std::max(f, flash.peak * flash_curve(frame - flash.start));
    return std::min(1.0, f);
}

// The share of the view (with widescreen's sides) whose columns show some open sky.
float open_sky()
{
    if (g_heights.empty())
        return 0;
    float open = 0;
    int n = 0;
    for (int x = g.camera_x - 48; x < g.camera_x + 368; x += 16, n++) {
        const int column = std::clamp(x / 16, 0, (int)g_heights.size() / 2 - 1);
        open += std::clamp(((int)(g_heights[column] & 0x7FFF) - g.camera_y) / 48.0f, 0.0f, 1.0f);   // (any sky seen in the column)
    }
    return n ? open / n : 0;
}

// A thunderclap: a crack (bright noise, louder when near), then a rumble of low noise rolling
// on for a few seconds. Made up here, not the game's sound.
void start_thunder(float volume, int rate)
{
    const int samples = (int)(rate * 3.6);
    g_voice.assign((size_t)samples * 2, 0.0f);
    g_voice_at = 0;
    // A few rolls: bumps in the rumble's loudness.
    struct Roll { float at, width, height; } rolls[5];
    for (Roll& r : rolls)
        r = {0.15f + next_random() * 1.6f, 0.12f + next_random() * 0.35f, 0.4f + next_random() * 0.6f};
    const float crack = std::clamp((volume - 0.55f) * 2.2f, 0.0f, 1.0f);
    float brown[2] = {}, low1[2] = {}, low2[2] = {}, crack_lp[2] = {};
    const float k1 = 1 - std::exp(-2 * 3.14159f * 140.0f / rate);
    const float k2 = 1 - std::exp(-2 * 3.14159f * 90.0f / rate);
    const float kc = 1 - std::exp(-2 * 3.14159f * 2500.0f / rate);
    float peak = 0;
    for (int i = 0; i < samples; i++) {
        const float t = (float)i / rate;
        float env = std::min(1.0f, t / 0.04f) * std::exp(-t / 1.3f);
        float roll = 0.35f;
        for (const Roll& r : rolls) {
            const float d = (t - r.at) / r.width;
            roll += r.height * std::exp(-d * d);
        }
        env *= roll;
        const float shared = next_random() * 2 - 1;
        for (int ch = 0; ch < 2; ch++) {
            const float white = 0.7f * shared + 0.3f * (next_random() * 2 - 1);
            brown[ch] = brown[ch] * 0.995f + white * 0.1f;
            low1[ch] += k1 * (brown[ch] - low1[ch]);
            low2[ch] += k2 * (low1[ch] - low2[ch]);
            crack_lp[ch] += kc * (white - crack_lp[ch]);
            const float c = crack * crack_lp[ch] * std::exp(-t / 0.09f) * 0.9f;
            const float v = low2[ch] * env * 6.0f + c;
            g_voice[(size_t)i * 2 + ch] = v;
            peak = std::max(peak, std::fabs(v));
        }
    }
    const float gain = peak > 0 ? 0.5f * volume / peak : 0;
    for (float& v : g_voice)
        v *= gain;
}

}  // namespace

void reset()
{
    const uint32_t random = g.random;
    g = State();
    g.random = random;
    g_voice.clear();
    g_voice_at = 0;
}

void set_splash_scale(float scale) { g_splash_scale = std::clamp(scale, 0.1f, 1.0f); }

void update(const uint8_t* ram, const std::vector<uint8_t>& rom, bool enabled)
{
    const int state = word(ram, 0xF300), stage = word(ram, 0xFF42);
    if (!enabled || state != 0x0E || stage != kStageIndex || ram[0xF40A]) {
        if (g.active)
            reset();
        return;
    }
    g.active = true;
    g.camera_x = word(ram, 0xFA84);
    g.camera_y = word(ram, 0xFA80);
    build_heights(ram, rom);
    if (!g_splash_tried)
        build_splash(rom);
    g.paused = ram[0xF41D] & 1;
    if (g.paused)                                   // paused: everything waits
        return;
    g.frame++;
    if (ram[0x9800] & 2)                            // the night is done (StageEffect02)
        g.night = g.night < 0 ? 0 : g.night + 1;
    else
        g.night = -1;
    const bool raining = g.night >= kRainDelay;
    g.strength = raining ? std::min(1.0f, g.strength + 1.0f / kRainFadeIn) : std::max(0.0f, g.strength - 1.0f / kRainFadeOut);
    const float sky = open_sky();
    g.sky += std::clamp(sky - g.sky, -0.04f, 0.04f);

    // Lightning, once the rain is in full: the first a few seconds in, then every 8-20 s,
    // sometimes twice in quick succession; thunder 1-2 s later, louder the sooner it comes.
    if (!raining) {
        g.flashes.clear();
        g.thunders.clear();
        g.next_flash = 0;
    } else if (g.strength >= 1) {
        if (!g.next_flash)
            g.next_flash = g.frame + 180 + (int)(next_random() * 240);
        if (g.frame >= g.next_flash) {
            g.flashes.push_back({g.frame, 0.8f + next_random() * 0.2f});
            if (next_random() < 0.35f)
                g.flashes.push_back({g.frame + 8 + (int)(next_random() * 5), 0.55f + next_random() * 0.35f});
            const float delay = next_random();
            g.thunders.push_back({g.frame + 60 + (int)(delay * 60), 1.0f - 0.45f * delay});
            g.next_flash = g.frame + 480 + (int)(next_random() * 720);
        }
    }
    g.flashes.erase(std::remove_if(g.flashes.begin(), g.flashes.end(),
                                   [](const Flash& f) { return g.frame - f.start > 60; }),
                    g.flashes.end());
}

bool flashing() { return g.active && flash_at(g.frame) > 0; }

void apply(PaletteOverride& pal, double back)
{
    if (!g.active)
        return;
    const float f = (float)flash_at(g.frame - (g.paused ? 0.0 : back));
    if (f <= 0)
        return;
    auto lighten = [&](int i, float r, float gr, float b, float amount) {
        const uint32_t c = pal.rgb[i];
        auto mix = [&](int shift, float to) {
            const float from = (float)((c >> shift) & 255);
            return (uint32_t)std::lround(from + (to - from) * amount) << shift;
        };
        pal.rgb[i] = (c & 0xFF000000u) | mix(16, r) | mix(8, gr) | mix(0, b);
    };
    for (int i = 1; i < 16; i++) {
        // Line 3, the far forest: as much as the view shows sky. Line 2, the trees, the ground and
        // the rock: always (underground too, the light coming in through the openings).
        lighten(48 + i, 232, 236, 255, 0.8f * f * g.sky);
        lighten(32 + i, 205, 215, 255, 0.35f * f);
    }
}

bool picture(Picture& out, double back)
{
    if (!g.active || (g.strength <= 0 && !flashing()) || g_heights.empty())
        return false;
    if (g.paused)
        back = 0;
    out.time = g.frame - back;
    out.strength = g.strength;
    out.flash = (float)flash_at(g.frame - back) * g.sky;
    out.camera_x = g.camera_x;
    out.camera_y = g.camera_y;
    out.splash_scale = g_splash_scale;
    out.heights = &g_heights;
    out.heights_version = g_heights_version;
    out.level_height = g_level_height;
    out.splash = g_splash.empty() ? nullptr : &g_splash;
    out.splash_version = g_splash_version;
    return out.strength > 0;
}

void mix(std::vector<int16_t>& audio, int sample_rate)
{
    for (size_t i = 0; i < g.thunders.size();)
        if (g.frame >= g.thunders[i].at) {
            start_thunder(g.thunders[i].volume, sample_rate);
            g.thunders.erase(g.thunders.begin() + (long)i);
        } else {
            i++;
        }
    if (g_voice_at >= g_voice.size())
        return;
    for (size_t i = 0; i < audio.size() && g_voice_at < g_voice.size(); i++, g_voice_at++) {
        const int v = audio[i] + (int)std::lround(g_voice[g_voice_at] * 32767.0f);
        audio[i] = (int16_t)std::clamp(v, -32768, 32767);
    }
}

}  // namespace storm
