// Weather - Stage 4 (Enhancements > Graphics, GPU renderer only): about 10 seconds after stage 4's
// day-to-night change, rain starts, with lightning now and then and thunder a second or two
// after each flash. Display and sound only: the game's RAM, VRAM and CRAM are read, never
// written, and the game's random numbers are left alone. See docs/notes/stage4-storm.md.
//
// - The night is the stage's own effect (StageEffect02, vint.asm): bit 1 of $FF9800 is set
//   when it is done; the rain starts 600 frames after that and fades in.
// - Rain stops at roofs: for each 16-pixel column of the level, the first solid block from
//   the top (the level's collision blocks in RAM, as BlockAt reads them), so it falls on the
//   open forest and not into the tunnels or the caves.
// - The lightning is a palette override (like Smooth Fades): the far forest (palette line 3)
//   goes towards white and the trees and ground (line 2) a little, timed as the final boss's
//   flashes (6 frames up, 5 held, 18 down); sprites and the HUD are untouched.
// - The splashes where drops land are the game's own water-entry splash (object $93,
//   decoded from the player's ROM), drawn small.
#pragma once

#include <cstdint>
#include <vector>

struct PaletteOverride;

namespace storm {

// What the rain pass draws for a picture (GpuVdp::draw_storm).
struct Picture {
    double time = 0;            // game frames, with the fraction for pictures between frames
    float strength = 0;         // rain: 0 none to 1 full
    float flash = 0;            // lightning: 0-1
    int camera_x = 0, camera_y = 0;     // CameraA ($FFFA84, $FFFA80)
    int hud_top = 224;          // the first line of the HUD (the window); no rain from there
    float splash_scale = 0.25f; // the splashes' size against the game's
    const std::vector<uint32_t>* heights = nullptr; // two rows, a word per 16-pixel column: the roof's Y and the
                                                    // ledges' (storm.cpp's g_heights)
    int level_height = 0;       // (a column with no roof has this)
    uint32_t heights_version = 0;
    const std::vector<uint32_t>* splash = nullptr;  // the splash's frames, kSplashWidth x 16 (0xAABBGGRR)
    uint32_t splash_version = 0;
};
constexpr int kSplashFrameWidth = 48, kSplashFrames = 4, kSplashWidth = kSplashFrameWidth * kSplashFrames;
constexpr int kSplashHeight = 16;

// Once per game frame (at the start of run_frame): follows the night, the rain and the
// lightning. `enabled`: the setting is on and the GPU renderer draws.
void update(const uint8_t* ram, const std::vector<uint8_t>& rom, bool enabled);
// True while a flash is under way (the palette must be overridden, Smooth Fades or not).
bool flashing();
// Lightens the colours of `pal` for the flash, `back` game frames (0-1) before the current one.
void apply(PaletteOverride& pal, double back);
// The rain pass's parameters for a picture `back` frames before the current one; false when
// there is nothing to draw.
bool picture(Picture& out, double back);
// Adds the thunder to a frame's sound (interleaved stereo).
void mix(std::vector<int16_t>& audio, int sample_rate);
// The splashes' size (--storm-splash): 0.25 by default.
void set_splash_scale(float scale);
// Restarts everything (a reset, or the setting switched off).
void reset();

}  // namespace storm
