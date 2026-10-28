// Castle of Illusion PC port: runs the original 68000 code on Musashi, with a
// C++ VDP and an SDL3 front end.
//
// Usage: coi [rom.bin] [--stage N] [--frames N] [--dump out.ppm] [--press BUTTON@FIRST-LAST ...]
//   --stage N    start the first level of a new game at stage N (1-28), see README
//   --show-state show the GameState in the window title
//   --no-widescreen start at the original 320 x 224 (the F1 menu can switch it later)
//   --easy-bounce start with Easy Bounce Attack on (Enhancements > Gameplay)
//   --smooth-swing start with Smooth Swinging on (Enhancements > Graphics)
//   --smooth-scroll start with Smooth Scrolling on (Enhancements > Graphics); --no-smooth-scroll off
//   --vanilla-rock-edges start with Clean Rock Edges off (Enhancements > Graphics)
//   --vanilla-fades start with Smooth Fades off (Enhancements > Graphics)
//   --show-fps   start with the FPS counter shown (Video)
//   --interpolate start with High Refresh Interpolation on (Video); --no-interpolate off
//   --frame-rate N start at frame rate N: 60, 120 or 240, or 0 for the display's (Video)
//   --mdplus, --no-mdplus  play the MD+ pack in mdplus/ or not (Enhancements > Audio);
//   --log-mdplus print the MD+ commands the game sends (testing)
//   --cheat NAME switch a cheat on for this run (Cheats): invincibility, infinite-power,
//                infinite-tries, infinite-items, infinite-continues, full-power, walk-faster,
//                moon-jump, midair-jump; repeat for more
//   --crt        start with the CRT shader on (Enhancements > Graphics)
//   --console    show a terminal window for messages (Windows; none opens otherwise)
//   --scanlines N start with simple scanlines at N percent and the CRT shader off
//   --hqx N      start with hqx at scale N (2-4, 1 = off) and the CRT shader off
// Menu settings are remembered between runs (settings.cpp); the options above apply to
// this run only. Headless runs ignore the saved settings.
// While running: F1 opens the menu, F11 or Alt+Enter toggles fullscreen, F12 the CRT shader,
// Ctrl+R resets, F2/F3/F4 pick the Quick Settings (Enhanced, Vanilla, Custom).
// Testing (headless): --poke FRAME:ADDR=VALUE, --poke-file FILE, --peek ADDR, --watch ADDR, --ram FILE,
//   --vram FILE, --cram FILE (CRAM, registers, VSRAM), --wav FILE, --log-state, --log-ram ADDR, --trace-vdp, --no-sprite-masking,
//   --capture FRAME FILE (windowed: save the window's picture after FRAME frames, then quit)
//   --switch-widescreen FRAME, --switch-smooth-swing FRAME, --reset FRAME, --go-to-stage FRAME:STAGE (the menu's
//   actions, at the start of a frame)
//   --frames N   run N frames without a window, then exit (with --dump: save the last frame)
//   --interp-back X  with --dump: draw the last frame as interpolation would X frames earlier (0-1)
//   --refresh HZ treat the display as running at HZ (windowed; for testing the pacing)
//   --press      hold a button (up/down/left/right/a/b/c/start) for a range of frames (also windowed)

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bus.h"
#include "cheats.h"
#include "display.h"
#include "fade.h"
#include "interp.h"
#include "mdplus.h"
#include "menu.h"
#include "patches.h"
#include "settings.h"
#include "smooth_scroll.h"
#include "smooth_swing.h"
#include "sound.h"
#include "stages.h"
#include "vdp.h"

extern "C" {
#include "m68k.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
}

namespace {

constexpr int kWidth = 320;                         // the original view
constexpr int kWideWidth = kWidth + 2 * kWideMargin;
// GameStates drawn at 400 pixels; every other state is pillarboxed at 320.
//   $0E  level play
//   $22  the cutscenes between worlds (set up by $20, which picks the scene by level):
//        a scripted camera over 512-pixel planes that are drawn once. The castle
//        exterior before the final boss needs a fill past its brick tower (TowerFill).
//   $2E  the ending, in the final arena: same walls as the fight, drawn in front of the
//        sprites so the characters exit behind them.
// Identified by play with --show-state: $06 is the intro after boot and $2A the scene
// before the hallway; both stay pillarboxed.
bool is_wide_state(uint16_t state, uint16_t level)
{
    (void)level;
    return state == 0x0E || state == 0x22 || state == 0x2E;
}

bool g_widescreen = true;
bool g_easy_bounce = false;
bool g_clean_rock_edges = true;                     // Enhancements > Graphics > Clean Rock Edges
bool g_smooth_fades = true;                         // Enhancements > Graphics > Smooth Fades (fade.cpp)
FadeSmoother g_fade;
PaletteOverride g_fade_palette;                     // this frame's colours
PaletteOverride g_fade_palette_back;                // for frames drawn between two game frames
Cheats g_cheats;                                    // the Cheats menu, patched into the ROM
int g_out_width = kWideWidth;                       // width of the frame buffer
// The ROM as dumped; the bus runs a copy with the patches for the current settings.
std::vector<uint8_t> g_rom_original;
// MD+ (mdplus.cpp): a pack found in mdplus/, its patched ROM, and the cartridge ROM from rom/.
MdPlus g_mdplus;
std::vector<uint8_t> g_rom_cartridge, g_rom_mdplus;
bool g_mdplus_on = false;
bool g_wide_available = false, g_smooth_swing_available = false;
bool g_smooth_swing = false;
// Smooth scrolling draws each game pixel as kSubpixels pixels across, so the planes can move
// by fractions of a pixel; g_scale is 1 when it is off.
constexpr int kSubpixels = 4;
int g_scale = 1;
ScrollSmoother g_scroll_smoother;
std::vector<uint16_t> g_b_sprite_tiles;           // see run_frame: first tile (with palette) of sprites moving with plane B
// hqx with smooth scrolling: each line is drawn as separate layers at whole-pixel positions
// (with kLayerMargin extra pixels each side), which the display enlarges one by one and
// moves by the smoother's fractions of a pixel. g_scale stays 1.
constexpr int kLayerMargin = 2;
constexpr int kLayerStride = kWideWidth + 2 * kLayerMargin;   // room for any width
bool g_layered = false;
std::vector<uint32_t> g_layers[LayeredFrame::kLayers];
static_assert(LayeredFrame::kLayers == Vdp::LineLayers::kCount, "layer counts differ");
float g_line_shifts[Vdp::kScreenHeight * 4];
uint32_t g_line_backdrop[Vdp::kScreenHeight];
int g_layer_clip_left = 0, g_layer_clip_right = 0;
constexpr int kLines = 262;                         // NTSC
constexpr int kCyclesPerLine = 488;                 // 7.67 MHz / 59.92 Hz / 262 lines
constexpr double kFrameSeconds = 1.0 / 59.92;
constexpr double kZ80CyclesPerLine = Sound::kZ80Clock / 59.92 / kLines;

Bus g_bus;
Vdp g_vdp(g_bus);
Sound g_sound(g_bus);
std::vector<int16_t> g_audio;                       // interleaved stereo samples for the current frame
int g_irq_pending = 0;                              // bit n = interrupt level n pending

void update_irq()
{
    int level = 0;
    for (int l = 7; l > 0; l--)
        if (g_irq_pending & (1 << l)) {
            level = l;
            break;
        }
    m68k_set_irq(level);
}

void raise_irq(int level)
{
    g_irq_pending |= 1 << level;
    update_irq();
}

struct Press {
    uint8_t button;
    int first, last;
};

bool parse_press(const std::string& spec, Press& press)
{
    static const struct { const char* name; uint8_t bit; } kNames[] = {
        {"up", BTN_UP}, {"down", BTN_DOWN}, {"left", BTN_LEFT}, {"right", BTN_RIGHT},
        {"a", BTN_A}, {"b", BTN_B}, {"c", BTN_C}, {"start", BTN_START}};
    size_t at = spec.find('@'), dash = spec.find('-', at);
    if (at == std::string::npos || dash == std::string::npos)
        return false;
    std::string name = spec.substr(0, at);
    for (const auto& n : kNames)
        if (name == n.name) {
            press.button = n.bit;
            press.first = std::stoi(spec.substr(at + 1, dash - at - 1));
            press.last = std::stoi(spec.substr(dash + 1));
            return true;
        }
    return false;
}

// Where to look for a folder the player supplies (rom/, mdplus/): beside the program first,
// then in the current folder and up to three folders above it (for running from build/).
std::vector<std::filesystem::path> search_dirs(const char* name)
{
    std::vector<std::filesystem::path> dirs;
    if (const char* base = SDL_GetBasePath())
        dirs.push_back(std::filesystem::path(base) / name);
    for (const char* up : {"", "../", "../../", "../../../"})
        dirs.push_back(std::filesystem::path(std::string(up) + name));
    return dirs;
}

std::vector<uint8_t> load_rom(const std::string& explicit_path)
{
    std::vector<std::filesystem::path> candidates;
    if (!explicit_path.empty())
        candidates.push_back(explicit_path);
    else
        for (const auto& dir : search_dirs("rom"))
            if (std::filesystem::is_directory(dir))
                for (const auto& entry : std::filesystem::directory_iterator(dir))
                    if (entry.path().extension() == ".bin")
                        candidates.push_back(entry.path());
    for (const auto& path : candidates) {
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (data.size() == 0x80000) {
            std::printf("ROM: %s\n", path.string().c_str());
            return data;
        }
    }
    return {};
}

// Per-scene fills for the widened strips, where a scene's plane holds the wrong thing past
// the original view (see docs/widescreen.md). Chosen once per frame in run_frame.
enum class StripRule { None, TowerFill, ArenaWalls };
StripRule g_strip_rule = StripRule::None;

// Floor block to repeat as walls in an arena's strips, in plane A pixels.
struct WallSource { int x, y, w, h; };
WallSource g_wall;
constexpr WallSource kLevel2BossWalls = {192, 224, 32, 32};  // stage 9: 32 x 32 blocks, tiles $101-$110
constexpr WallSource kFinalBossWalls = {192, 464, 64, 32};   // stage 28: 64 x 32 floor slabs, tiles $175-$189

int wrap(int value, int size) { return ((value % size) + size) % size; }

// Remaps plane A coordinates for pixels in the widened strips (view_x < 0 or >= 320).
bool extend_plane_a(int view_x, int line, int& plane_x, int& plane_y)
{
    (void)line;
    switch (g_strip_rule) {
    case StripRule::TowerFill:
        // Castle exterior before the final boss: the brick tower ends at plane A X 512 (0),
        // and past it the plane starts over with the pale castle wall. In the right strip,
        // repeat the tower's last 8-pixel column (its dark shading) instead.
        if (view_x >= 320 && wrap(plane_x, 512) < 40) {
            plane_x = 504 + wrap(plane_x, 8);
            return true;
        }
        return false;
    case StripRule::ArenaWalls:
        // Locked boss arenas whose strips lie outside the player's reach (stages 9 and 28):
        // walls of the arena's own floor blocks on both sides, aligned to the floor's grid
        // in plane coordinates, so they line up with the floor and scroll with it.
        plane_x = g_wall.x + wrap(plane_x - g_wall.x, g_wall.w);
        plane_y = g_wall.y + wrap(plane_y - g_wall.y, g_wall.h);
        return true;
    default:
        return false;
    }
}

// Draws one line as layers (g_layered), with the shifts that move them when drawn.
void render_layer_line(Vdp& vdp, int line, bool wide)
{
    const int stride = g_out_width + 2 * kLayerMargin;
    // Outside widescreen play, the original 320 pixels sit in the middle of the frame.
    const int border = wide ? 0 : (g_out_width - kWidth) / 2;
    // There the margins are the black border, as in the flat picture (the game never shows
    // what lies beyond those 320 pixels); elsewhere they hold the planes beyond the edge.
    const int margin = border ? 0 : kLayerMargin;
    Vdp::LineLayers layers;
    for (int i = 0; i < Vdp::LineLayers::kCount; i++) {
        uint32_t* row = &g_layers[i][line * stride];
        std::fill(row, row + stride, i == 0 && border ? 0xFF000000u : 0u);
        layers.layer[i] = row + border + kLayerMargin - margin;
    }
    layers.sub_scale = kSubpixels;
    if (wide)
        vdp.render_line(line, nullptr, kWideWidth + 2 * margin, kWideMargin + margin, 1, &layers);
    else
        vdp.render_line(line, nullptr, kWidth + 2 * margin, margin, 1, &layers);

    float* shift = &g_line_shifts[line * 4];
    shift[0] = layers.window_line ? 0.0f : (float)layers.sub_a / kSubpixels;   // the window stays put
    shift[1] = (float)layers.sub_sprites / kSubpixels;                          // sprites move with plane A
    shift[2] = (float)layers.sub_b / kSubpixels;
    shift[3] = (float)vdp.sub_sprites_b / kSubpixels;                            // sprites moving with CameraB

    // hqx blends each pixel with its neighbours, so parts of a layer hidden behind another
    // would bleed into its visible parts (the flat picture never has them). Where a layer
    // moves with the layer in front of it, its hidden pixels take the front pixel's colour.
    using L = Vdp::LineLayers;
    const float moves[L::kCount] = {shift[2], shift[0], shift[1], shift[2], shift[0], shift[3], shift[1]};
    const int count = (wide ? kWideWidth : kWidth) + 2 * margin;
    for (int x = 0; x < count; x++) {
        int top = L::kCount - 1;
        while (top > 0 && !layers.layer[top][x])
            top--;
        for (int i = 0; i < top; i++)
            if (moves[i] == moves[top])
                layers.layer[i][x] = layers.layer[top][x];
    }
    g_line_backdrop[line] = layers.backdrop;
    g_layer_clip_left = border;
    g_layer_clip_right = wide ? g_out_width : border + kWidth;
}

// Draws one line: widened during level play, otherwise the original 320
// pixels, centred with black borders when the frame buffer is wider.
void render_line(Vdp& vdp, int line, uint32_t* row, bool wide)
{
    if (g_layered) {
        render_layer_line(vdp, line, wide);
        return;
    }
    if (wide) {
        vdp.render_line(line, row, kWideWidth, kWideMargin, g_scale);
        return;
    }
    const int border = (g_out_width - kWidth) / 2 * g_scale, row_width = g_out_width * g_scale;
    for (int x = 0; x < border; x++)
        row[x] = row[row_width - 1 - x] = 0xFF000000;
    vdp.render_line(line, row + border, kWidth, 0, g_scale);
}

// High Refresh Interpolation (interp.cpp): the last two game frames, and a second VDP that
// redraws the current one for a moment between them.
FrameInterpolator g_interp;
bool g_interp_capture = false;                      // snapshots are being taken
Vdp g_interp_vdp(g_bus);

// Draws the current game frame as it was `back` frames earlier (0-1) into `pixels` (or the
// layers), moving the planes and sprites back along their movement since the previous frame.
void render_interpolated(uint32_t* pixels, double back)
{
    const FrameInterpolator::Snapshot& now = g_interp.current();
    const FrameInterpolator::Snapshot& before = g_interp.previous();
    Vdp& v = g_interp_vdp;
    std::memcpy(v.vram, now.vram.data(), sizeof(v.vram));
    v.extend_plane_a = g_vdp.extend_plane_a;
    v.strip_fill_in_front = g_vdp.strip_fill_in_front;
    v.clean_black_edges = g_vdp.clean_black_edges;
    v.edge_tiles = g_vdp.edge_tiles;
    v.sprite_limit = g_vdp.sprite_limit;
    v.palette = nullptr;
    if (g_vdp.palette) {
        g_fade.fill(g_fade_palette_back, back);
        v.palette = &g_fade_palette_back;
    }
    v.sprite_masking = g_vdp.sprite_masking;
    v.sprite_moves_with_b = g_vdp.sprite_moves_with_b;

    // Each line's plane shifts in pixels: from the smoother's fitted movement when smooth
    // scrolling is on, otherwise part of the way back along the change since the previous frame.
    const bool smoothing = g_scale > 1 || g_layered;
    // shift_now is the shift the current frame itself is drawn with (smooth scrolling's
    // correction, or none).
    static double shift[Vdp::kScreenHeight][2], shift_now[Vdp::kScreenHeight][2];
    for (int line = 0; line < Vdp::kScreenHeight; line++) {
        const int now_hs[2] = {now.lines[line].hs_a, now.lines[line].hs_b};
        const int before_hs[2] = {before.lines[line].hs_a, before.lines[line].hs_b};
        for (int plane = 0; plane < 2; plane++) {
            double offset;
            shift_now[line][plane] = 0;
            if (smoothing && g_scroll_smoother.line_at(line, plane, back, offset)) {
                shift[line][plane] = offset;
                g_scroll_smoother.line_at(line, plane, 0, shift_now[line][plane]);
            } else {
                const int d = scroll_difference(before_hs[plane], now_hs[plane]);
                shift[line][plane] = d >= -32 && d <= 32 ? -back * d : 0.0;
            }
        }
    }
    // Sprites the game moves with the background camera keep the current frame's smoothing
    // shift; their movement comes from their objects, below.
    double camera_b = 0;
    v.sub_sprites_b = smoothing && g_scroll_smoother.camera_at(0, camera_b) ? (int)std::lround(camera_b * kSubpixels) : 0;

    v.scroll_at_scale_1 = true;
    v.smooth_scroll = [](int line, int, int, int scale, int& sub_a, int& sub_b) {
        sub_a = (int)std::lround(shift[line][0] * scale);
        sub_b = (int)std::lround(shift[line][1] * scale);
    };
    // The camera's part of a level sprite's movement: how much further right than in the
    // current frame it puts things at this moment. From the smoother's fitted camera movement
    // when smoothing (as the planes), otherwise part of the way back along the camera's step.
    double camera_then = 0, camera_now = 0;
    const bool camera_smoothed = smoothing && g_scroll_smoother.camera_a_at(back, camera_then)
                                 && g_scroll_smoother.camera_a_at(0, camera_now);
    const double camera_smoothing = camera_then - camera_now;
    v.sprite_offset = [&](int index, int line, uint16_t attr, int scale, int& dx, int& dy) {
        // Each sprite goes back along its object's movement and the camera's; sprites not
        // drawn by an object (masks, borders) stay put. The renderer moves sprites with plane
        // A's shift (whole output pixels, or the sprite layers' shift): of that only the
        // current frame's own shift is kept.
        int mx = 0, my = 0, cx = 0, cy = 0;
        bool screen;
        const bool moving = g_interp.sprite_motion(index, mx, my, cx, cy, screen);
        const double camera = !moving || screen ? 0.0 : camera_smoothed ? camera_smoothing : back * cx;
        const int follow = moving ? (int)std::lround(-back * mx * scale) + (int)std::lround(camera * scale) : 0;
        dy = moving ? (int)std::lround(-back * my) + (int)std::lround(back * cy) : 0;
        if (v.sprite_moves_with_b && v.sprite_moves_with_b(attr)) {
            dx = follow;                            // drawn with the frame's camera B shift
            return;
        }
        if (g_layered) {
            // The sprite layers carry the frame's shift plus the smoothed camera part (as a
            // fraction of a pixel); sprites not on the level undo that part.
            const int layer = camera_smoothed ? (int)std::lround(camera_smoothing * scale) : 0;
            if (!moving)
                dx = -layer;
            else if (screen)
                dx = (int)std::lround(-back * mx * scale) - layer;
            else
                dx = (int)std::lround(-back * mx * scale) + (camera_smoothed ? 0 : (int)std::lround(back * cx * scale));
            return;
        }
        const int plane_a = (int)std::lround(shift[line][0] * scale);
        const int frame_a = (int)std::lround(shift_now[line][0] * scale);
        dx = follow + frame_a - plane_a;
    };
    const double layer_camera = camera_smoothed ? camera_smoothing : 0.0;
    v.sprite_layer_shift = [layer_camera](int line, int scale) {
        return (int)std::lround((shift_now[line][0] + layer_camera) * scale);
    };

    uint32_t* const row_base = pixels;
    for (int line = 0; line < Vdp::kScreenHeight; line++) {
        const FrameInterpolator::Line& l = now.lines[line];
        const FrameInterpolator::Line& p = before.lines[line];
        std::memcpy(v.reg, l.reg, sizeof(v.reg));
        std::memcpy(v.cram, l.cram, sizeof(v.cram));
        for (int i = 0; i < 40; i++) {
            const int d = scroll_difference(p.vsram[i] & 0x3FF, l.vsram[i] & 0x3FF);
            v.vsram[i] = d >= -32 && d <= 32 ? (uint16_t)((l.vsram[i] - std::lround(back * d)) & 0x7FF) : l.vsram[i];
        }
        render_line(v, line, row_base + line * g_out_width * g_scale, now.wide);
    }
    v.sprite_offset = nullptr;
    v.sprite_layer_shift = nullptr;
    v.smooth_scroll = nullptr;
}

void set_smooth_scroll(bool on)
{
    g_scale = on ? kSubpixels : 1;
    g_scroll_smoother.reset();
}

void run_frame(uint32_t* pixels)
{
    g_audio.clear();
    g_vdp.start_frame();
    if (g_smooth_fades) {                           // CRAM holds this frame's colours by now
        g_fade.update(g_vdp.cram);
        g_fade.fill(g_fade_palette);
        g_vdp.palette = &g_fade_palette;
    } else {
        g_fade.reset();
        g_vdp.palette = nullptr;
    }
    const uint16_t state = g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301];
    const uint16_t level = g_bus.ram[0xFF44] << 8 | g_bus.ram[0xFF45];
    const uint16_t act = g_bus.ram[0xFF46] << 8 | g_bus.ram[0xFF47];
    const bool wide = g_widescreen && is_wide_state(state, level);
    // $20/$22 pick the castle exterior for level 5, or for level 3 when $FFFF0D is 2
    // (the scene table at $07AB74).
    const bool exterior = level == 5 || (g_bus.ram[0xFF0D] == 2 && level == 3);
    if (state == 0x22 && exterior)
        g_strip_rule = StripRule::TowerFill;
    else if (state == 0x0E && level == 1 && act == 3) {
        g_strip_rule = StripRule::ArenaWalls;
        g_wall = kLevel2BossWalls;
    } else if ((state == 0x0E || state == 0x2E) && level == 5 && act == 0) {
        g_strip_rule = StripRule::ArenaWalls;
        g_wall = kFinalBossWalls;
    } else {
        g_strip_rule = StripRule::None;
    }
    // In the ending ($2E, in the final arena) the characters run out past the left edge:
    // draw the walls in front of them so they exit behind the wall, not through it.
    g_vdp.strip_fill_in_front = state == 0x2E && g_strip_rule == StripRule::ArenaWalls;
    g_vdp.clean_black_edges = g_clean_rock_edges && state == 0x0E && level == 0;   // forest stages
    // Stage 15's building tops (objects $BE and $BF) are sprites the game moves with plane
    // B; for smooth scrolling, mark their sprites by the tiles they use (32 from the base
    // in the object's word at $4, palette included).
    g_b_sprite_tiles.clear();
    for (uint32_t obj = 0xD000; obj < 0xE000; obj += 0x40) {
        const uint16_t id = g_bus.ram[obj] << 8 | g_bus.ram[obj + 1];
        if ((id == 0xBE || id == 0xBF) && !(g_bus.ram[obj + 2] & 0x04))
            g_b_sprite_tiles.push_back((g_bus.ram[obj + 4] << 8 | g_bus.ram[obj + 5]) & 0x67FF);
    }
    g_scroll_smoother.begin_frame();
    if (g_interp_capture)
        g_interp.begin_frame(wide);
    if (g_scale > 1 || g_layered)                   // for sprites moving with the background camera
        g_vdp.sub_sprites_b = g_scroll_smoother.camera(-(g_bus.ram[0xFAC4] << 8 | g_bus.ram[0xFAC5]), kSubpixels);
    if (g_interp_capture && (g_scale > 1 || g_layered))   // for level sprites between frames
        g_scroll_smoother.camera_a(-(g_bus.ram[0xFA84] << 8 | g_bus.ram[0xFA85]));
    for (int line = 0; line < kLines; line++) {
        g_vdp.set_line(line);
        if (line < Vdp::kScreenHeight)
        {
            if (g_interp_capture)
                g_interp.capture_line(g_vdp, line);
            render_line(g_vdp, line, pixels + line * g_out_width * g_scale, wide);
        }
        if (line == Vdp::kScreenHeight) {
            g_vdp.set_vblank(true);
            if (g_vdp.vint_enabled())
                raise_irq(6);
            g_sound.raise_irq();                    // the Z80 interrupt is held for about one line
        } else if (line == Vdp::kScreenHeight + 1) {
            g_sound.clear_irq();
        }
        if (g_vdp.hint_line() && g_vdp.hint_enabled())
            raise_irq(4);
        m68k_execute(kCyclesPerLine);
        g_sound.run(kZ80CyclesPerLine, g_audio);
    }
}

// The stage being played (1-28) for the menu's check mark: level start and play outside
// the attract demo; 0 otherwise.
int current_stage()
{
    const int state = g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301];
    if ((state != 0x0C && state != 0x0E) || g_bus.ram[0xF40A])
        return 0;
    return stage_number(g_bus.ram[0xFF42] << 8 | g_bus.ram[0xFF43]);
}

// Easy Bounce Attack: in the original, Mickey only bounces on enemies if Down is pressed during a
// jump. This taps Down for the one frame after take-off, as a player would, so every jump
// becomes the bounce attack; the jump button is left alone, so holding it still decides the
// height. Take-off is bit 0 of $FFF406, set by the jump routine ($00C834) and cleared on
// landing (falling off a ledge doesn't set it). Where the stage swaps Up and Down (bit 0 of
// $FFF441, handled by $00CE3C), the tap is Up, which the game then reads as Down.
uint8_t easy_bounce_buttons()
{
    static bool was_airborne = false;
    const bool airborne = g_bus.ram[0xF406] & 1;
    const bool took_off = airborne && !was_airborne;
    was_airborne = airborne;
    const bool playing = (g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301]) == 0x0E && !g_bus.ram[0xF40A];
    if (!g_easy_bounce || !took_off || !playing)
        return 0;
    return (g_bus.ram[0xF441] & 1) ? BTN_UP : BTN_DOWN;
}

// Power-on reset of the whole console, as when switching it off and on again.
void power_on()
{
    g_mdplus.stop();
    g_bus.power_on();
    g_vdp.power_on();
    g_fade.reset();
    g_sound.power_on();
    g_irq_pending = 0;
    m68k_set_irq(0);
    m68k_pulse_reset();
}

// Moves the live camera limits between their original and widescreen values, so switching
// widescreen during level play takes effect without restarting the stage. InitCamera sets
// minimum X 0 and maximum X (map width in blocks, +$E) * 16 - 320; the patched routines at
// $07FF00 and $07FF6E set 40 and the values computed here. Limits the stage has since
// changed itself (boss locks and the like) are left alone.
void retune_camera_limits(bool wide)
{
    if ((g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301]) != 0x0E)
        return;                                     // other states run InitCamera again first
    auto word = [](int a) { return (int16_t)(g_bus.ram[a] << 8 | g_bus.ram[a + 1]); };
    auto set = [](int a, int v) { g_bus.ram[a] = (uint8_t)(v >> 8); g_bus.ram[a + 1] = (uint8_t)v; };
    for (int camera : {0xFA80, 0xFAC0}) {
        const int min = word(camera + 0x22), max = word(camera + 0x26);
        const int original_max = word(camera + 0x0E) * 16 - kWidth;
        auto wide_max = [original_max](int min) {
            int m = original_max - kWideMargin;
            return m < min && min >= 192 ? original_max : m;
        };
        if (wide) {
            const int new_min = min == 0 ? kWideMargin : min;
            if (max == original_max)
                set(camera + 0x26, wide_max(new_min));
            set(camera + 0x22, new_min);
        } else {
            if (max == wide_max(min))
                set(camera + 0x26, original_max);
            if (min == kWideMargin)
                set(camera + 0x22, 0);
        }
    }
}

// Switches widescreen: the ROM code (only the patched bytes differ, so the running code
// carries on), the camera limits and the frame width. Planes are streamed as the camera
// moves, so the new strips can show stale columns until the view has scrolled past them.
void build_rom()
{
    std::vector<uint8_t> rom = g_rom_original;
    if (g_widescreen)
        apply_widescreen_patches(rom);
    if (g_smooth_swing)
        apply_smooth_swing(rom);
    const std::string skipped = apply_cheats(rom, g_cheats);
    if (!skipped.empty())
        std::fprintf(stderr, "%s\n", skipped.c_str());
    fix_checksum(rom);
    g_bus.rom = std::move(rom);
}

void set_widescreen(bool wide)
{
    if (wide && !g_wide_available)
        return;
    g_widescreen = wide;
    build_rom();
    g_out_width = wide ? kWideWidth : kWidth;
    retune_camera_limits(wide);
}

// Switches smooth swinging; vines already in RAM are moved to the matching step of the other
// animation, so a swing in progress carries on from where it is.
void set_smooth_swing(bool on)
{
    if (on && !g_smooth_swing_available)
        return;
    if (on != g_smooth_swing)
        convert_swing_objects(g_bus.ram, on);
    g_smooth_swing = on;
    build_rom();
}

void write_wav(const char* path, const std::vector<int16_t>& samples, int rate)
{
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return;
    auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = (uint32_t)(samples.size() * 2);
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    std::fwrite(samples.data(), 2, samples.size(), f);
    std::fclose(f);
}

bool save_ppm(const char* path, const uint32_t* pixels)
{
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", g_out_width * g_scale, Vdp::kScreenHeight);
    for (int i = 0; i < g_out_width * g_scale * Vdp::kScreenHeight; i++) {
        uint8_t rgb[3] = {uint8_t(pixels[i] >> 16), uint8_t(pixels[i] >> 8), uint8_t(pixels[i])};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

// Game controllers, through SDL's gamepad API (XInput, DirectInput and the newer Windows
// input APIs are all handled by SDL). Buttons are named by position: on an Xbox pad
// South is A, East is B, West is X, and Start is the menu button.
std::vector<SDL_Gamepad*> g_gamepads;

void open_gamepad(SDL_JoystickID id)
{
    if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) {
        g_gamepads.push_back(pad);
        std::printf("Controller connected: %s\n", SDL_GetGamepadName(pad));
    }
}

void close_gamepad(SDL_JoystickID id)
{
    for (size_t i = 0; i < g_gamepads.size(); i++)
        if (SDL_GetGamepadID(g_gamepads[i]) == id) {
            std::printf("Controller disconnected: %s\n", SDL_GetGamepadName(g_gamepads[i]));
            SDL_CloseGamepad(g_gamepads[i]);
            g_gamepads.erase(g_gamepads.begin() + i);
            return;
        }
}

uint8_t gamepad_buttons()
{
    constexpr Sint16 kStickThreshold = 13000;       // about 40% of full travel
    uint8_t b = 0;
    for (SDL_Gamepad* pad : g_gamepads) {
        auto held = [pad](SDL_GamepadButton button) { return SDL_GetGamepadButton(pad, button); };
        Sint16 x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        Sint16 y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (held(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -kStickThreshold) b |= BTN_UP;
        if (held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > kStickThreshold) b |= BTN_DOWN;
        if (held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -kStickThreshold) b |= BTN_LEFT;
        if (held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > kStickThreshold) b |= BTN_RIGHT;
        if (held(SDL_GAMEPAD_BUTTON_WEST)) b |= BTN_A;      // Xbox X
        if (held(SDL_GAMEPAD_BUTTON_SOUTH)) b |= BTN_B;     // Xbox A
        if (held(SDL_GAMEPAD_BUTTON_EAST)) b |= BTN_C;      // Xbox B
        if (held(SDL_GAMEPAD_BUTTON_START)) b |= BTN_START; // Xbox menu
    }
    return b;
}

uint8_t keyboard_buttons()
{
    const bool* keys = SDL_GetKeyboardState(nullptr);
    uint8_t b = 0;
    if (keys[SDL_SCANCODE_UP]) b |= BTN_UP;
    if (keys[SDL_SCANCODE_DOWN]) b |= BTN_DOWN;
    if (keys[SDL_SCANCODE_LEFT]) b |= BTN_LEFT;
    if (keys[SDL_SCANCODE_RIGHT]) b |= BTN_RIGHT;
    if (keys[SDL_SCANCODE_Z]) b |= BTN_A;
    if (keys[SDL_SCANCODE_X]) b |= BTN_B;
    if (keys[SDL_SCANCODE_C]) b |= BTN_C;
    if (keys[SDL_SCANCODE_RETURN]) b |= BTN_START;
    return b;
}

}  // namespace

// Musashi memory and interrupt callbacks.
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return g_bus.read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return g_bus.read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return g_bus.read16(a) << 16 | g_bus.read16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { g_bus.write8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { g_bus.write16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { g_bus.write16(a, v >> 16); g_bus.write16(a + 2, v & 0xFFFF); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return g_bus.read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return g_bus.read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

int genesis_int_ack(int level)
{
    g_irq_pending &= ~(1 << level);
    update_irq();
    return M68K_INT_ACK_AUTOVECTOR;
}
}

// On Windows the port is built as a GUI program, so no terminal window opens with it. With
// --console it shows one for its messages: the terminal it was started from, or a new window.
// Output that is already redirected (to a file or a pipe) is left alone.
void show_console()
{
#ifdef _WIN32
    // Checked before attaching, which sets the standard handles to the console.
    auto unset = [](DWORD which) {
        HANDLE h = GetStdHandle(which);
        return h == nullptr || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN;
    };
    const bool out_unset = unset(STD_OUTPUT_HANDLE), err_unset = unset(STD_ERROR_HANDLE);
    if (!out_unset && !err_unset)
        return;
    const bool attached = AttachConsole(ATTACH_PARENT_PROCESS);
    if (!attached && !AllocConsole())
        return;
    // Unbuffered, so messages show as they are printed rather than when the game exits.
    if (out_unset && std::freopen("CONOUT$", "w", stdout))
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (err_unset && std::freopen("CONOUT$", "w", stderr))
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    if (attached)                                   // the shell's prompt has already returned
        std::printf("\n");
#endif
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], "--console") == 0)
            show_console();
    std::string rom_path, dump_path, wav_path, ram_path, vram_path, cram_path;
    int headless_frames = 0;
    std::vector<Press> presses;
    std::vector<uint32_t> peeks;                    // RAM words to print after a headless run
    bool log_state = false;                         // print each GameState change with its frame number
    bool show_state = false;                        // show the GameState in the window title
    bool log_scroll = false;                        // print each frame's plane A/B scroll (line 0)
    std::vector<uint32_t> logged_bytes;             // RAM bytes to print with the frame number when they change
    struct Poke { int frame; uint32_t address; uint16_t value; };
    std::vector<Poke> pokes;                        // RAM words to write at the start of a frame (testing)
    std::vector<int> switch_frames, reset_frames;   // menu actions to run headlessly (testing)
    std::vector<int> swing_switch_frames;
    std::vector<std::pair<int, int>> stage_jumps;   // frame, stage (1-28)
    // This run only, over the saved settings.
    bool no_widescreen = false, easy_bounce = false, smooth_swing = false, smooth_scroll = false, crt = false;
    bool no_smooth_scroll = false, vanilla_rock_edges = false, vanilla_fades = false, show_fps = false, interpolate = false;
    bool no_interpolate = false, log_mdplus = false;
    int mdplus = -1;                                // --mdplus / --no-mdplus
    double interp_back = -1;                        // --interp-back (testing)
    double refresh_override = 0;                    // --refresh (testing)
    int frame_rate = -1;                            // --frame-rate
    std::vector<std::string> cheats;                // --cheat NAME (this run only)
    int scanlines = -1;                             // percent, -1 = the saved setting
    int hqx = -1;                                   // scale, -1 = the saved setting
    int capture_frame = -1;                         // windowed test: save the picture at this frame
    std::string capture_path;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) headless_frames = std::atoi(argv[++i]);
        else if (arg == "--dump" && i + 1 < argc) dump_path = argv[++i];
        else if (arg == "--trace-vdp") g_vdp.trace = true;
        else if (arg == "--wav" && i + 1 < argc) wav_path = argv[++i];
        else if (arg == "--no-widescreen") no_widescreen = true;
        else if (arg == "--easy-bounce") easy_bounce = true;
        else if (arg == "--smooth-swing") smooth_swing = true;
        else if (arg == "--smooth-scroll") smooth_scroll = true;
        else if (arg == "--no-smooth-scroll") no_smooth_scroll = true;
        else if (arg == "--vanilla-rock-edges") vanilla_rock_edges = true;
        else if (arg == "--vanilla-fades") vanilla_fades = true;
        else if (arg == "--show-fps") show_fps = true;
        else if (arg == "--interpolate") interpolate = true;
        else if (arg == "--no-interpolate") no_interpolate = true;
        else if (arg == "--mdplus") mdplus = 1;
        else if (arg == "--no-mdplus") mdplus = 0;
        else if (arg == "--log-mdplus") log_mdplus = true;
        else if (arg == "--interp-back" && i + 1 < argc) interp_back = std::atof(argv[++i]);
        else if (arg == "--refresh" && i + 1 < argc) refresh_override = std::atof(argv[++i]);
        else if (arg == "--frame-rate" && i + 1 < argc) frame_rate = std::atoi(argv[++i]);
        else if (arg == "--cheat" && i + 1 < argc) cheats.push_back(argv[++i]);
        else if (arg == "--crt") crt = true;
        else if (arg == "--console") {}           // handled first thing in main
        else if (arg == "--scanlines" && i + 1 < argc) scanlines = std::atoi(argv[++i]);
        else if (arg == "--hqx" && i + 1 < argc) hqx = std::atoi(argv[++i]);
        else if (arg == "--capture" && i + 2 < argc) {
            capture_frame = std::atoi(argv[++i]);
            capture_path = argv[++i];
        }
        else if (arg == "--peek" && i + 1 < argc) peeks.push_back(std::strtoul(argv[++i], nullptr, 16));
        else if (arg == "--ram" && i + 1 < argc) ram_path = argv[++i];
        else if (arg == "--vram" && i + 1 < argc) vram_path = argv[++i];
        else if (arg == "--cram" && i + 1 < argc) cram_path = argv[++i];
        else if (arg == "--log-state") log_state = true;
        else if (arg == "--show-state") show_state = true;
        else if (arg == "--log-scroll") log_scroll = true;
        else if (arg == "--log-ram" && i + 1 < argc) logged_bytes.push_back(std::strtoul(argv[++i], nullptr, 16) & 0xFFFF);
        else if (arg == "--watch" && i + 1 < argc) g_bus.watches.push_back(std::strtoul(argv[++i], nullptr, 16) & 0xFFFF);
        else if (arg == "--stage" && i + 1 < argc) {
            // Stages are numbered 1-28 in play order; the game's own index ($FFFF42) differs.
            int stage = std::atoi(argv[++i]);
            if (stage < 1 || stage > kStageCount) {
                std::fprintf(stderr, "--stage must be 1-%d\n", kStageCount);
                return 1;
            }
            g_bus.start_stage = kStageIndex[stage - 1];
        }
        else if (arg == "--no-sprite-masking") g_vdp.sprite_masking = false;
        else if (arg == "--switch-widescreen" && i + 1 < argc) switch_frames.push_back(std::atoi(argv[++i]));
        else if (arg == "--reset" && i + 1 < argc) reset_frames.push_back(std::atoi(argv[++i]));
        else if (arg == "--switch-smooth-swing" && i + 1 < argc) swing_switch_frames.push_back(std::atoi(argv[++i]));
        else if (arg == "--go-to-stage" && i + 1 < argc) {
            std::string spec = argv[++i];
            size_t colon = spec.find(':');
            int stage = colon == std::string::npos ? 0 : std::atoi(spec.substr(colon + 1).c_str());
            if (stage < 1 || stage > kStageCount) {
                std::fprintf(stderr, "bad --go-to-stage value (FRAME:STAGE, stage 1-%d): %s\n", kStageCount, spec.c_str());
                return 1;
            }
            stage_jumps.push_back({std::atoi(spec.c_str()), stage});
        }
        else if ((arg == "--poke" || arg == "--poke-file") && i + 1 < argc) {
            // FRAME:ADDRESS=VALUE, e.g. 1200:FFDC10=0 (address and value in hex); --poke-file
            // reads one per line, for more than fit on a command line.
            std::vector<std::string> specs;
            if (arg == "--poke") {
                specs.push_back(argv[++i]);
            } else {
                std::ifstream file(argv[++i]);
                for (std::string line; std::getline(file, line);)
                    if (!line.empty())
                        specs.push_back(line);
            }
            for (const std::string& spec : specs) {
                size_t colon = spec.find(':'), equals = spec.find('=');
                if (colon == std::string::npos || equals == std::string::npos) {
                    std::fprintf(stderr, "bad poke: %s\n", spec.c_str());
                    return 1;
                }
                pokes.push_back({std::atoi(spec.substr(0, colon).c_str()),
                                 (uint32_t)std::strtoul(spec.substr(colon + 1, equals - colon - 1).c_str(), nullptr, 16),
                                 (uint16_t)std::strtoul(spec.substr(equals + 1).c_str(), nullptr, 16)});
            }
        }
        else if (arg == "--press" && i + 1 < argc) {
            Press p;
            if (!parse_press(argv[++i], p)) {
                std::fprintf(stderr, "bad --press value: %s\n", argv[i]);
                return 1;
            }
            presses.push_back(p);
        } else rom_path = arg;
    }

    g_rom_original = load_rom(rom_path);
    if (g_rom_original.empty()) {
        std::fprintf(stderr, "No 512 KB ROM found (pass a path, or put it in rom/).\n");
        if (headless_frames == 0)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Castle of Illusion",
                                     "No ROM found.\n\nPut your own dump of the US Genesis release (a 512 KB .bin file) "
                                     "in the rom folder beside coi.exe, then start it again.", nullptr);
        return 1;
    }
    g_rom_cartridge = g_rom_original;
    // An MD+ pack in mdplus/: its patched ROM must be this game with the pack's changes only.
    for (const auto& dir : search_dirs("mdplus")) {
        if (!std::filesystem::is_directory(dir))
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() != ".cue" || g_mdplus.loaded())
                continue;
            std::string error;
            if (!g_mdplus.load(entry.path(), error)) {
                std::fprintf(stderr, "MD+ pack not used: %s\n", error.c_str());
                continue;
            }
            std::ifstream file(g_mdplus.rom_path(), std::ios::binary);
            g_rom_mdplus.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            size_t differ = 0;
            for (size_t i = 0x200; i < g_rom_cartridge.size() && i < g_rom_mdplus.size(); i++)
                differ += g_rom_cartridge[i] != g_rom_mdplus[i];
            if (g_rom_mdplus.size() < g_rom_cartridge.size() || differ > 4096) {
                std::fprintf(stderr, "MD+ pack not used: %s is not a patch of this ROM\n",
                             g_mdplus.rom_path().filename().string().c_str());
                g_mdplus = MdPlus();
                g_rom_mdplus.clear();
                continue;
            }
            std::printf("MD+ pack: %s\n", g_mdplus.name().c_str());
        }
    }
    {
        // Try each patch set on its own copy, to know which the menu can offer.
        std::vector<uint8_t> trial = g_rom_original;
        std::string error = apply_widescreen_patches(trial);
        g_wide_available = error.empty();
        if (!g_wide_available)
            std::fprintf(stderr, "Widescreen unavailable: %s\n", error.c_str());
        trial = g_rom_original;
        error = apply_smooth_swing(trial);
        g_smooth_swing_available = error.empty();
        if (!g_smooth_swing_available)
            std::fprintf(stderr, "Smooth swinging unavailable: %s\n", error.c_str());
        // The pack's ROM must take the same patches.
        if (g_mdplus.loaded()) {
            trial = g_rom_mdplus;
            const std::string wide = apply_widescreen_patches(trial);
            trial = g_rom_mdplus;
            const std::string swing = apply_smooth_swing(trial);
            if ((g_wide_available && !wide.empty()) || (g_smooth_swing_available && !swing.empty())) {
                std::fprintf(stderr, "MD+ pack not used: the port's patches don't fit its ROM\n");
                g_mdplus = MdPlus();
                g_rom_mdplus.clear();
            }
        }
    }
    Settings settings;                              // defaults for headless runs, so tests repeat
    if (headless_frames == 0)
        settings = load_settings();
    g_easy_bounce = settings.easy_bounce || easy_bounce;
    if (vanilla_rock_edges)
        settings.clean_rock_edges = false;
    if (vanilla_fades)
        settings.smooth_fades = false;
    if (show_fps)
        settings.show_fps = true;
    if (interpolate)
        settings.interpolate = true;
    if (no_interpolate)
        settings.interpolate = false;
    if (mdplus >= 0)
        settings.mdplus = mdplus;
    // MD+: the pack's patched ROM runs instead of the cartridge's, with the port's patches too.
    g_mdplus_on = settings.mdplus && g_mdplus.loaded();
    g_rom_original = g_mdplus_on ? g_rom_mdplus : g_rom_cartridge;
    g_bus.on_cartridge_write = [](uint32_t address, uint16_t value) {
        if (g_mdplus_on)
            g_mdplus.write(address, value);
    };
    if (log_mdplus)
        g_mdplus.on_command = [](uint16_t command) {
            std::printf("MD+ command $%04X (state $%02X)\n", command, g_bus.ram[0xF301]);
        };
    if (frame_rate >= 0)
        settings.frame_rate = frame_rate;
    for (const std::string& name : cheats) {
        bool Cheats::*field = name == "invincibility" ? &Cheats::invincibility
                            : name == "infinite-power" ? &Cheats::infinite_power
                            : name == "infinite-tries" ? &Cheats::infinite_tries
                            : name == "infinite-items" ? &Cheats::infinite_items
                            : name == "infinite-continues" ? &Cheats::infinite_continues
                            : name == "full-power" ? &Cheats::full_power
                            : name == "walk-faster" ? &Cheats::walk_faster
                            : name == "moon-jump" ? &Cheats::moon_jump
                            : name == "midair-jump" ? &Cheats::midair_jump : nullptr;
        if (field)
            settings.cheats.*field = true;
        else
            std::fprintf(stderr, "Unknown cheat: %s\n", name.c_str());
    }
    g_cheats = settings.cheats;
    g_clean_rock_edges = settings.clean_rock_edges;
    g_smooth_fades = settings.smooth_fades;
    g_smooth_swing = (settings.smooth_swing || smooth_swing) && g_smooth_swing_available;
    bool smooth_scroll_on = (settings.smooth_scroll || smooth_scroll) && !no_smooth_scroll;   // chosen
    set_smooth_scroll(smooth_scroll_on);
    g_vdp.smooth_scroll = [](int line, int hs_a, int hs_b, int scale, int& sub_a, int& sub_b) {
        g_scroll_smoother.line(line, hs_a, hs_b, scale, sub_a, sub_b);
    };
    set_widescreen(settings.widescreen && !no_widescreen && g_wide_available);
    g_bus.vdp = &g_vdp;
    g_bus.sound = &g_sound;
    g_vdp.extend_plane_a = extend_plane_a;
    g_vdp.edge_tiles = {0x123, 0x124, 0x125, 0x12A, 0x12B, 0x130, 0x1B7};   // the forest's rock edge tiles (Clean Rock Edges)
    // Frame interpolation notes who writes each sprite and when the list reaches the VDP.
    g_bus.on_sprite_list_write = [](uint16_t address) {
        if (g_interp_capture)
            g_interp.sprite_written(address, m68k_get_reg(nullptr, M68K_REG_PPC),
                                    m68k_get_reg(nullptr, M68K_REG_A0), g_bus.ram);
    };
    g_vdp.on_dma = [](uint32_t source) {
        if (g_interp_capture)
            g_interp.dma(source);
    };
    g_vdp.sprite_moves_with_b = [](uint16_t attr) {
        for (uint16_t first : g_b_sprite_tiles)
            if ((uint16_t)((attr & 0x67FF) - first) < 32)
                return true;
        return false;
    };

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();

    std::vector<uint32_t> pixels(kWideWidth * kSubpixels * Vdp::kScreenHeight);   // room for any width
    for (auto& layer : g_layers)
        layer.resize(kLayerStride * Vdp::kScreenHeight);

    if (headless_frames > 0) {
        std::vector<int16_t> all_audio;
        g_interp_capture = interp_back >= 0;
        for (int frame = 0; frame < headless_frames; frame++) {
            g_bus.buttons = easy_bounce_buttons();
            for (const Press& p : presses)
                if (frame >= p.first && frame <= p.last)
                    g_bus.buttons |= p.button;
            for (const Poke& p : pokes)
                if (frame == p.frame)
                    g_bus.write16(p.address, p.value);
            for (int f : switch_frames)
                if (frame == f)
                    set_widescreen(!g_widescreen);
            for (int f : reset_frames)
                if (frame == f)
                    power_on();
            for (int f : swing_switch_frames)
                if (frame == f)
                    set_smooth_swing(!g_smooth_swing);
            for (const auto& [f, stage] : stage_jumps)
                if (frame == f)
                    g_bus.jump_stage = kStageIndex[stage - 1];
            if (g_vdp.trace)
                std::printf("[frame %d]\n", frame);
            run_frame(pixels.data());
            if (log_scroll) {
                int hs = (g_vdp.reg[13] & 0x3F) << 10;
                auto word = [](const uint8_t* p) { return p[0] << 8 | p[1]; };
                std::printf("frame %5d: state $%02X hscroll A %4d B %4d  vscroll A %4d B %4d\n", frame,
                            g_bus.ram[0xF301], word(&g_vdp.vram[hs]) & 0x3FF, word(&g_vdp.vram[hs + 2]) & 0x3FF,
                            g_vdp.vsram[0], g_vdp.vsram[1]);
            }
            for (size_t b = 0; b < logged_bytes.size(); b++) {
                static std::vector<int> last;
                last.resize(logged_bytes.size(), -1);
                const int value = g_bus.ram[logged_bytes[b]];
                if (value != last[b])
                    std::printf("frame %5d: $FF%04X = $%02X\n", frame, logged_bytes[b], value);
                last[b] = value;
            }
            if (log_state) {
                static int last = -1;
                int state = g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301];
                if (state != last)
                    std::printf("frame %5d: GameState $%02X\n", frame, state);
                last = state;
            }
            if (!wav_path.empty())
                all_audio.insert(all_audio.end(), g_audio.begin(), g_audio.end());
        }
        if (!wav_path.empty())
            write_wav(wav_path.c_str(), all_audio, g_sound.sample_rate());
        std::printf("YM2612 timer expirations: A=%u B=%u\n", g_sound.timer_expirations[0],
                    g_sound.timer_expirations[1]);
        std::printf("Ran %d frames; PC=%06X GameState=$%02X VIntMode=$%02X\n", headless_frames,
                    m68k_get_reg(nullptr, M68K_REG_PC), g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301],
                    g_bus.ram[0xF302] << 8 | g_bus.ram[0xF303]);
        if (!ram_path.empty())
            if (FILE* f = std::fopen(ram_path.c_str(), "wb")) {
                std::fwrite(g_bus.ram, 1, sizeof(g_bus.ram), f);
                std::fclose(f);
            }
        if (!vram_path.empty())
            if (FILE* f = std::fopen(vram_path.c_str(), "wb")) {
                std::fwrite(g_vdp.vram, 1, sizeof(g_vdp.vram), f);
                std::fclose(f);
            }
        if (!cram_path.empty())
            if (FILE* f = std::fopen(cram_path.c_str(), "wb")) {   // CRAM (64 words, big-endian), the 32 registers, VSRAM (40 words)
                for (uint16_t c : g_vdp.cram) {
                    std::fputc(c >> 8, f);
                    std::fputc(c & 0xFF, f);
                }
                std::fwrite(g_vdp.reg, 1, sizeof(g_vdp.reg), f);
                for (uint16_t v : g_vdp.vsram) {
                    std::fputc(v >> 8, f);
                    std::fputc(v & 0xFF, f);
                }
                std::fclose(f);
            }
        for (uint32_t a : peeks)
            std::printf("RAM $%06X = %d ($%04X)\n", a, (int16_t)(g_bus.ram[a & 0xFFFF] << 8 | g_bus.ram[(a + 1) & 0xFFFF]),
                        g_bus.ram[a & 0xFFFF] << 8 | g_bus.ram[(a + 1) & 0xFFFF]);
        std::printf("Frame %dx%d, widescreen %s, CameraA X=%d Y=%d\n", g_out_width, Vdp::kScreenHeight,
                    g_widescreen ? "on" : "off", (int16_t)(g_bus.ram[0xFA84] << 8 | g_bus.ram[0xFA85]),
                    (int16_t)(g_bus.ram[0xFA80] << 8 | g_bus.ram[0xFA81]));
        std::printf("VDP registers:");
        for (int r = 0; r < 24; r++)
            std::printf(" %02X", g_vdp.reg[r]);
        std::printf("\n");
        std::printf("VDP reg0=$%02X reg1=$%02X  irq pending=$%02X  SR=$%04X  RAM FF08=$%04X FF0A=$%04X\n",
                    g_vdp.reg[0], g_vdp.reg[1], g_irq_pending, m68k_get_reg(nullptr, M68K_REG_SR),
                    g_bus.ram[0xFF08] << 8 | g_bus.ram[0xFF09], g_bus.ram[0xFF0A] << 8 | g_bus.ram[0xFF0B]);
        if (interp_back >= 0 && g_interp.ready())   // --interp-back: the last frame, drawn earlier
            render_interpolated(pixels.data(), interp_back);
        if (!dump_path.empty() && !save_ppm(dump_path.c_str(), pixels.data())) {
            std::fprintf(stderr, "Could not write %s\n", dump_path.c_str());
            return 1;
        }
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    // OpenGL 3.3 core: the display draws the frame (through the CRT shader when it is on)
    // and the menu draws over it at the window's own resolution.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* window = SDL_CreateWindow("Castle of Illusion", g_out_width * 3, Vdp::kScreenHeight * 3,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
                                              | (settings.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    SDL_GLContext gl = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!gl) {
        std::fprintf(stderr, "Could not create the window with OpenGL 3.3: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(0);                      // set by the main loop (V-Sync)
    Display display;
    if (!display.init(window)) {
        std::fprintf(stderr, "Could not set up the display.\n");
        return 1;
    }
    if (crt)
        settings.crt.enabled = true;
    if (scanlines >= 0) {
        settings.scanlines = scanlines;
        settings.crt.enabled = false;               // the scanlines show only without the CRT shader
    }
    if (hqx >= 0) {
        settings.hqx = hqx;
        settings.crt.enabled = false;               // as is hqx
    }
    // hqx needs whole pixels, so with smooth scrolling the frame is drawn as layers instead.
    auto hqx_on = [&] {
        return settings.hqx >= 2 && settings.hqx <= 4 && !settings.crt.enabled && display.hqx_available();
    };
    menu_init(window, gl);

    SDL_AudioSpec audio_spec = {SDL_AUDIO_S16, 2, g_sound.sample_rate()};
    // The game's sound and the MD+ music (mdplus.cpp) are two streams on one device, which
    // mixes them.
    const SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    SDL_AudioStream* audio = nullptr;
    SDL_AudioStream* music = nullptr;
    if (audio_device) {
        const SDL_AudioSpec music_spec = {SDL_AUDIO_S16, 2, 44100};
        audio = SDL_CreateAudioStream(&audio_spec, nullptr);
        music = SDL_CreateAudioStream(&music_spec, nullptr);
        if (audio && !SDL_BindAudioStream(audio_device, audio)) {
            SDL_DestroyAudioStream(audio);
            audio = nullptr;
        }
        if (music && !SDL_BindAudioStream(audio_device, music)) {
            SDL_DestroyAudioStream(music);
            music = nullptr;
        }
        SDL_ResumeAudioDevice(audio_device);
    }
    if (!audio)
        std::fprintf(stderr, "No audio: %s\n", SDL_GetError());
    const int max_queued = g_sound.sample_rate() * 4 / 5;   // drop audio beyond 200 ms of latency

    auto fullscreen = [window] { return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0; };
    auto update_cursor = [&] {
        // Hidden over the game in fullscreen, unless the menu is open.
        if (fullscreen() && !menu_open())
            SDL_HideCursor();
        else
            SDL_ShowCursor();
    };
    // Quick Settings: switches widescreen and the graphics enhancements together. Leaving
    // Custom keeps its settings for when it is picked again.
    auto apply_quick = [&](Quick quick) {
        if ((Quick)settings.quick == Quick::Custom)
            settings.custom = look_of(settings);
        set_look(settings, quick == Quick::Custom ? settings.custom : quick_look(quick));
        settings.quick = (int)quick;
        if (settings.widescreen != g_widescreen)
            set_widescreen(settings.widescreen);
        if (settings.smooth_swing != g_smooth_swing)
            set_smooth_swing(settings.smooth_swing);
        smooth_scroll_on = settings.smooth_scroll;
        save_settings(settings);
    };
    auto act = [&](MenuChoice choice, bool& running) {
        switch (choice.action) {
        case MenuAction::Reset:
            power_on();
            if (audio)
                SDL_ClearAudioStream(audio);
            std::printf("Reset\n");
            break;
        case MenuAction::Exit:
            running = false;
            break;
        case MenuAction::ToggleWidescreen:
            set_widescreen(!g_widescreen);
            settings.widescreen = g_widescreen;
            save_settings(settings);
            break;
        case MenuAction::OpenSettingsFolder:
            if (const std::string dir = settings_folder(); !dir.empty() && !SDL_OpenURL(("file:///" + dir).c_str()))
                std::fprintf(stderr, "Could not open %s: %s\n", dir.c_str(), SDL_GetError());
            break;
        case MenuAction::ToggleMdPlus:
            // A different cartridge: the pack's patched ROM or the original, from a reset.
            g_mdplus_on = !g_mdplus_on && g_mdplus.loaded();
            settings.mdplus = g_mdplus_on;
            g_rom_original = g_mdplus_on ? g_rom_mdplus : g_rom_cartridge;
            build_rom();
            power_on();
            if (music)
                SDL_ClearAudioStream(music);
            if (audio)
                SDL_ClearAudioStream(audio);
            save_settings(settings);
            break;
        case MenuAction::ToggleSmoothSwing:
            set_smooth_swing(!g_smooth_swing);
            settings.smooth_swing = g_smooth_swing;
            save_settings(settings);
            break;
        case MenuAction::ToggleSmoothScroll:
            smooth_scroll_on = !smooth_scroll_on;
            settings.smooth_scroll = smooth_scroll_on;
            save_settings(settings);
            break;
        case MenuAction::ToggleEasyBounce:
            g_easy_bounce = !g_easy_bounce;
            settings.easy_bounce = g_easy_bounce;
            save_settings(settings);
            break;
        case MenuAction::QuickSettings:
            apply_quick((Quick)choice.quick);
            break;
        case MenuAction::GoToStage:
            g_bus.jump_stage = kStageIndex[choice.stage - 1];   // taken up at the top of the game's main loop
            break;
        case MenuAction::ToggleFullscreen:
            // No display mode set: SDL uses borderless fullscreen at the desktop resolution.
            settings.fullscreen = !fullscreen();
            SDL_SetWindowFullscreen(window, settings.fullscreen);
            save_settings(settings);
            break;
        case MenuAction::None:
            break;
        }
        if (choice.cheats_changed) {
            g_cheats = settings.cheats;
            build_rom();
            save_settings(settings);
        }
        if (choice.save_settings)
            save_settings(settings);
        update_cursor();
    };
    int frames_run = 0;                             // game frames since start (--press, --capture)

    // Pacing. Without V-Sync a timer runs one game frame per 1/59.92 s. With V-Sync each
    // swap waits for the display's refresh: on a display at about 60 Hz the game runs one frame
    // per refresh (a fraction faster or slower than the console; the audio is resampled to
    // match), and at other rates game frames come due by time and each refresh shows the
    // latest one.
    const double native_hz = 1.0 / kFrameSeconds;
    bool vsync_on = false;
    double owed = 0;                                // game frames due but not yet run (V-Sync)
    float audio_ratio = 1.0f;
    int refreshes_per_frame = 1, refresh_phase = 0;   // locked to a multiple of 60 Hz
    int fixed_rate = 0;                             // pictures per game frame at a chosen frame rate
    Uint64 last_tick = SDL_GetTicksNS(), next_frame = last_tick;
    // FPS counter: pictures shown and game frames run over the last half second.
    int fps_shown = 0, fps_run = 0;
    Uint64 fps_since = last_tick;
    float fps = 0, game_fps = 0;

    update_cursor();
    bool running = true;
    while (running) {
        if (settings.vsync != vsync_on) {
            vsync_on = settings.vsync && SDL_GL_SetSwapInterval(1);
            if (!vsync_on)
                SDL_GL_SetSwapInterval(0);
            owed = 0;
            next_frame = last_tick = SDL_GetTicksNS();
        }
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        const double refresh = refresh_override > 0 ? refresh_override : mode ? mode->refresh_rate : 0.0;
        // On a display at a whole multiple of 60 Hz (60, 120, 180, 240) each game frame is
        // shown for exactly that many refreshes, counted by refresh rather than by the clock,
        // so every picture lasts the same time (the game runs a fraction faster or slower than
        // the console to match; the audio is resampled the same way).
        const int multiple = refresh > 0 ? (int)std::lround(refresh / native_hz) : 0;
        // Only while the swaps really wait for the refresh: shown faster than it, V-Sync isn't
        // working (some drivers ignore it), and counting refreshes would run the game too fast.
        const bool locked = vsync_on && multiple >= 1 && std::fabs(refresh - multiple * native_hz) < multiple
                            && !(fps > refresh * 1.03);
        // Or a chosen frame rate (Video > Frame Rate: 60, 120 or 240, for variable-refresh
        // displays such as G-Sync or FreeSync ones): pictures are timed by the port at that
        // multiple of the console's 59.92 a second, whatever the display reports, and the game
        // runs at exactly the console's speed.
        const int fixed = settings.frame_rate == 60 || settings.frame_rate == 120 || settings.frame_rate == 240
                              ? settings.frame_rate / 60 : 0;
        const int per_frame = fixed ? fixed : locked ? multiple : 1;
        const bool counted = fixed || locked;      // game frames follow the pictures shown
        // High Refresh Interpolation: at 120 and up those pictures show evenly spaced
        // moments between the last two game frames instead of repeats (interp.cpp).
        const bool interpolating = settings.interpolate && per_frame >= 2;
        if (interpolating != g_interp_capture) {
            g_interp_capture = interpolating;
            g_interp.reset();
        }
        if (per_frame != refreshes_per_frame || fixed != fixed_rate) {
            refreshes_per_frame = per_frame;
            fixed_rate = fixed;
            next_frame = SDL_GetTicksNS();
            refresh_phase = 0;
            owed = 0;
            last_tick = SDL_GetTicksNS();
        }
        int due = 1;
        double back = 0;                            // how far behind the current game frame to draw
        if (counted) {
            // At a chosen frame rate, pictures that can't be drawn in time (too much work for
            // the rate) are skipped, still counting their game frames, so the game keeps its
            // speed and only pictures are lost.
            int steps = 1;
            if (fixed) {
                const Uint64 now = SDL_GetTicksNS(), period_ns = (Uint64)(kFrameSeconds / fixed * 1e9);
                if (now >= next_frame + period_ns) {
                    const Uint64 late = (now - next_frame) / period_ns;
                    if (late > 8)
                        next_frame = now;           // a stall: start the timing afresh
                    else
                        steps += (int)late, next_frame += late * period_ns;
                }
            }
            due = 0;
            for (int i = 0; i < steps; i++) {
                if (refresh_phase == 0)
                    due++;
                if (interpolating)
                    back = (double)(per_frame - 1 - refresh_phase) / per_frame;
                refresh_phase = (refresh_phase + 1) % per_frame;
            }
        } else if (vsync_on) {
            // Other rates: game frames come due by the clock (rounded, so steady refreshes
            // give steady counts) and each refresh shows the latest.
            const Uint64 now = SDL_GetTicksNS();
            owed += (now - last_tick) * 1e-9 * native_hz;
            last_tick = now;
            due = (int)(owed + 0.5);
            if (due > 3)
                due = 3, owed = 3;
            owed -= due;
        }
        const float ratio = locked && !fixed ? (float)(refresh / (per_frame * native_hz)) : 1.0f;
        if (audio && ratio != audio_ratio) {
            SDL_SetAudioStreamFrequencyRatio(audio, ratio);
            audio_ratio = ratio;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (menu_event(event))
                continue;
            if (event.type == SDL_EVENT_QUIT)
                running = false;
            else if (event.type == SDL_EVENT_GAMEPAD_ADDED)    // also sent for pads present at startup
                open_gamepad(event.gdevice.which);
            else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
                close_gamepad(event.gdevice.which);
            else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                menu_toggle(true);                  // Xbox View button
                update_cursor();
            } else if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
                update_cursor();
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                const SDL_Keycode key = event.key.key;
                const bool ctrl = event.key.mod & SDL_KMOD_CTRL, alt = event.key.mod & SDL_KMOD_ALT;
                if (key == SDLK_F1) {
                    menu_toggle();
                    update_cursor();
                } else if (key == SDLK_F11 || (alt && key == SDLK_RETURN)) {
                    act({MenuAction::ToggleFullscreen}, running);
                } else if (ctrl && key == SDLK_R) {
                    act({MenuAction::Reset}, running);
                } else if (key == SDLK_F2 || key == SDLK_F3 || key == SDLK_F4) {
                    act({MenuAction::QuickSettings, 0, (int)(key - SDLK_F2)}, running);
                } else if (key == SDLK_F12) {
                    settings.crt.enabled = !settings.crt.enabled;
                    save_settings(settings);
                }
            }
        }
        // Alt+Enter must not also press Start; the menu keeps its keys while it has focus.
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const bool keyboard_free = !menu_wants_keyboard() && !keys[SDL_SCANCODE_LALT] && !keys[SDL_SCANCODE_RALT];
        // Controllers drive the menu while it is open. After it closes, they reach the game
        // again once every button is up, so the A that picked an item doesn't also jump.
        static bool pad_held_over = false;
        uint8_t pad = gamepad_buttons();
        if (menu_open())
            pad_held_over = true;
        else if (pad == 0)
            pad_held_over = false;
        if (pad_held_over)
            pad = 0;
        act(menu_frame({g_widescreen, g_wide_available, fullscreen(), current_stage(), g_easy_bounce,
                        g_smooth_swing, g_smooth_swing_available, smooth_scroll_on, &settings.crt,
                        &settings.scanlines, &settings.hqx, &settings.clean_rock_edges, &settings.smooth_fades,
                        &settings.remove_sprite_limit, &settings.vsync, &settings.show_fps, fps, game_fps,
                        &settings.cheats, &settings.interpolate, refresh, &settings.frame_rate,
                        g_mdplus_on, g_mdplus.loaded() ? g_mdplus.name().c_str() : nullptr, settings.quick}),
            running);
        if ((Quick)settings.quick != Quick::Custom && look_of(settings) != quick_look((Quick)settings.quick)) {
            settings.quick = (int)Quick::Custom;    // one of its settings was changed by hand
            save_settings(settings);
        }
        g_clean_rock_edges = settings.clean_rock_edges;
        g_smooth_fades = settings.smooth_fades;
        g_vdp.sprite_limit = !settings.remove_sprite_limit;
        if ((smooth_scroll_on && !hqx_on()) != (g_scale > 1))
            set_smooth_scroll(smooth_scroll_on && !hqx_on());
        if ((smooth_scroll_on && hqx_on()) != g_layered) {
            g_layered = !g_layered;
            g_scroll_smoother.reset();
        }

        const uint8_t buttons = (keyboard_free ? keyboard_buttons() : 0) | pad;
        for (int i = 0; i < due && running; i++) {
            g_bus.buttons = buttons | easy_bounce_buttons();
            for (const Press& p : presses)          // --press works in a window too
                if (frames_run >= p.first && frames_run <= p.last)
                    g_bus.buttons |= p.button;
            for (const auto& [f, stage] : stage_jumps)  // --go-to-stage works in a window too
                if (frames_run == f)
                    g_bus.jump_stage = kStageIndex[stage - 1];
            run_frame(pixels.data());
            frames_run++;
            fps_run++;
            if (show_state) {
                static int shown = -1;
                int state = g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301];
                if (state != shown) {
                    char title[64];
                    std::snprintf(title, sizeof(title), "Castle of Illusion - GameState $%02X", state);
                    SDL_SetWindowTitle(window, title);
                    shown = state;
                }
            }
            if (audio) {
                if (SDL_GetAudioStreamQueued(audio) > max_queued)
                    SDL_ClearAudioStream(audio);
                SDL_PutAudioStreamData(audio, g_audio.data(), (int)(g_audio.size() * sizeof(int16_t)));
            }
        }
        g_mdplus.feed(music);
        if (interpolating && g_interp.ready())
            render_interpolated(pixels.data(), back);
        if (g_layered) {
            LayeredFrame frame;
            for (int i = 0; i < LayeredFrame::kLayers; i++)
                frame.layers[i] = g_layers[i].data();
            frame.width = g_out_width + 2 * kLayerMargin;
            frame.height = Vdp::kScreenHeight;
            frame.margin = kLayerMargin;
            frame.clip_left = g_layer_clip_left;
            frame.clip_right = g_layer_clip_right;
            frame.shifts = g_line_shifts;
            frame.backdrop = g_line_backdrop;
            display.draw_layers(frame, settings.scanlines, settings.hqx);
        } else {
            display.draw(pixels.data(), g_out_width * g_scale, Vdp::kScreenHeight, g_out_width, settings.crt,
                         settings.scanlines, settings.hqx);
        }
        menu_render();
        if (capture_frame >= 0 && frames_run >= capture_frame) {
            std::vector<uint8_t> rgb;
            int w, h;
            display.read_window(rgb, w, h);
            if (FILE* f = std::fopen(capture_path.c_str(), "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", w, h);
                std::fwrite(rgb.data(), 1, rgb.size(), f);
                std::fclose(f);
            }
            running = false;
        }
        SDL_GL_SwapWindow(window);

        fps_shown++;
        const Uint64 now = SDL_GetTicksNS();
        if (now - fps_since >= 500000000) {
            fps = fps_shown * 1e9f / (now - fps_since);
            game_fps = fps_run * 1e9f / (now - fps_since);
            fps_shown = fps_run = 0;
            fps_since = now;
        }
        // At a chosen frame rate, wait for the next picture's time. Otherwise, without V-Sync,
        // wait for the next game frame; with it the swap normally waits for the refresh, and in
        // case the driver ignores the request, pictures are never shown faster than the refresh
        // rate (a little above, so a working V-Sync is never held back).
        const double rate = refresh > 0 ? refresh : native_hz;
        const double period = fixed ? kFrameSeconds / fixed : vsync_on ? 1.0 / (rate * 1.05) : kFrameSeconds;
        next_frame += (Uint64)(period * 1e9);
        if (next_frame > now)
            SDL_DelayNS(next_frame - now);
        else if (now - next_frame > (Uint64)(4 * period * 1e9))
            next_frame = now;                       // far behind (a stall): start the timing afresh
        // (a little behind: no wait, so the next pictures catch up and the pace holds)
    }

    menu_shutdown();
    display.shutdown();
    if (audio)
        SDL_DestroyAudioStream(audio);
    if (music)
        SDL_DestroyAudioStream(music);
    if (audio_device)
        SDL_CloseAudioDevice(audio_device);
    for (SDL_Gamepad* pad : g_gamepads)
        SDL_CloseGamepad(pad);
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
