// Castle of Illusion PC port: runs the original 68000 code on Musashi, with a
// C++ VDP and an SDL3 front end.
//
// Usage: coi [rom.bin] [--stage N] [--frames N] [--dump out.ppm] [--press BUTTON@FIRST-LAST ...]
//   --stage N    start the first level of a new game at stage N (1-28), see docs/stages.md
//   --show-state show the GameState in the window title
//   --no-widescreen start at the original 320 x 224 (the F1 menu can switch it later)
//   --no-smooth-swing start with smooth swinging off (whatever the saved settings say)
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
//   --hqx N      start with hqx at scale N (2-4, 1 = off; 5 = cleanEdge) and the CRT shader off
//   --clean-edge-width W  start with cleanEdge's line width at W (0.45-1.14)
// Menu settings are remembered between runs (settings.cpp); the options above apply to
// this run only. Headless runs ignore the saved settings.
// While running: F1 opens the menu, F11 or Alt+Enter toggles fullscreen, F12 the CRT shader,
// Ctrl+R resets, F2/F3/F4 pick the Quick Settings (Enhanced, Vanilla, Custom); holding
// Backspace (or a controller's Y) fast-forwards the story scenes 4x (kFastForward).
// Testing (headless): --poke FRAME:ADDR=VALUE, --poke-file FILE, --peek ADDR, --watch ADDR, --ram FILE,
//   --vram FILE, --cram FILE (CRAM, registers, VSRAM), --wav FILE, --log-state, --log-ram ADDR, --trace-vdp, --no-sprite-masking,
//   --capture FRAME FILE (windowed: save the window's picture after FRAME frames, then quit)
//   Debug builds: F9 saves the frame and the --ram/--vram/--cram files as snapshots/snapN.* beside
//   the exe (--snapshots DIR to save them elsewhere); --sprite-capture DIR (headless) saves the
//   VRAM and CRAM behind each sprite set the run shows (see capture_sprites)
//   --switch-widescreen FRAME, --switch-smooth-swing FRAME, --reset FRAME, --go-to-stage FRAME:STAGE (the menu's
//   actions, at the start of a frame)
//   --frames N   run N frames without a window, then exit (with --dump: save the last frame);
//                strictly N pictures, which are more game frames with --fast-forward
//   --fast-forward FIRST-LAST  hold fast forward over pictures FIRST-LAST (also windowed)
//   --state-hash FILE  (headless) write each game frame's number, picture and machine hash
//   --show-touch show the touch controls on a desktop too (windowed)
//   --interp-back X  with --dump: draw the last frame as interpolation would X frames earlier (0-1)
//   --refresh HZ treat the display as running at HZ (windowed; for testing the pacing)
//   --no-native  run the game on the interpreter only (builds with the translator, COI_JIT, run
//                its translation of the ROM by default; docs/native.md)
//   --press      hold a button (up/down/left/right/a/b/c/start) for a range of frames (also windowed)

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef __ANDROID__
#include "android_pacing.h"
#include "android_mdplus.h"
#include "android_rom.h"
#include <GLES3/gl3.h>
#include <android/log.h>
#include <thread>
#include <unistd.h>
#endif
#include "touch.h"
#include "icon_data.h"
#ifdef COI_NATIVE
#include "native_rt.h"
#ifdef COI_NATIVE
#include "program.h"
#include "arm64.h"
#include "jit.h"
#include "recomp_lists.h"
#include "x64.h"

#include <future>
#endif
#include <map>
#endif

#include <algorithm>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "boss_rush.h"
#include "bus.h"
#include "cheats.h"
#include "display.h"
#include "fade.h"
#include "interp.h"
#include "mdplus.h"
#include "menu.h"
#include "patches.h"
#include "rom_family.h"
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
//   $10  stage 8's turn upside down (GameState_10): the picture of play squeezed to a line
//        and opened out again through the line interrupt's vertical scroll; the planes
//        are as wide as in play, so the strips squeeze with it
//   $22  the cutscenes between worlds (set up by $20, which picks the scene by level):
//        a scripted camera over 512-pixel planes that are drawn once. The castle
//        exterior before the final boss needs a fill past its brick tower (TowerFill).
//   $2E  the ending, in the final arena: same walls as the fight, drawn in front of the
//        sprites so the characters exit behind them (and Boss Rush's results screen).
//   $2C  the ending's setup, under a frame in the game; in a Boss Rush the results load
//        here for about 50 frames with the display off, showing Mizrabel's white across
//        the whole width rather than between black bars.
//   $16  the scenes after the ending (the flight past the castle at night, the cliff,
//        the forest credits): pictures drawn across the planes' full 512 pixels,
//        scrolled through the line scroll table with the cameras at 0, so the strips
//        show more of the same picture. "The End" (on black, framed for 320 pixels)
//        stays pillarboxed while its objects are loaded (after_ending_scene); the switches
//        fall on black frames. The theatre at the end repeats its own columns into the
//        strips (TheatreFill).
//   $2A  the story before the hallway (Mizrabel takes Minnie): its wall, trees and ground
//        repeat into the strips (StoryFill), the soldiers staying behind the right one.
// Identified by play with --show-state: $06 is the intro after boot (pillarboxed: framed
// pictures on black).
enum class AfterEnding { Other, TheEnd, Theatre };

AfterEnding after_ending_scene(const uint8_t* ram)
{
    // Objects (ID in the word at +0 of each $40-byte slot from $FFD000), seen with
    // --log-ram over the whole state: "The End" is drawn by $80 and then $81, the theatre
    // by $B8 and $BB (its setup) and $BC and $BD (to the end of the state). Nearby IDs
    // belong to the other scenes ($B3, $B6 and $B7 are the night flight's fireworks).
    for (uint32_t slot = 0xD000; slot < 0xE000; slot += 0x40) {
        const int id = ram[slot] << 8 | ram[slot + 1];
        if (id == 0x80 || id == 0x81)
            return AfterEnding::TheEnd;
        if (id == 0xB8 || id == 0xBB || id == 0xBC || id == 0xBD)
            return AfterEnding::Theatre;
    }
    return AfterEnding::Other;
}

bool is_wide_state(uint16_t state, uint16_t level, const uint8_t* ram)
{
    (void)level;
    if (state == 0x16)
        return after_ending_scene(ram) != AfterEnding::TheEnd;
    return state == 0x0E || state == 0x10 || state == 0x22 || state == 0x2A || state == 0x2C || state == 0x2E;
}

// Fast forward (Backspace, a controller's Y, or the touch controls' >> button, held): while
// one of the game's story scenes plays, each picture runs kFastForward game frames instead
// of one. The game's own frames are unchanged, just shown faster; its sound is muted while
// fast-forwarding (four frames of sound in one frame's time would only play as a squeal or
// pile up), and the MD+ music is silenced with it.
//   $2A  the story before the hallway (Mizrabel takes Minnie), after Start on the title
//   $22  the scenes between worlds: the hallway walks, the castle exterior, the Practice ending
//   $2C  the ending's setup, and $2E the ending in the final arena (after Mizrabel's last
//        hit); in a Boss Rush these two show the results screen instead, which isn't a scene
//   $16  the scenes after the ending: the night flight, the cliff, the forest credits,
//        "The End" and the theatre
//   $06  the story pictures after boot ("Welcome to Vera City..."), up to the title: the
//        state runs on into the title screen, whose logo comes in when its step counter
//        $FFF40C reaches $1E (Start there opens the title menu from $1F on)
// Not the title screen and its menu, the options, the gem and score screens, level play or
// boss fights.
constexpr int kFastForward = 4;

bool is_cutscene_state(const uint8_t* ram)
{
    const int state = ram[0xF300] << 8 | ram[0xF301];
    const bool boss_rush = ram[0xFF1E] != 0;        // RushActive (boss_rush.asm)
    switch (state) {
    case 0x06:
        return ram[0xF40C] < 0x1E;
    case 0x2A: case 0x22: case 0x16:
        return true;
    case 0x2C: case 0x2E:
        return !boss_rush;
    default:
        return false;
    }
}

bool g_widescreen = true;
bool g_easy_bounce = false;
bool g_clean_rock_edges = true;                     // Enhancements > Graphics > Clean Rock Edges
bool g_smooth_fades = true;                         // Enhancements > Graphics > Smooth Fades (fade.cpp)
bool g_smooth_water = true;                         // Enhancements > Graphics > Smooth Water
bool g_smooth_glass = true;                         // Enhancements > Graphics > Smooth Glass
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
std::vector<uint32_t> g_water_layers[2];            // Smooth Water: plane A's water, low and high priority
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
// The CPU's interrupt input, as Musashi's m68k_set_irq keeps it (CPU_INT_LEVEL, nmi_pending):
// the level, and a non-maskable one (a change to level 7) waiting to be taken. Translated code
// reads these; Musashi keeps its own copy for when it runs. (The Mega Drive never raises 7.)
int g_irq_level = 0;
bool g_nmi_pending = false;

void set_irq(int level)
{
    if (g_irq_level != 7 && level == 7)
        g_nmi_pending = true;
    g_irq_level = level;
#ifndef COI_NO_INTERPRETER
    m68k_set_irq(level);
#endif
}

void update_irq()
{
    int level = 0;
    for (int l = 7; l > 0; l--)
        if (g_irq_pending & (1 << l)) {
            level = l;
            break;
        }
    set_irq(level);
}

extern "C" unsigned int coi_native_irq_level(void)
{
    return g_nmi_pending ? 8 : unsigned(g_irq_level);
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
#ifdef __ANDROID__
    // The app's private storage (where it keeps the ROM it is given), then its folder on
    // shared storage (Android/data/<app>/files), which a file manager can reach.
    if (const char* internal = SDL_GetAndroidInternalStoragePath())
        dirs.push_back(std::filesystem::path(internal) / name);
    if (const char* external = SDL_GetAndroidExternalStoragePath())
        dirs.push_back(std::filesystem::path(external) / name);
#endif
    if (const char* base = SDL_GetBasePath())
        dirs.push_back(std::filesystem::path(base) / name);
    for (const char* up : {"", "../", "../../", "../../../"})
        dirs.push_back(std::filesystem::path(std::string(up) + name));
    return dirs;
}

// The files in dir with the given extension; none if it can't be read (on Android a folder
// on shared storage that another app or adb created can be listed but not opened).
std::vector<std::filesystem::path> files_in(const std::filesystem::path& dir, const char* extension)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error))
        return files;
    for (std::filesystem::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
        if (it->path().extension() == extension)
            files.push_back(it->path());
    return files;
}

std::vector<uint8_t> load_rom(const std::string& explicit_path)
{
    std::vector<std::filesystem::path> candidates;
    if (!explicit_path.empty())
        candidates.push_back(explicit_path);
    else
        for (const auto& dir : search_dirs("rom"))
            for (const auto& file : files_in(dir, ".bin"))
                candidates.push_back(file);
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

// Looks for an MD+ pack in mdplus/ and loads the first that fits: its patched ROM must be this
// game with the pack's changes only, and take the port's patches as the cartridge's ROM does.
// Returns why none was loaded (empty when one was).
std::string find_mdplus_pack()
{
    g_mdplus.stop();                                // closes the playing track's file
    g_mdplus = MdPlus();
    g_rom_mdplus.clear();
    std::string why = "no .cue file in mdplus/";
    for (const auto& dir : search_dirs("mdplus")) {
        for (const auto& cue : files_in(dir, ".cue")) {
            std::string error;
            if (!g_mdplus.load(cue, error)) {
                why = error;
                std::fprintf(stderr, "MD+ pack not used: %s\n", error.c_str());
                continue;
            }
            std::ifstream file(g_mdplus.rom_path(), std::ios::binary);
            g_rom_mdplus.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            size_t differ = 0;
            for (size_t i = 0x200; i < g_rom_cartridge.size() && i < g_rom_mdplus.size(); i++)
                differ += g_rom_cartridge[i] != g_rom_mdplus[i];
            if (g_rom_mdplus.size() < g_rom_cartridge.size() || differ > 4096) {
                why = g_mdplus.rom_path().filename().string() + " is not a patch of this ROM";
            } else {
                std::vector<uint8_t> trial = g_rom_mdplus;
                const std::string wide = apply_widescreen_patches(trial);
                trial = g_rom_mdplus;
                const std::string swing = apply_smooth_swing(trial);
                if ((g_wide_available && !wide.empty()) || (g_smooth_swing_available && !swing.empty())) {
                    why = "the port's patches don't fit its ROM";
                } else {
                    std::printf("MD+ pack: %s\n", g_mdplus.name().c_str());
                    return {};
                }
            }
            std::fprintf(stderr, "MD+ pack not used: %s\n", why.c_str());
            g_mdplus = MdPlus();
            g_rom_mdplus.clear();
        }
    }
    return why;
}

// Per-scene fills for the widened strips, where a scene's plane holds the wrong thing past
// the original view (see docs/widescreen.md). Chosen once per frame in run_frame.
enum class StripRule { None, TowerFill, ArenaWalls, TheatreFill, StoryFill };
StripRule g_strip_rule = StripRule::None;

// Floor block to repeat as walls in an arena's strips, in plane A pixels, and the tiles it
// is made of.
struct WallSource { int x, y, w, h, first_tile, last_tile; };
WallSource g_wall;
constexpr WallSource kLevel2BossWalls = {192, 224, 32, 32, 0x101, 0x110};  // stage 9: 32 x 32 blocks
constexpr WallSource kFinalBossWalls = {192, 464, 64, 32, 0x175, 0x189};   // stage 28: 64 x 32 floor slabs

// The wall block's name table entries, as last seen intact. The final boss's hit flash
// uses the plane A rows that hold the slab (Y 448-511, off screen) as scratch space for a
// moment, which emptied the walls; drawing them from this copy keeps them in place.
std::vector<uint16_t> g_wall_cells;

void update_wall_cells()
{
    static const int kSizes[4] = {32, 64, 32, 128};
    const int base = (g_vdp.reg[2] & 0x38) << 10, plane_w = kSizes[g_vdp.reg[16] & 3];
    const int cells_w = g_wall.w / 8, cells_h = g_wall.h / 8;
    std::vector<uint16_t> cells;
    for (int cy = 0; cy < cells_h; cy++)
        for (int cx = 0; cx < cells_w; cx++) {
            const int at = (base + ((g_wall.y / 8 + cy) * plane_w + g_wall.x / 8 + cx) * 2) & 0xFFFF;
            const uint16_t entry = g_vdp.vram[at] << 8 | g_vdp.vram[at | 1];
            if ((entry & 0x7FF) < g_wall.first_tile || (entry & 0x7FF) > g_wall.last_tile)
                return;                             // in use for something else: keep the last copy
            cells.push_back(entry);
        }
    g_wall_cells = cells;
}

int wrap(int value, int size) { return ((value % size) + size) % size; }

// Plane A camera X at the start of the frame, for rules that need to know which lap of the
// 512-pixel plane a pixel shows.
int g_strip_camera_x = 0;

// Whether plane A holds the castle exterior. For level 5, $22 first shows a hallway walk on
// the same state, then loads the exterior; the tower and castle cells tell them apart.
bool exterior_in_plane_a()
{
    static const int kSizes[4] = {32, 64, 32, 128};
    const int base = (g_vdp.reg[2] & 0x38) << 10, plane_w = kSizes[g_vdp.reg[16] & 3];
    auto tile = [&](int col, int row) {
        const int at = (base + (row * plane_w + col) * 2) & 0xFFFF;
        return (g_vdp.vram[at] << 8 | g_vdp.vram[at | 1]) & 0x7FF;
    };
    return plane_w == 64 && tile(63, 3) == 0x213 && tile(52, 8) == 0x200 && tile(0, 8) == 0x1A9;
}

// Remaps plane A coordinates for pixels in the widened strips (view_x < 0 or >= 320), and
// for the rules that set Vdp::extend_plane_a_inside, inside the original columns too.
bool extend_plane_a(int view_x, int line, int& plane_x, int& plane_y)
{
    (void)line;
    switch (g_strip_rule) {
    case StripRule::TowerFill: {
        // The scene's plane A X, unwrapped: the camera pans from 0 to 192, so the view shows
        // plane X -40 to 551, which wraps round to the same 512 pixels at both ends.
        const int x = g_strip_camera_x + view_x + wrap(plane_x - g_strip_camera_x - view_x + 256, 512) - 256;
        // At the start the pale castle's left edge is at plane X 0, so the left strip showed
        // the brick tower's end. Continue the castle instead with its own columns: its rows
        // repeat every 64 pixels, except the panel rows around the door (plane rows 12-24),
        // which repeat every 112, so each row joins its first column without a seam.
        if (x < 0) {
            const int row = wrap(plane_y, 512) / 8;
            plane_x = x + (row >= 12 && row <= 24 ? 112 : 64);
            return true;
        }
        // The brick tower ends at plane A X 512 (0), and past it the plane starts over with
        // the pale castle wall. Make the tower 40 pixels wider instead: after its column 61
        // (X 488-495), 40 more pixels of its own brick, then its dark right edge (columns
        // 62-63, X 496-511) at X 536-551, the end of the widened view at the scene's last
        // camera X (192). The bricks repeat every 16 pixels (columns 60-61) and alternate
        // by row: rows 3-5 take them from rows 3-4, rows 7 and below from rows 8-9 (rows 2
        // and 6, the last merlon's cap and the cornice, from column 62; rows 0-1 are sky).
        // The door and everything left of column 62 stay as they are.
        if (x >= 496 && x < 552) {
            const int k = x - 496;                  // 0-55 across the moved part
            const int y = wrap(plane_y, 512), row = y / 8;
            if (k >= 40) {
                plane_x = 456 + k;                  // the edge, columns 62-63
            } else if (row < 2) {
                plane_x = 472 + wrap(k, 8);         // sky: the blank tile at (472, 0)
                plane_y = wrap(y, 8);
            } else if (row == 2 || row == 6) {
                plane_x = 496 + wrap(k, 8);         // cap and cornice
            } else {
                plane_x = 480 + wrap(k, 16);        // brick, columns 60-61
                plane_y = row < 6 ? 24 + wrap(y - 24, 16) : 64 + wrap(y, 16);
            }
            return true;
        }
        return false;
    }
    case StripRule::ArenaWalls:
        // Locked boss arenas whose strips lie outside the player's reach (stages 9 and 28):
        // walls of the arena's own floor blocks on both sides, aligned to the floor's grid
        // in plane coordinates, so they line up with the floor and scroll with it.
        plane_x = g_wall.x + wrap(plane_x - g_wall.x, g_wall.w);
        plane_y = g_wall.y + wrap(plane_y - g_wall.y, g_wall.h);
        return true;
    case StripRule::TheatreFill:
        // The theatre after the credits ($16; planes 128 cells wide, no scrolling but plane
        // B's vertical one, which lowers the curtain): plane A holds the stage's front edge,
        // the footlights and the seats in rows 16-31, columns 0-39, and is empty past them.
        // The seats repeat every 4 columns and the footlights every 8 (columns 2, 10, 28
        // and 36, as if the middle ones were missing), so the strips take the columns 8 in
        // from each edge: a footlight is cut by each screen edge. Rows 0-15 (the cinema
        // screen) are transparent outside its frame and need nothing.
        if (wrap(plane_y, 256) < 128)
            return false;
        plane_x += view_x < 0 ? 64 : -64;
        return true;
    case StripRule::StoryFill: {
        // The story before the hallway ($2A; planes 128 cells wide, no scrolling): plane A
        // holds the trees, the canopy and the ground in rows 0-17 of columns 0-39 (all high
        // priority, transparent between the trees), then the text box. The trees repeat
        // every 26 columns (at 2-6 and 28-32), so the left strip takes the columns 26 in
        // (the top two canopy rows, which do not, 18 in), joining column 0 without a seam.
        // On the right the picture ends with pieces of its own (columns 34-39: the
        // canopy's edge, the ground before the door), so each row takes the columns that
        // best follow them. Below row 17 (the text box) the strips stay empty.
        const int row = wrap(plane_y, 256) / 8;
        if (row >= 18) {
            plane_x = wrap(plane_x, 8);             // column 0: transparent
            return true;
        }
        if (view_x < 0) {
            plane_x += (row < 2 ? 18 : 26) * 8;
            return true;
        }
        static const int8_t kRight[18] = {16, 34, 28, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 22, 22, 22, 22, 22};
        plane_x += (kRight[row] - 40) * 8;
        return true;
    }
    default:
        return false;
    }
}

// Plane B's strips (Vdp::extend_plane_b), for the rules that need them.
bool extend_plane_b(int view_x, int line, int& plane_x, int& plane_y)
{
    (void)line;
    if (g_strip_rule == StripRule::StoryFill) {
        // The story's brick wall (plane B rows 0-16, columns 0-39; the arch at 28-39): the
        // bricks repeat every 2 columns, so the strips take columns 10 in (left) and 30 in
        // (right). Row 2 is offset by a column above the arch, as the game draws it, and
        // continues so. The right strip's bricks take the high priority (see main), so the
        // toy soldiers, who start past the right edge, stay hidden until they reach the
        // arch's pillar (which hides them in the game) and come out of the door.
        if (view_x < 0) {
            plane_x += 80;
            return true;
        }
        plane_x -= wrap(plane_y, 256) / 8 == 2 ? 232 : 240;
        return true;
    }
    if (g_strip_rule != StripRule::TheatreFill)
        return false;
    // The theatre's curtain: plane B rows 0-15, columns 0-39, lowered by plane B's vertical
    // scroll (112 to 0). Past column 39 the plane holds other graphics. Every row repeats
    // every 12 columns (96 pixels), so the strips take the columns 12 in from each edge,
    // except the fringe (rows 3 and 4), which ends with an end piece of its own at the
    // right (columns 35-39). There the fringe keeps repeating instead, over columns 35-39
    // and on to the edge of the widened view, which cuts it as the left edge does. (Moving
    // the end piece out to the edge put it under the top of a different swag.) Rows 16-31
    // are blank in columns 0-39.
    const int row = wrap(plane_y, 256) / 8;
    if (view_x < 0) {
        plane_x += 96;
        return true;
    }
    if (view_x >= ((row == 3 || row == 4) ? 280 : 320)) {
        plane_x -= 96;
        return true;
    }
    return false;
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
    for (int i = 0; i < 2; i++) {
        uint32_t* row = &g_water_layers[i][line * stride];
        std::fill(row, row + stride, 0u);
        layers.water[i] = row + border + kLayerMargin - margin;
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

// Smooth Water: the big waterfalls of stages 11-13 flow by palette cycling: every 3 frames
// the game moves colours 1-7 of palette line 2 (CRAM 33-39) up one place (colour 2 takes
// colour 1's value, ..., colour 1 takes colour 7's), so their shades step down the streaks.
// Between steps each colour is eased towards the value the next step will give it, so the
// streaks glide instead. (The small waterfalls in the background cycle colours 11-14 of line
// 3 every 17 frames; eased, their few shades pulsed, so they are left as they are.)
struct WaterCycle {
    int first, count, dir;                        // CRAM entries; +1: each takes the one below's
    static constexpr int kMaxCount = 16;
    uint16_t last[kMaxCount] = {};
    uint16_t before[kMaxCount] = {};                // the colours before the last step
    bool primed = false, on = false;
    int frame = 0, last_step = -1000, period = 3;

    void update(bool active, const uint16_t* cram)
    {
        frame++;
        if (!active) {
            primed = on = false;
            return;
        }
        const uint16_t* now = cram + first;
        bool rotated = primed, same = true;
        for (int i = 0; i < count; i++) {
            rotated &= now[i] == last[(i + count - dir) % count];
            same &= now[i] == last[i];
        }
        if (rotated && !same) {
            const int since = frame - last_step;
            // A step a frame or two late (the game lags now and then) keeps the period.
            if (since >= 2 && since <= 40 && (since < period || since > period + 2))
                period = since;
            last_step = frame;
            std::copy(last, last + count, before);
        }
        std::copy(now, now + count, last);
        primed = true;
        // The cycle is running. A late step holds the next colours until it comes; one that
        // doesn't come within two periods means the cycle stopped.
        on = frame - last_step < 2 * period;
    }

    // The colours `back` game frames (0-1) before the current one, and how far (0-1) they
    // are on their way to the next step's: a moment before the last step is the end of the
    // step before it.
    const uint16_t* colours_at(double back, double& t) const
    {
        const double since = frame - last_step - back;
        if (since < 0) {
            t = std::clamp((period + since) / period, 0.0, 1.0);
            return before;
        }
        t = std::clamp(since / period, 0.0, 1.0);
        return last;
    }

    // Sets the cycling colours in `palette` (as shown `back` game frames before the current
    // one): the frame's (kept by update: by the time frames in between are drawn, CRAM may
    // already hold the next frame's), eased towards their next values. (Smooth Fades would
    // take a step of this cycle for a fade and ease it too, so its colours aren't used here.)
    void apply(PaletteOverride& palette, double back) const
    {
        if (!on)
            return;
        double t;
        const uint16_t* colours = colours_at(back, t);
        auto scale = [](int v) { return (uint32_t)(v * 255 / 7); };
        uint32_t now[kMaxCount];
        for (int i = 0; i < count; i++) {
            const uint16_t c = colours[i];
            palette.source[first + i] = last[i];        // what CRAM holds for this frame
            now[i] = scale((c >> 1) & 7) << 16 | scale((c >> 5) & 7) << 8 | scale((c >> 9) & 7);
        }
        for (int i = 0; i < count; i++) {
            const uint32_t a = now[i], b = now[(i + count - dir) % count];
            uint32_t mixed = 0xFF000000;
            for (int shift = 0; shift <= 16; shift += 8) {
                const double v = (a >> shift & 0xFF) * (1 - t) + (b >> shift & 0xFF) * t;
                mixed |= (uint32_t)std::lround(v) << shift;
            }
            palette.rgb[first + i] = mixed;
        }
    }
};
WaterCycle g_water_cycles[1] = {{33, 7, 1}};

// Smooth Water: some background water flows by palette cycling on plane B, a few colours that
// all move one place every so many frames, so the shades step along the water:
//   - stages 11 and 13: the small waterfalls (tiles 575, 576, 586 and 588; values 11-14 of
//     palette line 3, CRAM 59-62) every 17 frames, each colour taking the next one's value
//     (value 11 takes 12's, ..., 14 takes 11's);
//   - stages 24 and 25: the trickles down the walls (tiles 668-679; values 6-8 of line 3, CRAM
//     54-56) every 16 frames, each colour taking the one before's (7 takes 6's, 6 takes 8's).
// Eased like the big waterfalls, their few, unevenly used shades made the whole water pulse;
// so each step is morphed instead, column by column. In each column, each run of these pixels
// is matched with itself one step later (a dynamic-programming match that keeps the order of
// the pixels and pairs equal colours), and between steps the shades slide along the matched
// pairs: the picture before the step moved forward and the one after it moved back, blended,
// so the last picture before a step is the step itself.
struct Flow {
    int first_stage, last_stage;                    // game stage indexes ($FFFF42)
    int first_value, count, dir;                    // values on palette line 3 (CRAM 48 + value); dir as WaterCycle
    std::vector<uint16_t> tiles;
    WaterCycle cycle;
};
Flow g_flows[] = {
    {10, 12, 11, 4, -1, {575, 576, 586, 588}, {59, 4, -1}},
    {23, 24, 6, 3, 1, {668, 669, 670, 671, 672, 673, 674, 675, 676, 677, 678, 679}, {54, 3, 1}},
};
struct FlowRun { const Flow* flow; int x, y; std::vector<uint8_t> values; };   // plane pixels (x, y..), values 0..count-1
std::map<std::vector<uint8_t>, std::vector<std::pair<int16_t, int16_t>>> g_flow_matches;
std::vector<FlowRun> g_flow_runs;
std::vector<uint32_t> g_flow_rgb, g_flow_rgb_back; // plane B colours (Vdp::plane_b_rgb)
int g_flow_plane_w = 0, g_flow_plane_h = 0;

// Matches a run of values with itself one step later: value v now shows the colour that value
// v - dir shows after the step.
const std::vector<std::pair<int16_t, int16_t>>& flow_match(const Flow& flow, const std::vector<uint8_t>& values)
{
    std::vector<uint8_t> key = values;
    key.push_back((uint8_t)(flow.count * 4 + flow.dir + 1));    // the same values in another flow match differently
    auto found = g_flow_matches.find(key);
    if (found != g_flow_matches.end())
        return found->second;
    const int n = (int)values.size(), count = flow.count;
    constexpr int kBand = 8;
    std::vector<float> cost((n + 1) * (n + 1), 1e9f);
    std::vector<uint8_t> from((n + 1) * (n + 1));
    cost[0] = 0;
    for (int i = 1; i <= n; i++)
        for (int j = std::max(1, i - kBand); j <= std::min(n, i + kBand); j++) {
            const float d = (values[i - 1] == (values[j - 1] - flow.dir + count) % count ? 0.0f : 1.0f)
                            + 0.05f * std::abs(i - j);
            const float diag = cost[(i - 1) * (n + 1) + j - 1], up = cost[(i - 1) * (n + 1) + j],
                        left = cost[i * (n + 1) + j - 1];
            const float best = std::min({diag, up, left});
            cost[i * (n + 1) + j] = d + best;
            from[i * (n + 1) + j] = best == diag ? 0 : best == up ? 1 : 2;
        }
    std::vector<std::pair<int16_t, int16_t>> pairs;
    for (int i = n, j = n; i > 0 && j > 0;) {
        pairs.push_back({(int16_t)(i - 1), (int16_t)(j - 1)});
        const uint8_t f = from[i * (n + 1) + j];
        if (f == 0) i--, j--;
        else if (f == 1) i--;
        else j--;
    }
    if (g_flow_matches.size() > 4096)
        g_flow_matches.clear();
    return g_flow_matches[key] = std::move(pairs);
}

// Finds the flowing pixels in plane B (their stages in play, Smooth Water on, their cycle
// running) as runs down each column.
void update_flows(bool on, uint16_t state)
{
    const int stage = g_bus.ram[0xFF42] << 8 | g_bus.ram[0xFF43];
    g_flow_runs.clear();
    static const int kSizes[4] = {32, 64, 32, 128};
    const int cells_w = kSizes[g_vdp.reg[16] & 3], cells_h = kSizes[(g_vdp.reg[16] >> 4) & 3];
    const int base_b = (g_vdp.reg[4] & 0x07) << 13;
    g_flow_plane_w = cells_w * 8, g_flow_plane_h = cells_h * 8;
    for (Flow& flow : g_flows) {
        flow.cycle.update(on && state == 0x0E && stage >= flow.first_stage && stage <= flow.last_stage, g_vdp.cram);
        if (!flow.cycle.on)
            continue;
        std::map<int, std::vector<std::pair<int, uint8_t>>> columns;   // x -> (y, value)
        for (int cy = 0; cy < cells_h; cy++)
            for (int cx = 0; cx < cells_w; cx++) {
                const uint32_t at = (base_b + (cy * cells_w + cx) * 2) & 0xFFFF;
                const uint16_t entry = g_vdp.vram[at] << 8 | g_vdp.vram[at | 1];
                const int tile = entry & 0x7FF;
                if (((entry >> 13) & 3) != 3 || std::find(flow.tiles.begin(), flow.tiles.end(), tile) == flow.tiles.end())
                    continue;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        const int fx = entry & 0x0800 ? 7 - x : x, fy = entry & 0x1000 ? 7 - y : y;
                        const uint8_t pair = g_vdp.vram[(tile * 32 + fy * 4 + (fx >> 1)) & 0xFFFF];
                        const int value = (fx & 1 ? pair & 15 : pair >> 4) - flow.first_value;
                        if (value >= 0 && value < flow.count)
                            columns[cx * 8 + x].push_back({cy * 8 + y, (uint8_t)value});
                    }
            }
        for (auto& [x, pixels] : columns) {
            std::sort(pixels.begin(), pixels.end());
            for (size_t i = 0; i < pixels.size();) {
                FlowRun run{&flow, x, pixels[i].first, {}};
                size_t k = i;
                while (k < pixels.size() && pixels[k].first == run.y + (int)run.values.size())
                    run.values.push_back(pixels[k++].second);
                g_flow_runs.push_back(std::move(run));
                i = k;
            }
        }
    }
}

// The flowing pixels' colours `back` game frames (0-1) before the current one, into `out`
// (plane B pixels; Vdp::plane_b_rgb). Returns false when there are none.
bool flow_colours(std::vector<uint32_t>& out, double back)
{
    // Only the flowing pixels are set; clear the ones set last time (the runs may change).
    static std::vector<size_t> set_before[2];
    std::vector<size_t>& set = set_before[&out == &g_flow_rgb ? 0 : 1];
    if (out.size() != (size_t)g_flow_plane_w * g_flow_plane_h)
        out.assign((size_t)g_flow_plane_w * g_flow_plane_h, 0);
    else
        for (size_t at : set)
            out[at] = 0;
    set.clear();
    if (g_flow_runs.empty())
        return false;
    auto scale = [](int v) { return v * 255.0f / 7; };
    std::vector<float> acc;
    const Flow* colours_of = nullptr;
    double t = 0;
    float now[WaterCycle::kMaxCount][3], next[WaterCycle::kMaxCount][3];
    for (const FlowRun& run : g_flow_runs) {
        const Flow& flow = *run.flow;
        if (colours_of != &flow) {                  // this flow's colours now and after the step
            colours_of = &flow;
            // the frame's colours (CRAM may hold the next frame's by the time frames in
            // between are drawn); a moment before the last step is the end of the one before
            const uint16_t* colours = flow.cycle.colours_at(back, t);
            for (int k = 0; k < flow.count; k++)
                for (int ch = 0; ch < 3; ch++) {
                    now[k][ch] = scale((colours[k] >> (1 + ch * 4)) & 7);
                    next[k][ch] = scale((colours[(k - flow.dir + flow.count) % flow.count] >> (1 + ch * 4)) & 7);
                }
        }
        const int n = (int)run.values.size();
        acc.assign((size_t)n * 4, 0.0f);
        for (const auto& [ya, yb] : flow_match(flow, run.values)) {
            const double y = ya + (yb - ya) * t;
            const int y0 = (int)std::floor(y);
            const float f = (float)(y - y0);
            for (const auto& [yy, w] : {std::pair<int, float>{y0, 1 - f}, {y0 + 1, f}}) {
                if (yy < 0 || yy >= n || w <= 0)
                    continue;
                float* a = &acc[yy * 4];
                for (int ch = 0; ch < 3; ch++)
                    a[ch] += w * (float)((1 - t) * now[run.values[ya]][ch] + t * next[run.values[yb]][ch]);
                a[3] += w;
            }
        }
        for (int y = 0; y < n; y++) {
            const float* a = &acc[y * 4];
            float rgb[3];
            for (int ch = 0; ch < 3; ch++)
                rgb[ch] = a[3] > 0 ? a[ch] / a[3] : (t < 0.5 ? now : next)[run.values[y]][ch];
            const size_t at = (size_t)((run.y + y) % g_flow_plane_h) * g_flow_plane_w + run.x;
            set.push_back(at);
            out[at] = 0xFF000000u | (uint32_t)std::lround(rgb[0]) << 16 | (uint32_t)std::lround(rgb[1]) << 8
                      | (uint32_t)std::lround(rgb[2]);
        }
    }
    return true;
}

// Points `vdp` at the flowing pixels' colours as shown `back` game frames before the current one.
void set_flow_colours(Vdp& vdp, std::vector<uint32_t>& out, double back)
{
    if (flow_colours(out, back)) {
        vdp.plane_b_rgb = out.data();
        vdp.plane_b_rgb_w = g_flow_plane_w, vdp.plane_b_rgb_h = g_flow_plane_h;
    } else {
        vdp.plane_b_rgb = nullptr;
    }
}

// Smooth Water: stage 10's waterspouts (object $0E, sprites from tile 848 to 917 on palette
// line 1). The game animates them only by swapping each spout for its mirror image every 4-6
// frames (its sprites' X mirrored about its middle, their flip bits toggled; the jet's edges
// stay put), and bobs them a pixel up and down every 4 frames. They are drawn instead as a
// smooth loop between the two poses, 5 frames towards the mirror image and 5 back, with the
// in-between pictures morphed column by column: each column of the pose is matched to the
// same column of the mirror image (a dynamic-programming match on opacity and brightness that
// keeps the order of the pixels), and each matched pixel is moved part of the way up or down
// while its colour blends. The bob is left out: a spout sitting one line below the highest
// it has been over the last 8 frames is drawn one line higher (a real rise or fall still
// shows). Each spout is the sprite from tile 848 (its crown's edge, whose flip bit tells the
// pose) with the other sprites nearest to it, recognised from frame to frame by its middle in
// the level. A spout is 80 pixels wide, with that sprite at its left edge, or flipped at its
// right edge in the mirror image. Near the edges of the view the game leaves out the sprites
// beyond them, so the picture is only taken while the spout's whole width is there; until
// one has been, the spout is drawn as the game draws it. The game's own state is untouched:
// this only changes what is drawn.
struct SpoutMorph {
    int key = 0;                                    // camera X * 2 + its sprites' left + right
    int ys[8] = {}, seen = 0;                       // its top, newest first (for the bob)
    int x = 0, y = 0, w = 0, h = 0;                 // view position and size
    int sprite = 0;                                 // the anchor's sprite table index
    std::vector<uint8_t> pose;                      // palette indexes (0 = clear), unflipped pose
    std::vector<int> members;                       // its sprites' table indexes this frame
    std::vector<std::vector<std::pair<int16_t, int16_t>>> pairs;   // per column: matched rows
};
std::vector<SpoutMorph> g_spouts;
int g_spout_clock = 0;                              // game frames, for the loop

constexpr int kSpoutLoop = 10;                      // frames for there and back
constexpr int kSpoutWidth = 80;

// The colour of palette index `i` as the frame shows it.
uint32_t shown_rgb(int i)
{
    const uint16_t c = g_vdp.cram[i & 63];
    if (g_vdp.palette && g_vdp.palette->source[i & 63] == c)
        return g_vdp.palette->rgb[i & 63] & 0xFFFFFF;
    auto scale = [](int v) { return (uint32_t)(v * 255 / 7); };
    return scale((c >> 1) & 7) << 16 | scale((c >> 5) & 7) << 8 | scale((c >> 9) & 7);
}

// Matches each column of `spout.pose` with the same column of its mirror image.
void match_spout_columns(SpoutMorph& spout)
{
    const int w = spout.w, h = spout.h;
    auto feature = [&](int x, int y, float& opacity, float& light) {
        const int i = spout.pose[y * w + x];
        opacity = i ? 1.0f : 0.0f;
        const uint32_t c = i ? shown_rgb(i) : 0;
        light = ((c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF)) / 765.0f;
    };
    constexpr int kBand = 12;                       // the furthest a pixel moves
    std::vector<float> cost((h + 1) * (h + 1));
    std::vector<uint8_t> from((h + 1) * (h + 1));   // 0 diagonal, 1 up, 2 left
    spout.pairs.assign(w, {});
    for (int x = 0; x < w; x++) {
        const int mx = w - 1 - x;                   // the mirror image's column x
        std::fill(cost.begin(), cost.end(), 1e9f);
        cost[0] = 0;
        for (int i = 1; i <= h; i++)
            for (int j = std::max(1, i - kBand); j <= std::min(h, i + kBand); j++) {
                float oa, la, ob, lb;
                feature(x, i - 1, oa, la);
                feature(mx, j - 1, ob, lb);
                const float d = std::abs(oa - ob) * 2 + std::abs(la - lb) + 0.02f * std::abs(i - j);
                const float diag = cost[(i - 1) * (h + 1) + j - 1], up = cost[(i - 1) * (h + 1) + j],
                            left = cost[i * (h + 1) + j - 1];
                const float best = std::min({diag, up, left});
                cost[i * (h + 1) + j] = d + best;
                from[i * (h + 1) + j] = best == diag ? 0 : best == up ? 1 : 2;
            }
        auto& pairs = spout.pairs[x];
        for (int i = h, j = h; i > 0 && j > 0;) {
            pairs.push_back({(int16_t)(i - 1), (int16_t)(j - 1)});
            const uint8_t f = from[i * (h + 1) + j];
            if (f == 0) i--, j--;
            else if (f == 1) i--;
            else j--;
        }
    }
}

// Finds the spouts in the sprite list (stage 10 in play with Smooth Water on), keeps their
// poses and matches up to date, and steadies their position.
void update_spouts(bool on, uint16_t state)
{
    const bool here = on && state == 0x0E && (g_bus.ram[0xFF42] << 8 | g_bus.ram[0xFF43]) == 9;
    g_vdp.hidden_sprites.clear();
    if (!here) {
        g_spouts.clear();
        return;
    }
    g_spout_clock++;
    struct Piece { int index, x, y, w, h; uint16_t attr; };
    std::vector<Piece> anchors, others;
    const int base = g_vdp.reg[12] & 0x01 ? (g_vdp.reg[5] & 0x7E) << 9 : (g_vdp.reg[5] & 0x7F) << 9;
    int link = 0;
    for (int n = 0; n < 80; n++) {
        const uint8_t* s = &g_vdp.vram[(base + link * 8) & 0xFFFF];
        const uint16_t attr = s[4] << 8 | s[5];
        const int tile = attr & 0x7FF;
        if (tile >= 848 && tile <= 917 && ((attr >> 13) & 3) == 1)
            (tile == 848 ? anchors : others).push_back({link, ((s[6] << 8 | s[7]) & 0x1FF) - 128,
                                                        ((s[0] << 8 | s[1]) & 0x3FF) - 128,
                                                        ((s[2] >> 2) & 3) + 1, (s[2] & 3) + 1, attr});
        link = s[3] & 0x7F;
        if (!link)
            break;
    }
    const int camera_x = (int16_t)(g_bus.ram[0xFA84] << 8 | g_bus.ram[0xFA85]);
    std::vector<SpoutMorph> now;
    for (const Piece& anchor : anchors) {
        const bool flipped = anchor.attr & 0x0800;
        const int x0 = flipped ? anchor.x + anchor.w * 8 - kSpoutWidth : anchor.x, x1 = x0 + kSpoutWidth;
        std::vector<const Piece*> members = {&anchor};
        for (const Piece& p : others) {
            const Piece* nearest = nullptr;
            for (const Piece& a : anchors)
                if (!nearest || std::abs(p.x - a.x) + std::abs(p.y - a.y) < std::abs(p.x - nearest->x) + std::abs(p.y - nearest->y))
                    nearest = &a;
            if (nearest == &anchor && p.x >= x0 && p.x + p.w * 8 <= x1 && p.y >= anchor.y - 16)
                members.push_back(&p);                  // inside its box
        }
        int y0 = 1 << 20, y1 = -(1 << 20), left = 1 << 20, right = -(1 << 20);
        for (const Piece* p : members) {
            left = std::min(left, p->x), right = std::max(right, p->x + p->w * 8);
            y0 = std::min(y0, p->y), y1 = std::max(y1, p->y + p->h * 8);
        }
        const bool whole = left == x0 && right == x1;   // no sprite left out at a view edge
        SpoutMorph spout;
        spout.key = camera_x * 2 + x0 + x1;
        for (const SpoutMorph& old : g_spouts)
            if (std::abs(old.key - spout.key) <= 8)
                spout = old;
        spout.key = camera_x * 2 + x0 + x1;
        spout.x = x0, spout.sprite = anchor.index;
        spout.members.clear();
        for (const Piece* p : members)
            spout.members.push_back(p->index);
        // The picture, drawn back to front (the sprite list's first is in front), unflipped:
        // mirrored back when the game shows the mirror image.
        const int w = x1 - x0, h = y1 - y0;
        std::vector<uint8_t> pose(w * h);
        for (auto it = members.rbegin(); it != members.rend(); ++it) {
            const Piece& p = **it;
            for (int cy = 0; cy < p.h * 8; cy++)
                for (int cx = 0; cx < p.w * 8; cx++) {
                    const int col = p.attr & 0x0800 ? p.w * 8 - 1 - cx : cx;
                    const int row = p.attr & 0x1000 ? p.h * 8 - 1 - cy : cy;
                    const int tile = (p.attr & 0x7FF) + (col >> 3) * p.h + (row >> 3);
                    const uint8_t pair = g_vdp.vram[(tile * 32 + (row & 7) * 4 + ((col & 7) >> 1)) & 0xFFFF];
                    const int pixel = col & 1 ? pair & 15 : pair >> 4;
                    if (!pixel)
                        continue;
                    int px = p.x - x0 + cx;
                    if (flipped)
                        px = w - 1 - px;
                    if (px >= 0 && px < w)
                        pose[(p.y - y0 + cy) * w + px] = (uint8_t)(((p.attr >> 9) & 0x30) | pixel);
                }
        }
        if (whole && (pose != spout.pose || w != spout.w || h != spout.h)) {
            spout.pose = pose;
            spout.w = w, spout.h = h;
            match_spout_columns(spout);
        }
        // Steady: one line higher while one line below the highest of the last 8 frames.
        std::move_backward(spout.ys, spout.ys + 7, spout.ys + 8);
        spout.ys[0] = y0;
        spout.seen = std::min(spout.seen + 1, 8);
        const int highest = *std::min_element(spout.ys, spout.ys + spout.seen);
        spout.y = y0 == highest + 1 ? highest : y0;
        now.push_back(std::move(spout));
    }
    g_spouts = std::move(now);
}

// Sets `vdp`'s overlays to the spouts as the loop shows them `back` game frames (0-1) before
// the current one.
void spout_overlays(Vdp& vdp, double back)
{
    vdp.overlays.clear();
    const double phase = std::fmod(g_spout_clock - back + kSpoutLoop * 1000.0, (double)kSpoutLoop) / (kSpoutLoop / 2);
    const double t = phase <= 1 ? phase : 2 - phase;    // 0: the pose, 1: its mirror image
    for (const SpoutMorph& spout : g_spouts) {
        if (spout.pose.empty())
            continue;                               // not seen whole yet: the game's sprites
        if (&vdp == &g_vdp) {
            vdp.hidden_sprites.resize(80);
            for (int index : spout.members)
                vdp.hidden_sprites[index] = 1;
        }
        const int w = spout.w, h = spout.h;
        std::vector<float> acc((size_t)w * h * 5, 0.0f);    // premultiplied r, g, b, alpha; weight
        for (int x = 0; x < w; x++)
            for (const auto& [i, j] : spout.pairs[x]) {
                const int ia = spout.pose[i * w + x], ib = spout.pose[j * w + (w - 1 - x)];
                const uint32_t ca = ia ? shown_rgb(ia) : 0, cb = ib ? shown_rgb(ib) : 0;
                const float aa = ia ? 1.0f : 0.0f, ab = ib ? 1.0f : 0.0f;
                const float alpha = (float)(aa * (1 - t) + ab * t);
                const double y = i * (1 - t) + j * t;
                const int y0 = (int)std::floor(y);
                const float f = (float)(y - y0);
                for (const auto& [yy, wt] : {std::pair<int, float>{y0, 1 - f}, {y0 + 1, f}}) {
                    if (yy < 0 || yy >= h || wt <= 0)
                        continue;
                    float* a = &acc[((size_t)yy * w + x) * 5];
                    for (int k = 0; k < 3; k++) {
                        const int shift = 16 - k * 8;
                        a[k] += (float)(((ca >> shift & 0xFF) * aa * (1 - t) + (cb >> shift & 0xFF) * ab * t)) * wt;
                    }
                    a[3] += alpha * wt;
                    a[4] += wt;
                }
            }
        Vdp::Overlay overlay;
        overlay.x = spout.x, overlay.y = spout.y, overlay.w = w, overlay.h = h, overlay.sprite = spout.sprite;
        overlay.argb.assign((size_t)w * h, 0);
        for (size_t p = 0; p < overlay.argb.size(); p++) {
            const float* a = &acc[p * 5];
            if (a[4] <= 0 || a[3] <= 0.01f)
                continue;
            const float alpha = std::min(1.0f, a[3] / a[4]);
            uint32_t c = (uint32_t)std::lround(alpha * 255) << 24;
            for (int k = 0; k < 3; k++)
                c |= (uint32_t)std::clamp((int)std::lround(a[k] / a[3]), 0, 255) << (16 - k * 8);
            overlay.argb[p] = c;
        }
        vdp.overlays.push_back(std::move(overlay));
    }
}

// Smooth Water: stage 25's water surface (plane A tiles 416 and 417, palette line 2) has two
// animation frames: every 16 frames the game rewrites the tiles with the other one (the crest
// low, then high). Once both have been seen, the frames in between are morphed column by
// column, as the spouts are (each column of one frame matched with the same column of the
// other, the matched pixels moving part of the way while their colour and opacity blend):
// the frame now moved towards the other one and the other one moved back, blended, so the last
// picture before a swap is the swap itself. Drawn through Vdp::tile_rgba.
struct TileSwap {
    int first_stage, last_stage;                    // game stage indexes ($FFFF42)
    std::vector<uint16_t> tiles;
    int palette_line;
    // per tile: its pixels now and in the other frame (palette values 0-15), and the matched
    // rows of each column (now -> other)
    std::vector<std::array<uint8_t, 64>> now, other;
    std::vector<std::array<std::vector<std::pair<int8_t, int8_t>>, 8>> pairs;
    bool known = false;                             // both frames seen
    int frame = 0, last_swap = -1000, period = 16;
};
TileSwap g_tile_swaps[] = {
    {23, 24, {416, 417}, 2},
};

std::array<uint8_t, 64> tile_values(int tile)
{
    std::array<uint8_t, 64> v;
    for (int i = 0; i < 64; i++) {
        const uint8_t pair = g_vdp.vram[(tile * 32 + (i >> 1)) & 0xFFFF];
        v[i] = i & 1 ? pair & 15 : pair >> 4;
    }
    return v;
}

// Matches each column of `a` with the same column of `b` (opacity and brightness, keeping the
// order of the pixels).
std::array<std::vector<std::pair<int8_t, int8_t>>, 8> match_tile_columns(const std::array<uint8_t, 64>& a,
                                                                         const std::array<uint8_t, 64>& b, int line)
{
    std::array<std::vector<std::pair<int8_t, int8_t>>, 8> out;
    auto feature = [&](uint8_t value, float& opacity, float& light) {
        opacity = value ? 1.0f : 0.0f;
        const uint32_t c = value ? shown_rgb(line * 16 + value) : 0;
        light = ((c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF)) / 765.0f;
    };
    for (int x = 0; x < 8; x++) {
        float cost[9][9];
        uint8_t from[9][9] = {};
        for (auto& row : cost)
            for (float& c : row)
                c = 1e9f;
        cost[0][0] = 0;
        for (int i = 1; i <= 8; i++)
            for (int j = 1; j <= 8; j++) {
                float oa, la, ob, lb;
                feature(a[(i - 1) * 8 + x], oa, la);
                feature(b[(j - 1) * 8 + x], ob, lb);
                const float d = std::abs(oa - ob) * 2 + std::abs(la - lb) + 0.02f * std::abs(i - j);
                const float best = std::min({cost[i - 1][j - 1], cost[i - 1][j], cost[i][j - 1]});
                cost[i][j] = d + best;
                from[i][j] = best == cost[i - 1][j - 1] ? 0 : best == cost[i - 1][j] ? 1 : 2;
            }
        for (int i = 8, j = 8; i > 0 && j > 0;) {
            out[x].push_back({(int8_t)(i - 1), (int8_t)(j - 1)});
            if (from[i][j] == 0) i--, j--;
            else if (from[i][j] == 1) i--;
            else j--;
        }
    }
    return out;
}

// Follows the swaps (their stages in play, Smooth Water on).
void update_tile_swaps(bool on, uint16_t state)
{
    const int stage = g_bus.ram[0xFF42] << 8 | g_bus.ram[0xFF43];
    for (TileSwap& swap : g_tile_swaps) {
        swap.frame++;
        if (!on || state != 0x0E || stage < swap.first_stage || stage > swap.last_stage) {
            swap.now.clear();
            swap.known = false;
            continue;
        }
        const size_t n = swap.tiles.size();
        std::vector<std::array<uint8_t, 64>> current(n);
        for (size_t k = 0; k < n; k++)
            current[k] = tile_values(swap.tiles[k]);
        if (swap.now.empty()) {
            swap.now = current;
            continue;
        }
        if (current == swap.now)
            continue;
        // a swap: back to the other frame (the cycle runs), or a first or new one
        const bool cycling = swap.known && current == swap.other;
        const int since = swap.frame - swap.last_swap;
        if (cycling && since >= 2 && since <= 64 && (since < swap.period || since > swap.period + 2))
            swap.period = since;
        swap.other = swap.now;
        swap.now = current;
        swap.last_swap = swap.frame;
        swap.known = true;
        swap.pairs.resize(n);
        for (size_t k = 0; k < n; k++)
            swap.pairs[k] = match_tile_columns(swap.now[k], swap.other[k], swap.palette_line);
    }
}

// Smooth Water: the cream at the bottom of stages 15 and 23 (game stage indexes 21 and 22) is
// one picture of 18 x 4 tiles (346-417, palette line 2, 144 x 32 pixels) repeated along plane A.
// The game's block animation rewrites the tiles every 12 frames with the next of three frames,
// each the last one moved 48 pixels left. In between, each row of the picture is matched with
// the same row of the next frame, pixels moving at most kCreamReach either way (keeping their
// order), so the bands sway side to side as the colours blend; the outline, a surface height
// per column, rises and falls in place. The cream stays opaque. Drawn through Vdp::tile_rgba.
constexpr int kCreamW = 144, kCreamH = 32, kCreamReach = 24;
constexpr uint16_t kCreamLayout[4][18] = {
    {346, 347, 350, 351, 354, 355, 370, 371, 374, 375, 378, 379, 394, 395, 398, 399, 402, 403},
    {348, 349, 352, 353, 356, 357, 372, 373, 376, 377, 380, 381, 396, 397, 400, 401, 404, 405},
    {358, 359, 362, 363, 366, 367, 382, 383, 386, 387, 390, 391, 406, 407, 410, 411, 414, 415},
    {360, 361, 364, 365, 368, 369, 384, 385, 388, 389, 392, 393, 408, 409, 412, 413, 416, 417}};
constexpr int kCreamLine = 2;

struct CreamFrame {
    std::array<uint8_t, kCreamW * kCreamH> values;  // palette values, 0 transparent
    std::array<uint8_t, kCreamW * kCreamH> filled;  // transparent pixels take the cream's below
    std::array<uint8_t, kCreamW> height;            // first opaque row per column
    int next = -1;                                  // the frame seen after this one
    std::array<std::vector<std::pair<int16_t, int16_t>>, kCreamH> pairs;   // rows matched with next's
};
struct CreamMorph {
    std::vector<CreamFrame> frames;
    int current = -1, frame = 0, last_swap = -1000, period = 12;
} g_cream;

CreamFrame cream_frame()
{
    CreamFrame f;
    for (int ty = 0; ty < 4; ty++)
        for (int tx = 0; tx < 18; tx++) {
            const auto v = tile_values(kCreamLayout[ty][tx]);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    f.values[(ty * 8 + y) * kCreamW + tx * 8 + x] = v[y * 8 + x];
        }
    f.filled = f.values;
    for (int x = 0; x < kCreamW; x++) {
        int top = 0;
        while (top < kCreamH && !f.values[top * kCreamW + x])
            top++;
        f.height[x] = (uint8_t)top;
        for (int y = 0; y < top && top < kCreamH; y++)
            f.filled[y * kCreamW + x] = f.values[top * kCreamW + x];
    }
    return f;
}

// Matches each row of `a` with the same row of `b` (by colour, the picture repeating along
// the row): pixel pairs (i, j) in order, |i - j| <= kCreamReach, i and j unwrapped.
void match_cream_rows(CreamFrame& a, const CreamFrame& b)
{
    constexpr int pad = 2 * kCreamReach, m = kCreamW + 2 * pad;
    std::vector<float> cost((m + 1) * (m + 1));
    std::vector<uint8_t> from((m + 1) * (m + 1));
    float ra[m][3], rb[m][3];
    for (int y = 0; y < kCreamH; y++) {
        for (int i = 0; i < m; i++) {
            const int x = ((i - pad) % kCreamW + kCreamW) % kCreamW;
            const uint8_t va = a.filled[y * kCreamW + x], vb = b.filled[y * kCreamW + x];
            const uint32_t ca = va ? shown_rgb(kCreamLine * 16 + va) : 0, cb = vb ? shown_rgb(kCreamLine * 16 + vb) : 0;
            for (int ch = 0; ch < 3; ch++) {
                ra[i][ch] = (float)(ca >> (16 - ch * 8) & 0xFF);
                rb[i][ch] = (float)(cb >> (16 - ch * 8) & 0xFF);
            }
        }
        std::fill(cost.begin(), cost.end(), 1e30f);
        cost[0] = 0;
        for (int i = 1; i <= m; i++)
            for (int j = std::max(1, i - kCreamReach); j <= std::min(m, i + kCreamReach); j++) {
                const float d = (std::abs(ra[i - 1][0] - rb[j - 1][0]) + std::abs(ra[i - 1][1] - rb[j - 1][1])
                                 + std::abs(ra[i - 1][2] - rb[j - 1][2])) / 255.0f + 0.01f * std::abs(i - j);
                const float c0 = cost[(i - 1) * (m + 1) + j - 1], c1 = cost[(i - 1) * (m + 1) + j],
                            c2 = cost[i * (m + 1) + j - 1];
                const float best = std::min({c0, c1, c2});
                cost[i * (m + 1) + j] = d + best;
                from[i * (m + 1) + j] = best == c0 ? 0 : best == c1 ? 1 : 2;
            }
        auto& pairs = a.pairs[y];
        pairs.clear();
        for (int i = m, j = m; i > 0 && j > 0;) {
            const int pi = i - 1 - pad, pj = j - 1 - pad;
            if (pi >= -kCreamReach && pi < kCreamW + kCreamReach)
                pairs.push_back({(int16_t)pi, (int16_t)pj});
            const uint8_t f = from[i * (m + 1) + j];
            if (f == 0) i--, j--;
            else if (f == 1) i--;
            else j--;
        }
    }
}

// Follows the cream's frames (stages 15 and 23 in play, Smooth Water on).
void update_cream(bool on, uint16_t state)
{
    const int stage = g_bus.ram[0xFF42] << 8 | g_bus.ram[0xFF43];
    CreamMorph& c = g_cream;
    c.frame++;
    if (!on || state != 0x0E || (stage != 21 && stage != 22)) {
        c.frames.clear();
        c.current = -1;
        return;
    }
    CreamFrame now = cream_frame();
    int index = -1;
    for (size_t i = 0; i < c.frames.size(); i++)
        if (c.frames[i].values == now.values)
            index = (int)i;
    if (index < 0) {
        if (c.frames.size() >= 8) {                 // not the three-frame cycle: start over
            c.frames.clear();
            c.current = -1;
        }
        c.frames.push_back(now);
        index = (int)c.frames.size() - 1;
    }
    if (index == c.current)
        return;
    if (c.current >= 0) {
        CreamFrame& last = c.frames[c.current];
        if (last.next != index) {
            last.next = index;
            match_cream_rows(last, c.frames[index]);
        }
        const int since = c.frame - c.last_swap;
        if (since >= 2 && since <= 64)
            c.period = since;
    }
    c.current = index;
    c.last_swap = c.frame;
}

// Adds the cream's tiles to `vdp`'s tile pictures, as shown `back` game frames (0-1) before
// the current one.
void set_cream(Vdp& vdp, double back)
{
    const CreamMorph& c = g_cream;
    if (c.current < 0 || c.frame - c.last_swap >= 2 * c.period)
        return;
    // From the frame shown towards the next; a moment before the last swap, from the one before.
    double since = c.frame - c.last_swap - back;
    int a = c.current;
    if (since < 0) {
        a = -1;
        for (size_t i = 0; i < c.frames.size(); i++)
            if (c.frames[i].next == c.current)
                a = (int)i;
        if (a < 0)
            return;
        since += c.period;
    }
    const CreamFrame& from = c.frames[a];
    if (from.next < 0)
        return;                                     // the next frame not seen yet
    const CreamFrame& to = c.frames[from.next];
    const double t = std::clamp(since / c.period, 0.0, 1.0);
    uint32_t rgb[16];
    for (int v = 0; v < 16; v++)
        rgb[v] = v ? shown_rgb(kCreamLine * 16 + v) : 0;
    std::vector<uint32_t> picture(kCreamW * kCreamH);
    for (int y = 0; y < kCreamH; y++) {
        float acc[kCreamW][3] = {}, weight[kCreamW] = {};
        for (const auto& [i, j] : from.pairs[y]) {
            const uint32_t ca = rgb[from.filled[y * kCreamW + (i + kCreamW) % kCreamW]];
            const uint32_t cb = rgb[to.filled[y * kCreamW + (j + kCreamW) % kCreamW]];
            const double x = i * (1 - t) + j * t;
            const int x0 = (int)std::floor(x);
            const float f = (float)(x - x0);
            for (const auto& [xx, w] : {std::pair<int, float>{x0, 1 - f}, {x0 + 1, f}}) {
                if (w <= 0)
                    continue;
                const int col = (xx % kCreamW + kCreamW) % kCreamW;
                for (int ch = 0; ch < 3; ch++) {
                    const int shift = 16 - ch * 8;
                    acc[col][ch] += w * (float)((ca >> shift & 0xFF) * (1 - t) + (cb >> shift & 0xFF) * t);
                }
                weight[col] += w;
            }
        }
        for (int x = 0; x < kCreamW; x++) {
            const int height = (int)std::lround(from.height[x] * (1 - t) + to.height[x] * t);
            if (y < height || weight[x] <= 0)
                continue;
            uint32_t p = 0xFF000000;
            for (int ch = 0; ch < 3; ch++)
                p |= (uint32_t)std::clamp((int)std::lround(acc[x][ch] / weight[x]), 0, 255) << (16 - ch * 8);
            picture[y * kCreamW + x] = p;
        }
    }
    vdp.tile_rgba_index.resize(2048, -1);
    for (int ty = 0; ty < 4; ty++)
        for (int tx = 0; tx < 18; tx++) {
            std::array<uint32_t, 64> tile;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    tile[y * 8 + x] = picture[(ty * 8 + y) * kCreamW + tx * 8 + x];
            vdp.tile_rgba_index[kCreamLayout[ty][tx]] = (int16_t)vdp.tile_rgba.size();
            vdp.tile_rgba.push_back(tile);
        }
}

// Sets `vdp`'s tile pictures to the swaps as shown `back` game frames (0-1) before the current one.
void set_tile_swaps(Vdp& vdp, double back)
{
    vdp.tile_rgba_index.clear();
    vdp.tile_rgba.clear();
    set_cream(vdp, back);
    for (const TileSwap& swap : g_tile_swaps) {
        if (!swap.known || swap.frame - swap.last_swap >= 2 * swap.period)
            continue;                               // not swapping (yet)
        // t: how far the frame shown is on its way to the other one; a moment before the last
        // swap is the end of the way from the other one
        const double since = swap.frame - swap.last_swap - back;
        const bool before = since < 0;
        const double t = std::clamp(before ? (swap.period + since) / swap.period : since / swap.period, 0.0, 1.0);
        vdp.tile_rgba_index.resize(2048, -1);
        for (size_t k = 0; k < swap.tiles.size(); k++) {
            const auto& from = before ? swap.other[k] : swap.now[k];
            const auto& to = before ? swap.now[k] : swap.other[k];
            std::array<float, 64 * 5> acc{};        // premultiplied r, g, b, alpha; weight
            for (int x = 0; x < 8; x++)
                for (const auto& [i0, j0] : swap.pairs[k][x]) {
                    // pairs map now -> other; the other way round before the last swap
                    const int i = before ? j0 : i0, j = before ? i0 : j0;
                    const int va = from[i * 8 + x], vb = to[j * 8 + x];
                    const uint32_t ca = va ? shown_rgb(swap.palette_line * 16 + va) : 0;
                    const uint32_t cb = vb ? shown_rgb(swap.palette_line * 16 + vb) : 0;
                    const float aa = va ? 1.0f : 0.0f, ab = vb ? 1.0f : 0.0f;
                    const double y = i * (1 - t) + j * t;
                    const int y0 = (int)std::floor(y);
                    const float f = (float)(y - y0);
                    for (const auto& [yy, w] : {std::pair<int, float>{y0, 1 - f}, {y0 + 1, f}}) {
                        if (yy < 0 || yy > 7 || w <= 0)
                            continue;
                        float* a = &acc[(yy * 8 + x) * 5];
                        for (int ch = 0; ch < 3; ch++) {
                            const int shift = 16 - ch * 8;
                            a[ch] += w * (float)((ca >> shift & 0xFF) * aa * (1 - t) + (cb >> shift & 0xFF) * ab * t);
                        }
                        a[3] += w * (float)(aa * (1 - t) + ab * t);
                        a[4] += w;
                    }
                }
            std::array<uint32_t, 64> picture{};
            for (int p = 0; p < 64; p++) {
                const float* a = &acc[p * 5];
                if (a[4] <= 0 || a[3] <= 0.01f)
                    continue;
                const float alpha = std::min(1.0f, a[3] / a[4]);
                uint32_t c = (uint32_t)std::lround(alpha * 255) << 24;
                for (int ch = 0; ch < 3; ch++)
                    c |= (uint32_t)std::clamp((int)std::lround(a[ch] / a[3]), 0, 255) << (16 - ch * 8);
                picture[p] = c;
            }
            vdp.tile_rgba_index[swap.tiles[k]] = (int16_t)vdp.tile_rgba.size();
            vdp.tile_rgba.push_back(picture);
        }
    }
}

// The colours as CRAM holds them, as a palette override (when Smooth Fades isn't making one).
void fill_plain_palette(PaletteOverride& out, const uint16_t* cram)
{
    auto scale = [](int v) { return (uint32_t)(v * 255 / 7); };
    for (int i = 0; i < 64; i++) {
        const uint16_t c = cram[i];
        out.source[i] = c;
        out.rgb[i] = 0xFF000000 | scale((c >> 1) & 7) << 16 | scale((c >> 5) & 7) << 8 | scale((c >> 9) & 7);
    }
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
    v.extend_plane_a_inside = g_vdp.extend_plane_a_inside;
    v.extend_plane_b = g_vdp.extend_plane_b;
    v.extend_plane_b_inside = g_vdp.extend_plane_b_inside;
    v.strip_b_priority_right = g_vdp.strip_b_priority_right;
    v.strip_fill_in_front = g_vdp.strip_fill_in_front;
    v.strip_fill_in_front_left = g_vdp.strip_fill_in_front_left;
    v.strip_cells = g_vdp.strip_cells;
    v.strip_cells_x = g_vdp.strip_cells_x;
    v.strip_cells_y = g_vdp.strip_cells_y;
    v.strip_cells_w = g_vdp.strip_cells_w;
    v.clean_black_edges = g_vdp.clean_black_edges;
    v.edge_tiles = g_vdp.edge_tiles;
    v.water_tiles = g_vdp.water_tiles;
    v.hidden_sprites = g_vdp.hidden_sprites;
    spout_overlays(v, back);
    set_flow_colours(v, g_flow_rgb_back, back);
    set_tile_swaps(v, back);
    std::memcpy(v.edge_palette, g_vdp.edge_palette, sizeof(v.edge_palette));
    v.sprite_limit = g_vdp.sprite_limit;
    v.palette = nullptr;
    if (g_vdp.palette) {
        if (g_smooth_fades)
            g_fade.fill(g_fade_palette_back, back);
        else
            fill_plain_palette(g_fade_palette_back, g_vdp.cram);
        for (const WaterCycle& cycle : g_water_cycles)
            cycle.apply(g_fade_palette_back, back);
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

// Stage 26 (level 4, act 2): the spawn grid ($FFCA00, see docs/widescreen.md) files a yellow
// gear (object $0D at X 720) and an enemy that waits for the player ($3F at X 688) under the
// cell for X 768-895 instead of their own, so they spawn a cell late, and the gear popped in
// at the widened view's right edge (on the console too, 40 pixels further in, mostly off
// screen). In widescreen, file their list under their own cell. The grid is rebuilt
// whenever the level loads, so this checks the bytes each frame and is done once per load.
void fix_spawn_grid(uint16_t level, uint16_t act)
{
    uint8_t* grid = &g_bus.ram[0xCA00];
    constexpr int kCell5 = 4 + 10 * 8 + 5, kCell6 = kCell5 + 1;   // cells (5, 10) and (6, 10)
    if (level != 4 || act != 2 || grid[2] != 8 || grid[3] != 11 || grid[kCell5] != 0 || grid[kCell6] != 1)
        return;
    const uint16_t lists = grid[0] << 8 | grid[1];
    const uint8_t* list = grid + lists + ((grid[lists] << 8) | grid[lists + 1]);   // list 1
    if (list[2] != 0x0D || (list[8] << 8 | list[9]) != 720)  // its first entry: the gear at X 720
        return;
    grid[kCell5] = 1;
    grid[kCell6] = 0;
}

// Spawn entries the widescreen SpawnCell patch marked missed (bit 1 of the spawned byte, no
// free slot in the object's pool): spawned here, as SpawnCell would, once a slot is free,
// while the entry is still inside the range objects live in (DrawObject's deletion limits:
// X from 168 left of the camera to 552 right, Y from 128 above to 512 below). Out of that
// range the mark is dropped, and a later crossing spawns the entry as usual.
void retry_missed_spawns()
{
    uint8_t* ram = g_bus.ram;
    const uint8_t* grid = &ram[0xCA00];
    const int width = grid[2], height = grid[3];
    const uint16_t lists = grid[0] << 8 | grid[1];
    if (!lists || width * height == 0 || 4 + width * height > lists)
        return;
    const int camera_x = (int16_t)(ram[0xFA84] << 8 | ram[0xFA85]);
    const int camera_y = (int16_t)(ram[0xFA88] << 8 | ram[0xFA89]);
    for (int cell = 0; cell < width * height; cell++) {
        const int list = grid[4 + cell];
        if (!list)
            continue;
        const uint32_t at = 0xCA00 + lists + 2 * (list - 1);
        const uint32_t start = 0xCA00 + lists + (ram[at & 0xFFFF] << 8 | ram[(at + 1) & 0xFFFF]);
        const int count = ram[start & 0xFFFF] + 1;
        for (int i = 0; i < count; i++) {
            uint8_t* e = &ram[(start + 1 + i * 9) & 0xFFFF];   // flags, ID, setting, spawned, count, Y, X
            if ((e[3] & 3) != 2)
                continue;
            const int dx = (e[7] << 8 | e[8]) - camera_x, dy = (e[5] << 8 | e[6]) - camera_y;
            if (dx < -168 || dx >= 552 || dy < -128 || dy >= 512) {
                e[3] &= ~2;
                continue;
            }
            if ((e[0] & 0x20) && !e[4])
                continue;                           // SpawnCell's own condition
            uint32_t slot = e[0] & 0x80 ? 0xD000 : e[0] & 0x40 ? 0xDD80 : 0xD400;
            const int slots = e[0] & 0x80 ? 32 : e[0] & 0x40 ? 10 : 32;
            int n = 0;
            while (n < slots && (ram[slot] | ram[slot + 1]))
                slot += 0x40, n++;
            if (n == slots)
                continue;
            const uint16_t spawned = (uint16_t)((start + 1 + i * 9 + 3) & 0xFFFF);
            ram[slot + 0x01] = e[1];
            ram[slot + 0x29] = e[2];
            ram[slot + 0x24] = spawned >> 8, ram[slot + 0x25] = spawned & 0xFF;
            ram[slot + 0x14] = e[5], ram[slot + 0x15] = e[6];
            ram[slot + 0x10] = e[7], ram[slot + 0x11] = e[8];
            e[3] = 1;
        }
    }
}

#ifndef NDEBUG
// --sprite-capture DIR (Debug builds, headless): for each sprite set an object shows (its
// mapping table at $6 and tile base at $4), once the object has been alive for 8 frames,
// save VRAM, CRAM and the palette at $FFF800 (what fades move towards) as DIR/capN.vram,
// .cram and .pal, and add a line to DIR/captures.txt
// (N, stage index, object ID, mapping address, tile base, RAM slot, frame). The animation
// frames are drawn from these by a script outside the repository.
void save_state(const std::string& ram_path, const std::string& vram_path, const std::string& cram_path);

void capture_sprites(const std::string& dir, int frame)
{
    struct Slot { uint32_t key = 0; int age = 0; };
    static Slot slots[128];
    static std::vector<uint64_t> seen, anims_seen;
    static int captures = 0;
    auto w = [](uint32_t a) { return (uint32_t)(g_bus.ram[a & 0xFFFF] << 8 | g_bus.ram[(a + 1) & 0xFFFF]); };
    const int stage = g_bus.ram[0xFF43];
    for (int i = 0; i < 128; i++) {
        const uint32_t a = 0xD000 + i * 0x40;
        const uint32_t id = w(a) & 0xFF, mapping = w(a + 6) << 16 | w(a + 8), base = w(a + 4);
        const uint32_t key = id ? (mapping ^ (base << 8) ^ (id << 24)) : 0;
        Slot& s = slots[i];
        s.age = key && key == s.key ? s.age + 1 : 0;
        s.key = key;
        // The animations each set really plays (byte $A), to DIR/anims.txt: several kinds of
        // object can share one mapping table, each with its own tile base.
        if (key && mapping && mapping < 0x80000) {
            const uint64_t anim = (uint64_t)stage << 56 | (uint64_t)id << 48 | (uint64_t)base << 32
                                  | (uint64_t)g_bus.ram[(a + 0xA) & 0xFFFF] << 24 | mapping;
            if (std::find(anims_seen.begin(), anims_seen.end(), anim) == anims_seen.end()) {
                anims_seen.push_back(anim);
                if (FILE* f = std::fopen((dir + "/anims.txt").c_str(), "a")) {
                    std::fprintf(f, "%d %02X %06X %04X %d\n", stage, id, mapping, base, g_bus.ram[(a + 0xA) & 0xFFFF]);
                    std::fclose(f);
                }
            }
        }
        if (s.age != 8 || mapping == 0 || mapping >= 0x80000 || (mapping & 1))
            continue;
        const uint64_t full = (uint64_t)stage << 56 | (uint64_t)id << 48 | (uint64_t)base << 32 | mapping;
        if (std::find(seen.begin(), seen.end(), full) != seen.end())
            continue;
        seen.push_back(full);
        const std::string name = dir + "/cap" + std::to_string(++captures);
        save_state("", name + ".vram", name + ".cram");
        if (FILE* f = std::fopen((name + ".pal").c_str(), "wb")) {   // the palette fades move towards
            std::fwrite(&g_bus.ram[0xF800], 1, 128, f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((dir + "/captures.txt").c_str(), "a")) {
            std::fprintf(f, "%d %d %02X %06X %04X %04X %d\n", captures, stage, id, mapping, base, a, frame);
            std::fclose(f);
        }
    }
}
#endif

#ifdef COI_TIMING_LOG
// Performance check (logged with the frame timings in main; Android builds made with
// -DCOI_TIMING_LOG=ON only): time spent drawing lines (VDP), running the 68000, and running
// the Z80 and sound chips.
Uint64 g_time_video = 0, g_time_cpu = 0, g_time_sound = 0;
int g_lock_changes = 0;
int g_audio_min = 1 << 30, g_audio_max = 0, g_audio_clears = 0;   // sound queued, in bytes

// The GPU's own time for each picture's drawing (GL_EXT_disjoint_timer_query), read back a few
// pictures later so the CPU never waits for it: the total and the longest per log line.
struct GpuTimer {
    static constexpr GLenum kTimeElapsed = 0x88BF;  // GL_TIME_ELAPSED_EXT
    static constexpr int kQueries = 4;
    GLuint queries[kQueries] = {};
    bool pending[kQueries] = {};
    int next = 0;
    bool available = false;
    uint64_t total = 0, longest = 0;
    int count = 0;

    void init()
    {
        GLint extensions = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
        for (GLint i = 0; i < extensions; i++)
            if (std::strcmp((const char*)glGetStringi(GL_EXTENSIONS, i), "GL_EXT_disjoint_timer_query") == 0)
                available = true;
        if (available)
            glGenQueries(kQueries, queries);
    }
    void begin()
    {
        if (!available)
            return;
        if (pending[next]) {                        // the oldest, if done: count it
            GLuint done = 0;
            glGetQueryObjectuiv(queries[next], GL_QUERY_RESULT_AVAILABLE, &done);
            if (done) {
                GLuint ns = 0;
                glGetQueryObjectuiv(queries[next], GL_QUERY_RESULT, &ns);
                total += ns;
                longest = std::max<uint64_t>(longest, ns);
                count++;
            }
            pending[next] = false;
        }
        glBeginQuery(kTimeElapsed, queries[next]);
    }
    void end()
    {
        if (!available)
            return;
        glEndQuery(kTimeElapsed);
        pending[next] = true;
        next = (next + 1) % kQueries;
    }
};
GpuTimer g_gpu_timer;
#define TIMED(total, work) do { const Uint64 t0_ = SDL_GetTicksNS(); work; total += SDL_GetTicksNS() - t0_; } while (0)
#else
#define TIMED(total, work) work
#endif

#ifdef COI_NATIVE
Uint64 g_cpu_time = 0;      // time running the 68000 (interpreted or translated), for headless runs
int64_t g_cpu_cycles = 0, g_native_cycles = 0;      // 68000 cycles run in all, and by translated code
std::map<uint32_t, int64_t> g_interpreted;          // --native: where the interpreter ran, and how often
#endif

#ifdef COI_NATIVE
void native_poll();
int cpu_execute(int cycles);
void native_reset();
uint32_t cpu_reg(m68k_register_t r);
void native_sync();
void cpu_reset();
#else
void cpu_reset() { m68k_pulse_reset(); }            // (without the translator, Musashi's own)
#endif

// Refilling the planes after widescreen is switched on during level play. The game streams
// one block column (16 pixels) whenever a camera's X crosses a multiple of 16, keeping each
// plane loaded with the 32 block columns from X/16 - 7 to X/16 + 24 (block b in plane
// columns 2(b mod 32) and the one after). The switch raises the camera limits, and the next
// UpdateCamera moves the camera onto them (0 to 40 at a stage's start) in one step, which
// streams at most one column: blocks of the new range were never loaded, and showed stale
// tiles once the view reached them. So once the camera has moved, the blocks of the new
// range that were not in the old one are written as the game's own streaming writes them
// (StreamColumn, $00F9B4: map bytes from +$8, 8-byte blocks from +$10, 32 rows from
// (Y - 112)/16), straight into VRAM.
struct PlaneRefill {
    bool pending = false;
    int16_t old_x[2] = {};                          // CameraA and CameraB X at the switch
};
PlaneRefill g_plane_refill;

void write_block_column(uint32_t camera, uint32_t plane, int block)
{
    auto word = [](uint32_t a) { return (int16_t)(g_bus.ram[a] << 8 | g_bus.ram[a + 1]); };
    auto ptr = [](uint32_t a) {
        return (uint32_t)(g_bus.ram[a] << 24 | g_bus.ram[a + 1] << 16 | g_bus.ram[a + 2] << 8 | g_bus.ram[a + 3]);
    };
    const uint32_t map = ptr(camera + 0x08), table = ptr(camera + 0x10);
    const int width = (uint16_t)word(camera + 0x0E);
    const int row = (int16_t)(word(camera) - 0x70) >> 4;
    // a1 = map + block + row * width, the product added as a word (adda.w), as the game does.
    uint32_t a1 = map + (int16_t)block + (int16_t)(uint16_t)((uint16_t)row * width);
    // Plane A upside down ($FFFF9801 set): rows read from 25 below upwards, the block's
    // halves swapped and flipped vertically ($00FA52).
    const bool flipped = camera == 0xFA80 && g_bus.ram[0x9801];
    if (flipped)
        a1 += (uint32_t)(width * 0x19);
    const int column = (block & 0x1F) * 2;
    for (int i = 0; i < 32; i++) {
        const int slot = (row + i) & 0x1F;
        const uint32_t b = table + g_bus.read8(a1) * 8;
        uint16_t w[4];
        for (int k = 0; k < 4; k++)
            w[k] = g_bus.read16(b + k * 2);
        // Left column top and bottom, right column top and bottom; upside down the top and
        // bottom swap and each cell is flipped.
        uint16_t cells[4] = {w[0], w[2], w[1], w[3]};
        if (flipped) {
            const uint16_t f[4] = {w[2], w[0], w[3], w[1]};
            for (int k = 0; k < 4; k++)
                cells[k] = f[k] ^ 0x1000;
        }
        for (int k = 0; k < 4; k++) {
            const uint32_t a = plane + ((slot * 2 + (k & 1)) * 64 + column + (k >> 1)) * 2;
            g_vdp.vram[a] = (uint8_t)(cells[k] >> 8);
            g_vdp.vram[a + 1] = (uint8_t)cells[k];
        }
        a1 += flipped ? (uint32_t)-width : (uint32_t)width;
    }
}

void refill_planes_if_due()
{
    if (!g_plane_refill.pending)
        return;
    auto word = [](int a) { return (int16_t)(g_bus.ram[a] << 8 | g_bus.ram[a + 1]); };
    if ((g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301]) != 0x0E || !g_widescreen) {
        g_plane_refill.pending = false;             // the level has gone (the next one fills itself) or narrow again
        return;
    }
    // Wait until CameraA is inside its new limits: UpdateCamera does not run in every frame
    // (at a stage's start it moves the camera about 18 frames after the level appears).
    const int x = word(0xFA84);
    if (x < word(0xFAA2) || x > word(0xFAA6))
        return;
    g_plane_refill.pending = false;
    for (int i = 0; i < 2; i++) {
        const uint32_t camera = i ? 0xFAC0 : 0xFA80;
        if (i && (g_bus.ram[0x9800] & 1))
            continue;                               // plane B holds a fixed picture, never streamed
        const int now = word(camera + 4) >> 4, then = g_plane_refill.old_x[i] >> 4;
        for (int block = now - 7; block <= now + 24; block++)
            if (block < then - 7 || block > then + 23)
                write_block_column(camera, i ? 0xE000 : 0xC000, block);
    }
}

// Smooth Water: the tiles that draw water as a dithered pattern on plane A, by the game's
// stage index ($FFFF42), and the pixel values (bit p for value p) in them that are water.
// All are drawn with high priority, in front of the sprites.
// Smooth Glass uses the same drawing for the library's glass jars, under its own setting.
struct WaterTiles {
    uint8_t first_stage, last_stage;
    uint16_t first, last;
    uint32_t values;                                // Vdp::water_tiles: water, and (<< 16) behind it
    bool glass;                                     // Smooth Glass rather than Smooth Water
};
constexpr WaterTiles kWaterTiles[] = {
    {5, 8, 756, 796, 1 << 10, false},               // stages 6-9: the jello pits (value 10: a checkerboard
                                                    // in the middle, solid at the rims)
    {9, 9, 491, 502, 1 << 10, false},               // stage 10: foam where the cave falls land (checkerboard)
    {10, 12, 330, 345, 0xFFFE, false},              // stages 11-13: waterfalls (streaks with gaps), and
    {10, 12, 346, 357, 0xFF0000FE, false},          // where they cross or end a ledge (streaks in values
                                                    // 1-7, the ledge in 8-15)
    {13, 17, 480, 482, 1 << 14, true},              // stages 14 and 16-19: the glass jars (value 14,
    {13, 17, 485, 486, 1 << 14, true},              // a checkerboard in the middle, solid at the rim)
    {13, 17, 490, 490, 1 << 14, true},
    {18, 20, 640, 643, 1 << 2, false},              // stages 20-22: swimming in the tea (checkerboard
                                                    // over the whole picture)
    {23, 24, 410, 413, 1 << 4, false},              // stages 24-25: the flooded castle (checkerboard)
    {23, 24, 418, 421, 1 << 7, false},
};

// Sets g_vdp.water_tiles for the stage being played (empty outside level play, in stages
// without water or glass, or with Smooth Water and Smooth Glass off).
void update_water_tiles(bool water, bool glass, uint16_t state)
{
    const int stage = state == 0x0E ? g_bus.ram[0xFF42] << 8 | g_bus.ram[0xFF43] : -1;
    const int key = stage * 4 + water * 2 + glass;
    static int shown = -2;
    if (key == shown)
        return;
    shown = key;
    g_vdp.water_tiles.clear();
    for (const WaterTiles& w : kWaterTiles)
        if (stage >= w.first_stage && stage <= w.last_stage && (w.glass ? glass : water)) {
            g_vdp.water_tiles.resize(2048);
            for (int t = w.first; t <= w.last; t++)
                g_vdp.water_tiles[t] = w.values;
        }
}

void run_frame(uint32_t* pixels)
{
#ifdef COI_NATIVE
    native_poll();                                  // a ROM image translated meanwhile (--jit)
#endif
    refill_planes_if_due();
    g_audio.clear();
    g_vdp.start_frame();
    const uint16_t state = g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301];
    const int stage_index = g_bus.ram[0xFF42] << 8 | g_bus.ram[0xFF43];
    bool cycling = false;
    for (WaterCycle& cycle : g_water_cycles) {
        cycle.update(g_smooth_water && state == 0x0E && stage_index >= 10 && stage_index <= 12, g_vdp.cram);
        cycling |= cycle.on;
    }
    if (g_smooth_fades) {                           // CRAM holds this frame's colours by now
        g_fade.update(g_vdp.cram);
        g_fade.fill(g_fade_palette);
        g_vdp.palette = &g_fade_palette;
    } else {
        g_fade.reset();
        g_vdp.palette = nullptr;
        if (cycling) {
            fill_plain_palette(g_fade_palette, g_vdp.cram);
            g_vdp.palette = &g_fade_palette;
        }
    }
    if (g_vdp.palette)
        for (const WaterCycle& cycle : g_water_cycles)
            cycle.apply(g_fade_palette, 0.0);
    const uint16_t level = g_bus.ram[0xFF44] << 8 | g_bus.ram[0xFF45];
    const uint16_t act = g_bus.ram[0xFF46] << 8 | g_bus.ram[0xFF47];
    const bool wide = g_widescreen && is_wide_state(state, level, g_bus.ram);
    if (g_widescreen)
        fix_spawn_grid(level, act);
    if (state == 0x0E)
        retry_missed_spawns();
    // $20/$22 pick the castle exterior for level 5, or for level 3 when $FFFF0D is 2
    // (the scene table at $07AB74).
    const bool exterior = level == 5 || (g_bus.ram[0xFF0D] == 2 && level == 3);
    if (state == 0x22 && exterior && exterior_in_plane_a()) {
        g_strip_rule = StripRule::TowerFill;
        g_strip_camera_x = (int16_t)(g_bus.ram[0xFA84] << 8 | g_bus.ram[0xFA85]);
    }
    else if (state == 0x0E && level == 1 && act == 3) {
        g_strip_rule = StripRule::ArenaWalls;
        g_wall = kLevel2BossWalls;
    } else if ((state == 0x0E || state == 0x2E) && level == 5 && act == 0) {
        g_strip_rule = StripRule::ArenaWalls;
        g_wall = kFinalBossWalls;
    } else if (state == 0x16 && after_ending_scene(g_bus.ram) == AfterEnding::Theatre) {
        g_strip_rule = StripRule::TheatreFill;
    } else if (state == 0x2A) {
        g_strip_rule = StripRule::StoryFill;
    } else {
        g_strip_rule = StripRule::None;
    }
    if (g_strip_rule == StripRule::ArenaWalls) {
        update_wall_cells();
        g_vdp.strip_cells = g_wall_cells;
        g_vdp.strip_cells_x = g_wall.x / 8;
        g_vdp.strip_cells_y = g_wall.y / 8;
        g_vdp.strip_cells_w = g_wall.w / 8;
    } else {
        g_wall_cells.clear();
        g_vdp.strip_cells.clear();
    }
    // In the ending ($2E, in the final arena) the characters run out past the left edge:
    // draw the walls in front of them so they exit behind the wall, not through it.
    g_vdp.strip_fill_in_front = state == 0x2E && g_strip_rule == StripRule::ArenaWalls;
    // The castle exterior's left fill likewise covers Mickey (and the sprites the scene
    // keeps there) before he comes out of the door.
    g_vdp.strip_fill_in_front_left = g_strip_rule == StripRule::TowerFill;
    g_vdp.extend_plane_a_inside = wide && g_strip_rule == StripRule::TowerFill;
    g_vdp.extend_plane_b_inside = wide && g_strip_rule == StripRule::TheatreFill;
    g_vdp.strip_b_priority_right = g_strip_rule == StripRule::StoryFill;
    g_vdp.clean_black_edges = g_clean_rock_edges && state == 0x0E && level == 0;   // forest stages
    update_water_tiles(g_smooth_water, g_smooth_glass, state);
    update_spouts(g_smooth_water, state);
    spout_overlays(g_vdp, 0.0);
    update_flows(g_smooth_water, state);
    set_flow_colours(g_vdp, g_flow_rgb, 0.0);
    update_tile_swaps(g_smooth_water, state);
    update_cream(g_smooth_water, state);
    set_tile_swaps(g_vdp, 0.0);
    for (int i = 0; i < 64; i++)
        g_vdp.edge_palette[i] = g_bus.ram[0xF800 + i * 2] << 8 | g_bus.ram[0xF801 + i * 2];
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
            TIMED(g_time_video, render_line(g_vdp, line, pixels + line * g_out_width * g_scale, wide));
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
#ifdef COI_NATIVE
        const Uint64 cpu_start = SDL_GetTicksNS();
#endif
#ifdef COI_NATIVE
        TIMED(g_time_cpu, g_cpu_cycles += cpu_execute(kCyclesPerLine));
        g_cpu_time += SDL_GetTicksNS() - cpu_start;
#else
        TIMED(g_time_cpu, m68k_execute(kCyclesPerLine));
#endif
        TIMED(g_time_sound, g_sound.run(kZ80CyclesPerLine, g_audio));
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
    set_irq(0);
    cpu_reset();
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

// The settings that patch the ROM, and the image they make. `report`: patches skipped go to
// stderr (the image running; not the ones translated ahead).
RomSettings current_rom_settings()
{
    return {g_widescreen, g_smooth_swing, g_cheats};
}

std::vector<uint8_t> make_rom(const RomSettings& s, bool report)
{
    return make_rom_image(g_rom_original, s, report);
}

// --save-rom FILE: the ROM as this run's settings patch it; --save-rom-family DIR: the image
// family the readable C is generated from (rom_family.h; the build's coi_family does the same).
std::string g_save_rom_path, g_save_rom_family;

#ifdef COI_NATIVE_C
}  // namespace (the C translation's symbols are the generated C's own)
extern "C" {
// The C translation linked in (-DCOI_NATIVE=ON, or the readable C with COI_READABLE): its code,
// and for the readable C the hashes of its image family (the image with no setting on, then each
// setting's image alone, in kRomSettings' order; none for a translation of one image).
extern const CoiNativeCode coi_native_c;
extern const uint64_t coi_native_family[];
extern const int coi_native_family_count;
uint32_t coi_native_settings = 0;       // (readable_rt.h) the readable C's settings bits
}
namespace {

#define COI_NATIVE_BUILT_IN (&coi_native_c)

uint64_t rom_hash(const std::vector<uint8_t>& rom);
uint64_t running_rom_hash();

// The family's code fits the ROM as patched now if the image with no settings is the one it came
// from and so is each setting's on its own (the settings never touch the same bytes, so any mix
// of them is these together); coi_native_settings then picks the variants. Each image is built
// and hashed once.
bool native_family_matches()
{
    static std::vector<int> known;              // per image: 0 not yet, 1 the same, -1 not
    if (known.empty()) known.assign(size_t(coi_native_family_count), 0);
    auto same = [&](size_t k, const RomSettings& s) {
        if (k >= known.size()) return false;
        if (!known[k]) known[k] = rom_hash(make_rom(s, false)) == coi_native_family[k] ? 1 : -1;
        return known[k] > 0;
    };
    if (!same(0, RomSettings{})) return false;
    const RomSettings now = current_rom_settings();
    const uint32_t mask = rom_settings_mask(now);
    for (size_t k = 0; k < kRomSettingCount; k++)
        if (mask & (1u << k)) {
            RomSettings one{};
            kRomSettings[k].field(one) = true;
            if (!same(k + 1, one)) return false;
        }
    // (and the ROM running is that image: nothing else, an MD+ pack's hooks say, patched it)
    static std::map<uint32_t, uint64_t> composed;
    if (!composed.count(mask)) composed[mask] = rom_hash(make_rom(now, false));
    if (running_rom_hash() != composed[mask]) return false;
    coi_native_settings = mask;
    return true;
}
#endif

#ifdef COI_NATIVE_DEV
#include "native_dev.inc"           // (local development builds: port/native_dev.cmake)
#endif

#ifdef COI_NATIVE
// COI_NO_INTERPRETER: translated code is the only CPU (no Musashi linked): always --native --jit,
// translated at once, CPU state only ever ours, and anything it can't run stops the game with a
// message (native_fatal).
#ifdef COI_NO_INTERPRETER
constexpr bool kNoInterpreter = true;
#else
constexpr bool kNoInterpreter = false;
#endif
// The 68000's cycle tables as Musashi counts them (recomp/cycles68000.c, written by a development
// build with --write-cycles and checked against Musashi's at start-up there).
extern "C" const uint8_t coi_exception_cycles_68000[256];
extern "C" void coi_cycles_68000(uint8_t* out);
// The translated code the port runs: the app's own (--jit, always on), unless a development
// build links a translation in.
#ifdef COI_NATIVE_BUILT_IN
const CoiNativeCode* g_native_code = COI_NATIVE_BUILT_IN;
bool g_native_jit = false;
#else
const CoiNativeCode* g_native_code = nullptr;
bool g_native_jit = true;
#endif
bool g_native_rom_ok = false;       // the ROM as patched now is the one the code was translated from
const bool g_native_strict = kNoInterpreter        // (without an interpreter, as strict as can be)
    || (std::getenv("COI_NATIVE_STRICT") && std::atoi(std::getenv("COI_NATIVE_STRICT")));  // (see strict_check)
std::map<uint32_t, double> g_on_demand;     // translated on demand (native_on_demand): address -> ms
std::set<uint32_t> g_on_demand_failed;      // addresses that couldn't be translated on demand

// An image's hash, for matching it with translations: hashing takes a few milliseconds on a
// phone, so the image running is hashed once each time it's built (build_rom clears it).
uint64_t rom_hash(const std::vector<uint8_t>& rom)
{
    return coi_native_hash(rom.data(), uint32_t(rom.size()));
}
bool g_rom_hash_known = false;
uint64_t g_rom_hash = 0;

uint64_t running_rom_hash()
{
    if (!g_rom_hash_known) {
        g_rom_hash = rom_hash(g_bus.rom);
        g_rom_hash_known = true;
    }
    return g_rom_hash;
}

bool native_rom_matches(const CoiNativeCode* code, size_t size, uint64_t hash)
{
#ifdef COI_NATIVE_C
    if (code == &coi_native_c && coi_native_family_count > 0)     // (the readable C: by its images)
        return native_family_matches();
#endif
    return code && size == code->rom_size && hash == code->rom_hash;
}

bool native_rom_matches(const CoiNativeCode* code)
{
    return native_rom_matches(code, g_bus.rom.size(), running_rom_hash());
}

// --jit: the configuration and the tracer's lists (read once, at start-up), and a translation
// of a ROM image with them.
struct JitInput {
    recomp::Config config;
    std::vector<uint32_t> list;
    bool trace = false;
};
JitInput g_jit_input;
struct JitResult {
    const CoiNativeCode* code = nullptr;
    size_t functions = 0;
    std::string error;
    std::map<uint32_t, std::string> failed;     // functions left out, and why
    std::set<uint32_t> unresolved;              // computed jumps and calls left to the interpreter
    double ms = 0;                              // how long translating took
    bool known = false;                         // (ahead) the image has a translation already
};

JitResult jit_translate(const JitInput& input, const std::vector<uint8_t>& bytes, bool arm64, uint32_t frame = 0)
{
    const Uint64 t0 = SDL_GetTicksNS();
    JitResult r;
    try {
        const recomp::Rom rom{bytes};
        recomp::Program program(rom, input.config);
        program.add_all(input.list, nullptr);
        r.functions = program.functions().size();
        r.failed = program.failed();
        r.unresolved = program.unresolved();
        r.code = arm64 ? recomp::translate_arm64(program, input.trace, &r.error, frame)
                       : recomp::translate_x64(program, input.trace, &r.error, frame);
    } catch (const std::exception& ex) {
        r.error = ex.what();
    }
    r.ms = (SDL_GetTicksNS() - t0) / 1e6;
    return r;
}

// Every translation made so far, one per ROM image (the settings that patch the ROM: widescreen,
// smooth swinging, cheats). When the player changes one in the game, the new image is
// translated on a thread while the interpreter runs it, and its code runs from the first frame
// after that; going back to an image translated before switches at once.
#if defined(__aarch64__) || defined(_M_ARM64)
constexpr bool kArm64 = true;
#else
constexpr bool kArm64 = false;
#endif
std::vector<const CoiNativeCode*> g_native_variants;
std::future<JitResult> g_native_pending;
bool g_native_variants_on = false;          // --jit in the game: translate each image met

// Translating ahead: once the image running has its code, every image one setting away from it
// (widescreen, smooth swinging, each cheat switched) is translated on the thread too, one at a
// time, so switching a setting in the game finds its code ready (without an interpreter,
// translating it then would stop the game for a moment). Up to kAheadVariants images are kept.
constexpr size_t kAheadVariants = 32;
std::vector<RomSettings> g_native_ahead;    // still to translate, the last first
const CoiNativeCode* g_native_ahead_for = nullptr;  // the code they're one setting away from

void native_ahead_queue()
{
    if (g_native_ahead_for == g_native_code)
        return;
    g_native_ahead_for = g_native_code;
    g_native_ahead.clear();
    const RomSettings s = current_rom_settings();
    std::vector<RomSettings> next;
    for (bool Cheats::* c : {&Cheats::midair_jump, &Cheats::moon_jump, &Cheats::walk_faster, &Cheats::full_power,
                             &Cheats::infinite_continues, &Cheats::infinite_items, &Cheats::infinite_tries,
                             &Cheats::infinite_power, &Cheats::invincibility}) {
        RomSettings t = s;
        t.cheats.*c = !(t.cheats.*c);
        if (t.cheats.*c && c == &Cheats::moon_jump)     // (as the Cheats menu: they patch the same code)
            t.cheats.midair_jump = false;
        if (t.cheats.*c && c == &Cheats::midair_jump)
            t.cheats.moon_jump = false;
        next.push_back(t);
    }
    if (g_smooth_swing_available) {
        RomSettings t = s;
        t.smooth_swing = !t.smooth_swing;
        next.push_back(t);
    }
    if (g_wide_available) {                     // (the likeliest switch: translated first)
        RomSettings t = s;
        t.widescreen = !t.widescreen;
        next.push_back(t);
    }
    g_native_ahead = std::move(next);
}

// The thread free: the next image ahead not translated yet, if any.
void native_ahead_next()
{
    if (g_native_pending.valid() || g_native_ahead.empty() || g_native_variants.size() >= kAheadVariants)
        return;
    std::vector<std::pair<size_t, uint64_t>> known;
    for (const CoiNativeCode* v : g_native_variants)
        known.emplace_back(v->rom_size, v->rom_hash);
    // (Building and hashing the image on the thread too: a few milliseconds each on a phone.)
    g_native_pending = std::async(std::launch::async, [in = g_jit_input, s = g_native_ahead.back(), known] {
        const std::vector<uint8_t> rom = make_rom(s, false);
        const std::pair<size_t, uint64_t> id{rom.size(), rom_hash(rom)};
        if (std::find(known.begin(), known.end(), id) != known.end()) {
            JitResult r;
            r.known = true;
            return r;
        }
        return jit_translate(in, rom, kArm64);
    });
    g_native_ahead.pop_back();
}

// The ROM changed: the code for it, if there is any yet.
void native_rom_changed()
{
#ifdef COI_NATIVE_BUILT_IN
    // A build with a translation linked in (the readable C) runs it whenever it fits the ROM as
    // patched now, and otherwise (an MD+ pack's ROM) the app's own translations, as below.
    // (--jit: only the app's own.)
    if (!g_native_jit && native_rom_matches(COI_NATIVE_BUILT_IN)) {
        g_native_code = COI_NATIVE_BUILT_IN;
        g_native_rom_ok = true;
        return;
    }
#endif
    g_native_rom_ok = native_rom_matches(g_native_code);
    if (!g_native_variants_on)
        return;
    if (g_native_rom_ok) {
        native_ahead_queue();
        return;
    }
    for (const CoiNativeCode* v : g_native_variants)
        if (native_rom_matches(v)) {
            g_native_code = v;
            recomp::jit::select(v);
            g_native_rom_ok = true;
            native_ahead_queue();
            return;
        }
    if (!g_native_pending.valid())              // (one at a time; the next starts when it's done)
        g_native_pending = std::async(std::launch::async, [in = g_jit_input, rom = g_bus.rom] { return jit_translate(in, rom, kArm64); });
    if (g_native_strict) {                      // (COI_NATIVE_STRICT: at once)
        g_native_pending.wait();
        native_poll();
    }
}

// A translation made (at start-up or on the thread): kept with the others, and reported. The
// first one also goes to COI_JIT_LIST=FILE: the address of every instruction translated, one a
// line, then the functions left out and the computed jumps and calls left to the interpreter.
void native_installed(const JitResult& r)
{
    if (const char* path = std::getenv("COI_JIT_LIST"); path && g_native_variants.empty())
        if (FILE* f = std::fopen(path, "w")) {
            for (int i = 0; i < r.code->resume_count; i++)
                std::fprintf(f, "%06X\n", r.code->resume[i].address);
            for (const auto& [address, why] : r.failed)
                std::fprintf(f, "# left out: %06X %s\n", address, why.c_str());
            for (uint32_t address : r.unresolved)
                std::fprintf(f, "# computed jump or call with no known targets: %06X\n", address);
            std::fclose(f);
        }
    g_native_variants.push_back(r.code);
    std::printf("jit: %zu functions translated in %.0f ms\n", r.functions, r.ms);
}

// At each frame's start: a translation that has finished joins the others (and runs, if it's
// the current ROM's). One that failed leaves the interpreter running.
void native_poll()
{
    if (g_native_pending.valid() && g_native_pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const JitResult r = g_native_pending.get();
        if (r.code) {
            native_installed(r);
            native_rom_changed();
        } else if (r.known) {
            native_rom_changed();                // (the image running may be waiting for the thread)
        } else {
            std::fprintf(stderr, "jit: %s\n", r.error.c_str());
        }
    }
    if (g_native_variants_on)
        native_ahead_next();
}
#endif

// Switches widescreen: the ROM code (only the patched bytes differ, so the running code
// carries on), the camera limits and the frame width. Turned on during level play, the
// blocks the moved cameras need are refilled once they have moved (refill_planes_if_due).
void build_rom()
{
    g_bus.rom = make_rom(current_rom_settings(), true);
#ifdef COI_NATIVE
    g_rom_hash_known = false;
#endif
#ifdef COI_NATIVE
    // Translated code is only right for the ROM it was translated from: with any other patch
    // set (widescreen, smooth swinging, Boss Rush, cheats) the interpreter runs everything,
    // until (--jit) that image is translated too.
    native_rom_changed();
#endif
}

void set_widescreen(bool wide)
{
    if (wide && !g_wide_available)
        return;
    const bool was_wide = g_widescreen;
    g_widescreen = wide;
    build_rom();
    g_out_width = wide ? kWideWidth : kWidth;
    retune_camera_limits(wide);
    // Turned on during level play: refill the blocks the moved cameras need once they have
    // moved, see refill_planes_if_due.
    if (wide && !was_wide && (g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301]) == 0x0E) {
        g_plane_refill.pending = true;
        for (int i = 0; i < 2; i++) {
            const int a = i ? 0xFAC4 : 0xFA84;
            g_plane_refill.old_x[i] = (int16_t)(g_bus.ram[a] << 8 | g_bus.ram[a + 1]);
        }
    }
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

// Saves the state that --ram, --vram and --cram write (empty paths are skipped).
void save_state(const std::string& ram_path, const std::string& vram_path, const std::string& cram_path)
{
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
}

// --state-hash: FNV-1a over what save_state saves (RAM, VRAM, CRAM, registers, VSRAM), and the
// 68000's registers (D0-D7, A0-A7, PC, SR).
uint64_t machine_hash()
{
    uint64_t h = 0xCBF29CE484222325ull;
    auto add = [&h](const void* data, size_t size) {
        for (size_t i = 0; i < size; i++)
            h = (h ^ static_cast<const uint8_t*>(data)[i]) * 0x100000001B3ull;
    };
    add(g_bus.ram, sizeof(g_bus.ram));
    add(g_vdp.vram, sizeof(g_vdp.vram));
    add(g_vdp.cram, sizeof(g_vdp.cram));
    add(g_vdp.reg, sizeof(g_vdp.reg));
    add(g_vdp.vsram, sizeof(g_vdp.vsram));
    for (int r = M68K_REG_D0; r <= M68K_REG_SR; r++) {
#ifdef COI_NATIVE
        const uint32_t value = cpu_reg(m68k_register_t(r));
#else
        const uint32_t value = m68k_get_reg(nullptr, m68k_register_t(r));
#endif
        add(&value, 4);
    }
    return h;
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

// The fast forward button (see kFastForward): Y on a controller (North), Backspace on the
// keyboard. Neither is one of the game's buttons.
bool gamepad_fast_forward()
{
    for (SDL_Gamepad* pad : g_gamepads)
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH))
            return true;
    return false;
}

bool keyboard_fast_forward()
{
    return SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_BACKSPACE];
}

}  // namespace

// Musashi memory and interrupt callbacks.
#ifdef COI_NATIVE
// --native-test (builds with COI_NATIVE; see port/recomp): checks recompiled 68000 functions
// against the interpreter. During a headless run, each call of a function config/recomp.txt
// lists saves the CPU, RAM and VDP as the call starts (up to --native-test N per function).
// When the run ends, each saved call is replayed twice from that state: by Musashi alone, one
// instruction at a time with interrupts held off until the function returns, and by the
// recompiled C. Both runs log every memory write (VDP port writes included); the logs, the
// registers and flags, RAM and the VDP's memories must come out the same.
struct WriteRecord {
    uint32_t address, size, value;
    bool operator==(const WriteRecord& o) const { return address == o.address && size == o.size && value == o.value; }
};
std::vector<WriteRecord>* g_write_log = nullptr;

struct VdpSnapshot {
    std::vector<uint8_t> vram;
    std::vector<uint16_t> cram, vsram;
    std::vector<uint8_t> reg;
    Vdp::PortState port;
};

VdpSnapshot save_vdp()
{
    return {{g_vdp.vram, g_vdp.vram + sizeof(g_vdp.vram)}, {g_vdp.cram, g_vdp.cram + 64}, {g_vdp.vsram, g_vdp.vsram + 40},
            {g_vdp.reg, g_vdp.reg + 32}, g_vdp.port_state()};
}

void restore_vdp(const VdpSnapshot& s)
{
    std::copy(s.vram.begin(), s.vram.end(), g_vdp.vram);
    std::copy(s.cram.begin(), s.cram.end(), g_vdp.cram);
    std::copy(s.vsram.begin(), s.vsram.end(), g_vdp.vsram);
    std::copy(s.reg.begin(), s.reg.end(), g_vdp.reg);
    g_vdp.set_port_state(s.port);
}

struct NativeCall {
    uint32_t entry;
    int frame;
    std::vector<uint8_t> context, ram;
    VdpSnapshot vdp;
};
std::vector<NativeCall> g_native_calls;
std::map<uint32_t, int> g_native_counts;
int g_native_limit = 0;
bool g_native_replaying = false;
int coi_native_tracing = 0;     // (native_rt.h) translated code calls coi_native_trace: replays, checks
const CoiCpu* g_native_cpu = nullptr;  // translated code running (live or replayed): its state
int g_native_frame = 0;
extern "C" int m68ki_initial_cycles, m68ki_remaining_cycles;      // Musashi's slice counters
extern "C" unsigned int* coi_m68k_registers(void);                 // Musashi's D0-D7, A0-A7 (musashi_regs.c)
extern "C" unsigned int* coi_m68k_usp(void);                       // its user stack pointer (in supervisor mode)
// COI_NATIVE_CHECK: each instruction's address, the cycles used before it and the registers
// (d0-d7, a0-a7, CCR), both ways.
struct CheckStep {
    uint32_t pc;
    int64_t cycles;
    uint32_t regs[17];
    bool operator==(const CheckStep& o) const
    {
        return pc == o.pc && cycles == o.cycles && std::equal(regs, regs + 17, o.regs);
    }
};
std::vector<CheckStep>* g_cycle_trace = nullptr;
bool g_native_live = true;              // run translated code in the game (--no-native: the interpreter only)

// Takes the interrupt waiting, as Musashi's m68ki_exception_interrupt (68000) does, on
// translated code's registers: acknowledged (genesis_int_ack, autovectored), the vector read,
// then the PC and the SR pushed, the SR's trace bit cleared and the mask set to the level, and
// the exception's cycles counted. `pc` is the address it returns to; returns the handler's.
// The caller has checked that one is due: non-maskable (taken once), or above the mask.
extern "C" void coi_m68k_clear_nmi(void);
extern "C" unsigned int coi_m68k_exception_cycles(unsigned int vector);
extern "C" int coi_m68k_halted(void);
extern "C" int coi_m68k_take_reset_cycles(void);
extern "C" void coi_m68k_reset_state(void);
extern "C" int coi_m68k_stopped(void);
extern "C" void coi_m68k_set_stopped(int stopped);
bool g_native_acking = false;           // genesis_int_ack called by translated code's interrupt

// Without an interpreter: what translated code can't run ends the game, saying where and why.
[[noreturn]] void native_fatal(uint32_t pc, const char* why)
{
    char msg[300];
    std::snprintf(msg, sizeof(msg), "The translated 68000 code can't go on at $%06X (frame %d): %s. This build has no "
                  "interpreter to hand it to.", pc, g_native_frame, why);
    std::fflush(stdout);
    std::fprintf(stderr, "%s\n", msg);
    std::fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Castle of Illusion", msg, nullptr);
    std::exit(4);
}

unsigned int native_exception_cycles(unsigned int vector)
{
    return coi_exception_cycles_68000[vector & 0xFF];
}
int64_t g_native_interrupts = 0;

extern "C" uint32_t coi_native_interrupt(CoiCpu* c, uint32_t pc)
{
    unsigned int level = coi_native_irq_level();
    if (level == 8) {
        g_nmi_pending = false;
#ifndef COI_NO_INTERPRETER
        coi_m68k_clear_nmi();
#endif
        level = 7;
    }
    g_native_acking = true;
    int vector = genesis_int_ack(int(level));
    g_native_acking = false;
    if (vector == int(M68K_INT_ACK_AUTOVECTOR))
        vector = 24 + int(level);
    else if (vector == int(M68K_INT_ACK_SPURIOUS))
        vector = 24;
    else if (vector > 255)
        return pc;
    const uint32_t sr = (uint32_t(c->sr_high) << 8) | rt_ccr(c);
    rt_set_sr(c, sr | 0x2000);              // supervisor mode (the SSP, from user mode)
    c->sr_high = uint16_t(0x20 | level);    // trace off, supervisor, the mask
    uint32_t handler = m68k_read_memory_32(uint32_t(vector) << 2);
    if (handler == 0)
        handler = m68k_read_memory_32(15 << 2);     // the uninitialised interrupt vector
    c->a[7] -= 4;
    m68k_write_memory_32(c->a[7] & 0xFFFFFF, pc);
    c->a[7] -= 2;
    m68k_write_memory_16(c->a[7] & 0xFFFFFF, sr);
    c->cycles += native_exception_cycles(unsigned(vector));
    c->stopped = 0;                         // (Musashi: CPU_STOPPED &= ~STOP_LEVEL_STOP)
    g_native_interrupts++;
    return handler;
}

// The other exceptions, as Musashi's 68000 takes them (m68ki_exception_trap, _trapN, _illegal,
// _1010, _1111): trace off and supervisor mode (the mask kept), the PC `pc` (the next
// instruction for trap, trapv, chk and a divide by zero; the instruction's own for illegal and
// line A/F) and then the SR pushed, the PC from the vector (no check for an empty one); the
// exception's cycles in place of the instruction's (Musashi gives those back), so the whole
// instruction costs CYC_EXCEPTION[vector]. Returns the handler's address.
int64_t g_native_exceptions = 0;

extern "C" uint32_t coi_native_exception(CoiCpu* c, uint32_t vector, uint32_t pc, uint32_t opcode)
{
    const uint32_t sr = (uint32_t(c->sr_high) << 8) | rt_ccr(c);
    rt_set_sr(c, sr | 0x2000);              // supervisor mode (the SSP, from user mode)
    c->sr_high = uint16_t((c->sr_high & 0x07) | 0x20);
    c->a[7] -= 4;
    m68k_write_memory_32(c->a[7] & 0xFFFFFF, pc);
    c->a[7] -= 2;
    m68k_write_memory_16(c->a[7] & 0xFFFFFF, sr);
    const uint32_t handler = m68k_read_memory_32(vector << 2);
    c->cycles += int64_t(native_exception_cycles(vector)) - m68ki_cycles[0][opcode & 0xFFFF];
    g_native_exceptions++;
    return handler;
}

// stop #sr, as Musashi's: the SR set, the CPU stopped, and an interrupt the new mask lets in
// taken at once (which ends the stop); then, as Musashi leaves its cycle count, the slice ends:
// with at least the instruction's cycles left they're all used, else what's left modulo them
// (USE_ALL_CYCLES), the instruction's own cycles then counted on top. Returns where the CPU
// goes on: the next instruction, or the interrupt's handler. While stopped, the native loop
// spends each slice waiting, as m68k_execute does.
extern "C" uint32_t coi_native_stop(CoiCpu* c, uint32_t sr, uint32_t next)
{
    rt_set_sr(c, sr);
    c->stopped = 1;
    uint32_t pc = next;
    if (((sr >> 8) & 7) < coi_native_irq_level())
        pc = coi_native_interrupt(c, next);
    if (c->cycle_limit) {
        const int64_t own = m68ki_cycles[0][0x4E72];
        int64_t left = c->cycle_limit - (c->cycles - own);
        left = left >= own ? own : left % own;
        c->cycles = c->cycle_limit - (left - own);
    }
    return pc;
}

// COI_NATIVE_STRICT=1 with --native: the interpreter may start a game instruction only where
// translated code hands over on purpose; anywhere else the run stops, saying where. Each
// translation is made at once instead of on a thread, so the boot and every image met run
// translated from their first instruction. The one handover allowed, counted for the report:
// a computed jsr/jmp whose target isn't one the translation knows.
// Interrupts are translated code's too (coi_native_interrupt, cpu_execute): Musashi taking one
// (genesis_int_ack called from Musashi) also stops the run.
std::map<std::pair<std::string, uint32_t>, int64_t> g_strict_allowed;

// A RAM stub's `jmp (xxx).l` (the stub table InstallRamStubs copies to $FFFF60 at boot), at
// its 24-bit address or the full one Musashi holds when it's called as `$FFxx.w`.
bool native_stub_jmp(uint32_t pc)
{
    pc &= 0xFFFFFF;
    return pc >= 0xFFFF60 && pc < 0xFFFF60 + 0x15 * 6 && (pc - 0xFFFF60) % 6 == 0
        && (g_bus.ram[pc & 0xFFFF] << 8 | g_bus.ram[(pc + 1) & 0xFFFF]) == 0x4EF9;
}

// The interpreter is about to run the instruction at `pc`, which translated code didn't.
void strict_check(uint32_t pc, const char* why, const CoiCpu* cpu)
{
    if (!g_native_strict)
        return;
    auto allow = [&](const char* reason) { g_strict_allowed[{reason, pc & 0xFFFFFF}]++; };
    const uint32_t a = pc & 0xFFFFFF;
    const uint16_t op = a + 1 < g_bus.rom.size() ? uint16_t(g_bus.rom[a] << 8 | g_bus.rom[a + 1]) : 0;
    if (!why[0]) {
        // Translated code stopped before its first instruction: why?
        const uint32_t sr = (uint32_t(cpu->sr_high) << 8) | rt_ccr(cpu);
        const int ea = op & 0x3F;
        int new_sr = -1;
        const uint16_t ext = a + 3 < g_bus.rom.size() ? uint16_t(g_bus.rom[a + 2] << 8 | g_bus.rom[a + 3]) : 0;
        if ((op & 0xFFC0) == 0x46C0 && ea == 0x3C) new_sr = ext;
        else if ((op & 0xFFF8) == 0x46C0) new_sr = cpu->d[op & 7] & 0xFFFF;
        else if (op == 0x027C) new_sr = sr & ext;
        else if (op == 0x007C) new_sr = sr | ext;
        else if (op == 0x0A7C) new_sr = sr ^ ext;
        else if (op == 0x4E73) new_sr = m68k_read_memory_16(cpu->a[7]);
        const int mode = ea >> 3;
        if (((op & 0xFFC0) == 0x4E80 || (op & 0xFFC0) == 0x4EC0)
            && (mode == 2 || mode == 5 || mode == 6 || ea == 0x3B))
            return allow("computed jsr/jmp to a target not translated");
        // The exceptions other than interrupts are Musashi's (none happens in play): named.
        if (new_sr >= 0) why = "SR write translated code doesn't do (leaving supervisor mode, or tracing)";
        else if ((op & 0xFFF0) == 0x4E40) why = "trap (an exception translated code didn't take)";
        else if (op == 0x4E76) why = "trapv (an exception translated code didn't take)";
        else if (op == 0x4AFC || (op & 0xF000) == 0xA000 || (op & 0xF000) == 0xF000)
            why = "illegal instruction, line A or line F (an exception translated code didn't take)";
        else if ((op & 0xF0C0) == 0x80C0) why = "divu/divs: a divide by zero, or one from (An)+ or -(An) (an exception translated code didn't take)";
        else if ((op & 0xF1C0) == 0x4180) why = "chk (an exception translated code didn't take)";
        else if (op == 0x4E72) why = "stop translated code didn't do (leaving supervisor mode, or tracing)";
        else if (op == 0x4E70) why = "reset";
        else if (op == 0x4E77) why = "rtr";
        else why = "instruction translated code stopped at";
    }
    if (kNoInterpreter)
        native_fatal(pc, why);
    std::fflush(stdout);
    std::fprintf(stderr, "native strict: frame %d: the interpreter ran $%06X (opcode $%04X): %s\n",
                 g_native_frame, pc, op, why);
    std::fflush(stderr);
    std::_Exit(3);
}

void strict_report()
{
    if (!g_on_demand.empty()) {
        double ms = 0;
        for (const auto& [a, t] : g_on_demand)
            ms += t;
        std::printf("jit: %zu translated on demand, in %.1f ms:", g_on_demand.size(), ms);
        int shown = 0;
        for (const auto& [a, t] : g_on_demand)
            if (shown++ < 12)
                std::printf(" $%06X", a);
        std::printf("%s\n", g_on_demand.size() > 12 ? " ..." : "");
    }
    if (!g_native_strict)
        return;
    std::printf("native strict: the interpreter ran only where allowed (interrupts taken by translated code: %lld, "
                "other exceptions: %lld):\n", (long long)g_native_interrupts, (long long)g_native_exceptions);
    for (const auto& [k, n] : g_strict_allowed)
        std::printf("  %lld times at $%06X: %s\n", (long long)n, k.second, k.first.c_str());
}

// One scanline's slice of 68000 time, as m68k_execute gives it. With translated code running
// (--native) the slice is run here, as Musashi's m68k_execute would run it: an interrupt due at
// its start (one raised between slices) taken first, by the code translated code takes one
// with, its cycles out of the slice; then translated code, entered again wherever it stops,
// until the slice's cycles are used (at least one instruction, as Musashi's do-while). Musashi
// runs only where translated code can't go on (a computed jump to a target it doesn't know,
// code not translated, the ROM image not translated yet): it is given the rest of the slice,
// and its instruction hook brings translated code back as soon as it can.
extern "C" int coi_native_enter(unsigned int pc, int cycles_left);
int native_run(CoiCpu& cpu, uint32_t pc, int cycles_left, uint32_t& next);

namespace {
// The CPU between slices, while the native loop runs: translated code's registers, and the PC.
// Musashi's copy is brought up to date only when it has to run (native_sync) or something reads
// it; while `ours` is set, it's stale.
struct NativeState {
    CoiCpu cpu{};
    uint32_t pc = 0;
    bool ours = kNoInterpreter;     // (without Musashi, always: it starts as m68k_init leaves it)
};
NativeState g_ns;
int g_native_check_from = std::getenv("COI_NATIVE_CHECK") ? std::atoi(std::getenv("COI_NATIVE_CHECK")) : -1;

// Musashi's registers into ours (the native loop takes over).
void native_take()
{
#ifndef COI_NO_INTERPRETER
    if (g_ns.ours)
        return;
    const unsigned int* dar = coi_m68k_registers();
    std::memcpy(g_ns.cpu.d, dar, sizeof(g_ns.cpu.d));
    std::memcpy(g_ns.cpu.a, dar + 8, sizeof(g_ns.cpu.a));
    g_ns.cpu.usp = *coi_m68k_usp();
    const uint32_t sr = m68k_get_reg(nullptr, M68K_REG_SR);
    rt_set_ccr(&g_ns.cpu, sr & 0x1F);
    g_ns.cpu.sr_high = uint16_t(sr >> 8);
    g_ns.pc = m68k_get_reg(nullptr, M68K_REG_PC);
    g_ns.cpu.stopped = uint8_t(coi_m68k_stopped());
    g_ns.ours = true;
#endif
}

// Ours into Musashi's, which is then the one that counts (until native_take).
void native_sync()
{
#ifndef COI_NO_INTERPRETER
    if (!g_ns.ours)
        return;
    // The SR first: from user mode (Musashi never reset, after m68k_init) it swaps Musashi's
    // stack pointers, before ours are written.
    m68k_set_reg(M68K_REG_SR, (uint32_t(g_ns.cpu.sr_high) << 8) | rt_ccr(&g_ns.cpu));
    unsigned int* dar = coi_m68k_registers();
    std::memcpy(dar, g_ns.cpu.d, sizeof(g_ns.cpu.d));
    std::memcpy(dar + 8, g_ns.cpu.a, sizeof(g_ns.cpu.a));
    *coi_m68k_usp() = g_ns.cpu.usp;
    m68k_set_reg(M68K_REG_PC, g_ns.pc);
    m68k_set_reg(M68K_REG_PPC, g_ns.pc);            // as m68k_execute leaves it
    coi_m68k_set_stopped(g_ns.cpu.stopped);
    g_ns.ours = false;
#endif
}

// A reset (start-up, the menu's): Musashi's registers are the ones that count.
void native_reset()
{
    g_ns.ours = kNoInterpreter;
}

int g_reset_cycles = 0;     // a reset's cycles, eaten at the next slice's start as m68k_execute does

// A 68000 reset, as Musashi's m68k_pulse_reset does it: the SSP from vector 0 and the PC from
// vector 1; trace off, supervisor mode, the interrupt mask at 7, the condition codes, D0-A6 and
// the USP kept (from user mode, the A7 in use becomes the USP); the reset's cycles (Musashi's
// exception table) taken from the next slice. With translated code running (--native) the port
// does it on its own registers; otherwise Musashi does.
void cpu_reset()
{
#ifndef COI_NO_INTERPRETER
    if (!g_native_live) {
        m68k_pulse_reset();
        native_reset();
        return;
    }
#endif
    native_take();
    CoiCpu& cpu = g_ns.cpu;
    if (!(cpu.sr_high & 0x20))
        cpu.usp = cpu.a[7];
    cpu.sr_high = 0x27;
    cpu.a[7] = m68k_read_memory_32(0);
    g_ns.pc = m68k_read_memory_32(4);
    g_reset_cycles = int(native_exception_cycles(0));
#ifndef COI_NO_INTERPRETER
    coi_m68k_reset_state();
#endif
}

// A register as it stands between slices (D0-D7, A0-A7, PC, SR), wherever it's kept.
uint32_t cpu_reg(m68k_register_t r)
{
#ifndef COI_NO_INTERPRETER
    if (!g_ns.ours)
        return m68k_get_reg(nullptr, r);
#endif
    if (r >= M68K_REG_D0 && r <= M68K_REG_D7) return g_ns.cpu.d[r - M68K_REG_D0];
    if (r >= M68K_REG_A0 && r <= M68K_REG_A7) return g_ns.cpu.a[r - M68K_REG_A0];
    if (r == M68K_REG_PC || r == M68K_REG_PPC) return g_ns.pc;
    if (r == M68K_REG_SR) return (uint32_t(g_ns.cpu.sr_high) << 8) | rt_ccr(&g_ns.cpu);
    if (r == M68K_REG_USP) return g_ns.cpu.sr_high & 0x20 ? g_ns.cpu.usp : g_ns.cpu.a[7];
#ifndef COI_NO_INTERPRETER
    native_sync();
    return m68k_get_reg(nullptr, r);
#else
    return 0;
#endif
}
}  // namespace

uint32_t g_native_skip = ~0u;               // coi_native_enter hands this PC to Musashi at once
int64_t g_native_fallbacks = 0;             // slices finished by m68k_execute in the native loop
int g_slice_used = 0;                       // (traces: the slice's cycles used before this native_run)
namespace {
int native_interrupt();

int cpu_execute(int cycles)
{
    if (const int rc = g_reset_cycles) {            // as m68k_execute: not counted as used
        g_reset_cycles = 0;
        cycles -= rc;
        if (cycles <= 0)
            return rc;
    }
#ifdef COI_NO_INTERPRETER
    if (!g_native_rom_ok)
        native_fatal(g_ns.pc, "the ROM image running isn't translated");
#else
    if (!g_native_live || !g_native_rom_ok || coi_m68k_halted() || g_native_check_from >= 0) {
        native_sync();
        if (g_native_live && g_native_check_from < 0) {
            g_native_fallbacks++;
            strict_check(m68k_get_reg(nullptr, M68K_REG_PC), g_native_rom_ok ? "Musashi stopped" : "the ROM image running isn't the one translated", nullptr);
        }
        return m68k_execute(cycles);
    }
#endif
    native_take();
    int used = native_interrupt();
    if (g_ns.cpu.stopped)                   // waiting at a stop: the slice goes by (m68k_execute)
        return cycles;
    do {
        uint32_t next = 0;
        // (An interrupt at the slice's start can use more than the slice: Musashi's do-while
        // still runs one instruction, which a budget of 1 gives.)
        g_slice_used = used;
        const int r = native_run(g_ns.cpu, g_ns.pc, cycles - used > 0 ? cycles - used : 1, next);
        if (r < 0) {
#ifdef COI_NO_INTERPRETER
            native_fatal(g_ns.pc, "code translated code can't run (or a computed jump to an address it can't translate)");
#else
            g_native_fallbacks++;
            g_native_skip = g_ns.pc;        // (counted and checked already)
            native_sync();
            return used + m68k_execute(cycles - used);
#endif
        }
        g_ns.pc = next;
        used += r;
    } while (used < cycles);
    return used;
}

// An interrupt due at a slice's start, taken as Musashi's m68k_execute would take it: the
// cycles it used.
int native_interrupt()
{
    CoiCpu& cpu = g_ns.cpu;
    if (coi_native_irq_level() <= (cpu.sr_high & 7u))
        return 0;
    cpu.cycles = 0;
    g_ns.pc = coi_native_interrupt(&cpu, g_ns.pc);
    const int used = int(cpu.cycles);
    g_native_cycles += used;
    return used;
}
}  // namespace

// --native: Musashi's instruction hook (musashi_conf.h) calls this before each instruction.
// Where translated code covers `pc`, it runs it, chaining from function to function, until
// the cycles left in this slice are used up or it reaches code not translated, and returns the
// cycles it used (the interpreter then carries on from where it stopped); otherwise -1.
// Translating on demand: code translated code reaches without having it (a computed jump to a
// target no list names, a handler returning into code not in the lists, the lists left out) is
// translated on the spot, from that address, with the current translation's settings and frame,
// and joins it (jit::merge), so it's there from then on. Only a JIT translation grows so.

bool native_on_demand(uint32_t pc)
{
    uint32_t frame = 0;
    bool trace = false;
    if ((pc & 1) || pc >= g_bus.rom.size() || g_on_demand_failed.count(pc)
        || !recomp::jit::shape(g_native_code, &frame, &trace))
        return false;
    JitInput in;
    in.config = g_jit_input.config;
    in.config.functions.clear();
    in.list = {pc};
    in.trace = trace;
    const JitResult r = jit_translate(in, g_bus.rom, kArm64, frame);
    if (!r.code || r.failed.count(pc)) {
        g_on_demand_failed.insert(pc);
        if (!r.code)
            std::fprintf(stderr, "jit: translating $%06X on demand: %s\n", pc, r.error.c_str());
        return false;
    }
    recomp::jit::merge(g_native_code, r.code);
    g_on_demand[pc] = r.ms;
    if (std::getenv("COI_ON_DEMAND_LOG"))
        std::printf("jit: $%06X translated on demand (%zu functions)\n", pc, r.functions);
    return true;
}

// Runs translated code on `cpu` from `pc` (as the hook or the native loop has it), chaining
// until the budget is spent or it reaches code it can't run: the cycles used, with `next` where
// it stopped, or -1 if it can't start here (counted, and checked in strict mode).
int native_run(CoiCpu& cpu, uint32_t pc, int cycles_left, uint32_t& next)
{
    static std::vector<CoiNativeFn> at;              // per ROM word: a function that runs from here
    static const CoiNativeCode* at_for = nullptr;
    static int at_count = 0;                         // (resumes it holds: more after on-demand code)
    if (!g_native_rom_ok) {
        strict_check(pc, "the ROM image running isn't the one translated", &cpu);
        return -1;
    }
    auto tables = [&] {
        if (at_for == g_native_code && at_count == g_native_code->resume_count)
            return;
        if (at_for != g_native_code)
            at.assign(g_bus.rom.size() / 2 + 1, nullptr);
        for (int i = at_for == g_native_code ? at_count : 0; i < g_native_code->resume_count; i++)
            if (g_native_code->resume[i].address / 2 < at.size())
                at[g_native_code->resume[i].address / 2] = g_native_code->resume[i].fn;
        at_for = g_native_code;
        at_count = g_native_code->resume_count;
    };
    tables();
    auto lookup = [](uint32_t a) { return a / 2 < at.size() && !(a & 1) ? at[a / 2] : nullptr; };   // (odd: never code)
    // A RAM stub's jmp, reached when a slice's budget ran out just before it (translated code
    // calling through a stub with budget left does the jmp itself): done here, as translated
    // code would, its cycles and then the routine, at the address the stub holds.
    uint32_t stub_target = 0;
    if (native_stub_jmp(pc)) {
        const uint8_t* s = g_bus.ram + ((pc + 2) & 0xFFFF);
        stub_target = uint32_t(s[0]) << 24 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 8 | s[3];
    }
    const uint32_t start = stub_target ? stub_target : pc;
    if (start > 0xFFFFFF) {                         // a PC with its top byte set: Musashi keeps it
        strict_check(pc, "a PC with its top byte set", &cpu);
        return -1;
    }
    CoiNativeFn fn = lookup(start);
    if (!fn && native_on_demand(start)) {
        tables();
        fn = lookup(start);
    }
    if (!fn) {
        g_interpreted[pc]++;
        strict_check(pc, "not translated", &cpu);
        return -1;
    }
    if (cpu.stopped)
        return -1;
    cpu.cycles = 0;
    cpu.cycle_limit = cycles_left;
    cpu.fail = cpu.missing = 0;
    cpu.jump = 0;
    g_native_cpu = &cpu;
    next = pc;
    if (stub_target) {
        RT_TRACE(&cpu, pc);
        RT_CYCLES(&cpu, 0x4EF9);                    // jmp (xxx).l
        next = stub_target;
    }
    if (!stub_target || cpu.cycles < cycles_left) for (;;) {
        const uint32_t r = g_native_code->run(&cpu, fn, next);
        if (cpu.fail) {
            next = cpu.fail;
            break;
        }
        next = r;                                   // all 32 bits, as the PC holds them
        if (cpu.cycles >= cycles_left || !(fn = lookup(next)))
            break;
    }
    g_native_cpu = nullptr;
    if (next == pc && cpu.cycles == 0) {
        g_interpreted[pc]++;
        strict_check(pc, cpu.missing ? "a call to code not translated" : "", &cpu);
        return -1;
    }
    g_native_cycles += cpu.cycles;
    return int(cpu.cycles);
}

#ifndef COI_NO_INTERPRETER
// --native, Musashi running: its instruction hook (musashi_conf.h) calls this before each
// instruction; translated code runs from there on Musashi's registers (native_run), and the
// cycles it used are handed back (the interpreter then carries on from where it stopped), or -1.
extern "C" int coi_native_enter(unsigned int pc, int cycles_left)
{
    if (!g_native_live)
        return -1;
    if (pc == g_native_skip) {                      // the native loop gave Musashi this instruction
        g_native_skip = ~0u;
        return -1;
    }
    static unsigned int* const dar = coi_m68k_registers();
    const uint32_t sr = m68k_get_reg(nullptr, M68K_REG_SR);
    CoiCpu cpu{};
    std::memcpy(cpu.d, dar, sizeof(cpu.d));
    std::memcpy(cpu.a, dar + 8, sizeof(cpu.a));
    cpu.usp = *coi_m68k_usp();
    rt_set_ccr(&cpu, sr & 0x1F);
    cpu.sr_high = uint16_t(sr >> 8);

    // COI_NATIVE_CHECK=F: from frame F on, each run here is checked against the interpreter
    // running the same cycles from the same state; the first difference is reported.
    const bool check = g_native_check_from >= 0 && g_native_frame >= g_native_check_from;
    std::vector<uint8_t> context0;
    std::vector<uint8_t> ram0;
    VdpSnapshot vdp0;
    // The bus state the 68000 can change besides RAM: the controller ports (their select line
    // decides which half of the pad a read returns) and the Z80's RAM and bus.
    Bus::IoState io0{};
    std::vector<uint8_t> z80_0;
    if (check) {
        context0.resize(m68k_context_size());
        m68k_get_context(context0.data());
        ram0.assign(g_bus.ram, g_bus.ram + 0x10000);
        vdp0 = save_vdp();
        io0 = g_bus.io_state();
        z80_0.assign(g_bus.z80_ram, g_bus.z80_ram + sizeof(g_bus.z80_ram));
    }

    std::vector<CheckStep> native_steps;
    const int tracing0 = coi_native_tracing;        // (a probe may be tracing already)
    if (check) {
        g_cycle_trace = &native_steps;
        coi_native_tracing = 1;
    }
    uint32_t next = pc;
    const int ran = native_run(cpu, pc, cycles_left, next);
    g_cycle_trace = nullptr;
    coi_native_tracing = tracing0;
    if (ran < 0)
        return -1;
    if (cpu.stopped)                                // translated code ran a stop: Musashi waits
        coi_m68k_set_stopped(1);
    m68k_set_reg(M68K_REG_SR, (uint32_t(cpu.sr_high) << 8) | rt_ccr(&cpu));     // (first: it may swap the SPs)
    std::memcpy(dar, cpu.d, sizeof(cpu.d));
    std::memcpy(dar + 8, cpu.a, sizeof(cpu.a));
    *coi_m68k_usp() = cpu.usp;
    m68k_set_reg(M68K_REG_PC, next);
    if (check) {
        std::vector<uint8_t> context1(m68k_context_size());
        m68k_get_context(context1.data());
        const std::vector<uint8_t> ram1(g_bus.ram, g_bus.ram + 0x10000);
        const VdpSnapshot vdp1 = save_vdp();
        const int initial = m68ki_initial_cycles, remaining = m68ki_remaining_cycles;
        const std::vector<uint8_t> z80_1(g_bus.z80_ram, g_bus.z80_ram + sizeof(g_bus.z80_ram));
        const Bus::IoState io1 = g_bus.io_state();
        m68k_set_context(context0.data());
        std::copy(ram0.begin(), ram0.end(), g_bus.ram);
        restore_vdp(vdp0);
        g_bus.set_io_state(io0);
        std::copy(z80_0.begin(), z80_0.end(), g_bus.z80_ram);
        g_native_live = false;
        int64_t used = 0;
        std::vector<CheckStep> steps;
        while (used < cpu.cycles) {
            CheckStep st{m68k_get_reg(nullptr, M68K_REG_PC) & 0xFFFFFF, used, {}};
            for (int i = 0; i < 16; i++)
                st.regs[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i));
            st.regs[16] = m68k_get_reg(nullptr, M68K_REG_SR) & 0x1F;
            steps.push_back(st);
            used += m68k_execute(1);
        }
        g_native_live = true;
        char why[160] = "";
        const uint32_t ipc = m68k_get_reg(nullptr, M68K_REG_PC);   // (all 32 bits, as translated code keeps them)
        if (ipc != next || used != cpu.cycles)
            std::snprintf(why, sizeof(why), "stopped at $%06X after %lld cycles, interpreter $%06X after %lld",
                          next, (long long)cpu.cycles, ipc, (long long)used);
        for (int i = 0; i < 16 && !why[0]; i++) {
            const uint32_t v = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i));
            const uint32_t n = i < 8 ? cpu.d[i] : cpu.a[i - 8];
            if (v != n)
                std::snprintf(why, sizeof(why), "%c%d = %08X, interpreter %08X", i < 8 ? 'd' : 'a', i & 7, n, v);
        }
        const uint32_t isr = m68k_get_reg(nullptr, M68K_REG_SR) & 0xA71F;
        if (!why[0] && isr != ((uint32_t(cpu.sr_high) << 8 | rt_ccr(&cpu)) & 0xA71F))
            std::snprintf(why, sizeof(why), "SR = %04X, interpreter %04X", (uint32_t(cpu.sr_high) << 8) | rt_ccr(&cpu), isr);
        for (int a = 0; a < 0x10000 && !why[0]; a++)
            if (ram1[a] != g_bus.ram[a])
                std::snprintf(why, sizeof(why), "RAM $FF%04X = %02X, interpreter %02X", a, ram1[a], g_bus.ram[a]);
        const VdpSnapshot vi = save_vdp();
        if (!why[0] && (vi.vram != vdp1.vram || vi.cram != vdp1.cram || vi.vsram != vdp1.vsram || vi.reg != vdp1.reg))
            std::snprintf(why, sizeof(why), "VDP differs");
        if (!why[0] && !(g_bus.io_state() == io1))
            std::snprintf(why, sizeof(why), "controller ports or Z80 bus differ");
        if (!why[0] && !std::equal(z80_1.begin(), z80_1.end(), g_bus.z80_ram))
            std::snprintf(why, sizeof(why), "Z80 RAM differs");
        if (why[0]) {
            std::printf("native check, frame %d: from $%06X: %s\n", g_native_frame, pc, why);
            size_t k = 0;
            while (k < steps.size() && k < native_steps.size() && steps[k] == native_steps[k])
                k++;
            std::printf("  %zu instructions agree (of %zu/%zu)\n", k, native_steps.size(), steps.size());
            for (size_t j = k > 1 ? k - 1 : 0; j < k + 1; j++)
                for (const auto* tr : {&native_steps, &steps})
                    if (j < tr->size()) {
                        const CheckStep& st = (*tr)[j];
                        std::printf("  #%zu %s $%06X @%lld ccr %02X d:", j, tr == &steps ? "interpreter" : "native     ",
                                    st.pc, (long long)st.cycles, st.regs[16]);
                        for (int r = 0; r < 16; r++)
                            std::printf(r == 8 ? " a: %08X" : " %08X", st.regs[r]);
                        std::printf("\n");
                    }
            std::fflush(stdout);
            std::exit(2);
        }
        m68k_set_context(context1.data());
        std::copy(ram1.begin(), ram1.end(), g_bus.ram);
        restore_vdp(vdp1);
        m68ki_initial_cycles = initial;
        m68ki_remaining_cycles = remaining;
    }
    return int(cpu.cycles);
}
#endif  // COI_NO_INTERPRETER

// The instruction traces of the replay being compared: the address and registers (d0-d7,
// a0-a7, CCR) as each instruction starts.
struct TraceStep {
    uint32_t pc, regs[17];
    int64_t cycles = 0;     // (probes: the slice's cycles used before it; not compared here)
    bool operator==(const TraceStep& o) const { return pc == o.pc && std::equal(regs, regs + 17, o.regs); }
};
std::vector<TraceStep>* g_trace = nullptr;
std::vector<uint8_t>* g_probe_seen = nullptr;  // (COI_PROBE_SEEN: each instruction the interpreter ran)
int64_t g_probe_slice_used = 0;     // (probes: cycles used by the slices before this one)

extern "C" void coi_native_trace(const CoiCpu* c, uint32_t address)
{
    if (g_cycle_trace) {
        CheckStep st{address, c->cycles, {}};
        for (int i = 0; i < 8; i++) {
            st.regs[i] = c->d[i];
            st.regs[8 + i] = c->a[i];
        }
        st.regs[16] = rt_ccr(c);
        g_cycle_trace->push_back(st);
    }
    if (!g_trace)
        return;
    TraceStep s{address, {}};
    for (int i = 0; i < 8; i++) {
        s.regs[i] = c->d[i];
        s.regs[8 + i] = c->a[i];
    }
    s.regs[16] = rt_ccr(c);
    s.cycles = g_probe_slice_used + g_slice_used + c->cycles;
    g_trace->push_back(s);
}

#ifndef COI_NO_INTERPRETER
void native_hook(unsigned int pc)
{
    pc &= 0xFFFFFF;
    if (g_probe_seen && pc < g_probe_seen->size())
        (*g_probe_seen)[pc] = 1;
    if (g_native_replaying) {
        if (g_trace) {
            TraceStep s{pc, {}};
            for (int i = 0; i < 16; i++)
                s.regs[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i));
            s.regs[16] = m68k_get_reg(nullptr, M68K_REG_SR) & 0x1F;
            s.cycles = g_probe_slice_used + m68k_cycles_run();
            g_trace->push_back(s);
        }
        return;
    }
    static std::vector<uint8_t> is_entry;           // per ROM word: a listed function starts here
    if (is_entry.empty()) {
        is_entry.assign(0x40000, 0);
        for (int i = 0; i < g_native_code->entry_count; i++)
            is_entry[(g_native_code->entries[i] >> 1) & 0x3FFFF] = 1;
    }
    if (pc >= 0x80000 || !is_entry[pc >> 1])
        return;
    if (g_native_counts[pc]++ >= g_native_limit)
        return;
    NativeCall call{pc, g_native_frame, std::vector<uint8_t>(m68k_context_size()), {g_bus.ram, g_bus.ram + 0x10000}, save_vdp()};
    m68k_get_context(call.context.data());
    g_native_calls.push_back(std::move(call));
}

struct NativeResult {
    std::vector<WriteRecord> writes;
    uint32_t regs[16], ccr, returned;
    int64_t cycles = 0;
    std::vector<uint8_t> ram;
    VdpSnapshot vdp;
    std::string error;
};

// Replays every saved call both ways; returns the number that differed.
int native_report()
{
    g_native_replaying = true;
    coi_native_tracing = 1;
    int failed = 0, waited = 0, stopped = 0;
    // COI_NATIVE_PAUSE: give each call a budget that runs out partway, as an interrupt would.
    const bool pause = std::getenv("COI_NATIVE_PAUSE") != nullptr;
    std::map<uint32_t, std::pair<int, int>> tally;      // entry -> (calls, matching)
    std::map<uint32_t, int> stops;                      // untranslated code it stopped at -> times
    std::map<uint32_t, std::string> differs;            // entry -> its first mismatch
    for (const NativeCall& call : g_native_calls) {
        auto restore = [&] {
            m68k_set_context(const_cast<uint8_t*>(call.context.data()));
            std::copy(call.ram.begin(), call.ram.end(), g_bus.ram);
            restore_vdp(call.vdp);
        };
        auto capture = [&](NativeResult& r) {
            r.ram.assign(g_bus.ram, g_bus.ram + 0x10000);
            r.vdp = save_vdp();
        };
        NativeResult ref, nat;
        std::vector<TraceStep> ref_trace, nat_trace;

        // The interpreter.
        restore();
        const uint32_t sp0 = m68k_get_reg(nullptr, M68K_REG_A7);
        const uint32_t ret = m68k_read_memory_32(sp0) & 0xFFFFFF;
        const uint32_t sr = m68k_get_reg(nullptr, M68K_REG_SR);
        m68k_set_reg(M68K_REG_SR, sr | 0x0700);
        g_write_log = &ref.writes;
        g_trace = &ref_trace;
        long steps = 0;
        while (steps++ < 2'000'000) {
            ref.cycles += m68k_execute(1);
            if ((m68k_get_reg(nullptr, M68K_REG_PC) & 0xFFFFFF) == ret && m68k_get_reg(nullptr, M68K_REG_A7) == sp0 + 4)
                break;
        }
        g_write_log = nullptr;
        g_trace = nullptr;
        for (int i = 0; i < 16; i++)
            ref.regs[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i));
        ref.ccr = m68k_get_reg(nullptr, M68K_REG_SR) & 0xA71F;
        ref.returned = m68k_get_reg(nullptr, M68K_REG_PC) & 0xFFFFFF;
        capture(ref);
        if (ref.returned != ret) {          // didn't return (waits for an interrupt, or wasn't called)
            waited++;
            continue;
        }

        // The recompiled code.
        restore();
        CoiCpu cpu{};
        for (int i = 0; i < 8; i++) {
            cpu.d[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i));
            cpu.a[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_A0 + i));
        }
        rt_set_ccr(&cpu, sr & 0x1F);
        cpu.sr_high = uint16_t((sr | 0x0700) >> 8);    // interrupts held off, as for the interpreter
        cpu.usp = *coi_m68k_usp();
        cpu.cycle_limit = pause ? 1 + int64_t((uint64_t(&call - g_native_calls.data()) * 7919) % uint64_t(ref.cycles))
                                : ref.cycles + 64;
        if (std::getenv("COI_NATIVE_VERBOSE"))
            std::fprintf(stderr, "replaying $%06X (frame %d)\n", call.entry, call.frame), std::fflush(stderr);
        g_write_log = &nat.writes;
        g_trace = &nat_trace;
        g_native_cpu = &cpu;
        nat.returned = g_native_code->run(&cpu, g_native_code->lookup(call.entry), 0);
        g_native_cpu = nullptr;
        nat.cycles = cpu.cycles;
        if (cpu.fail) {
            // Stopped partway: the interpreter carries on from there, as in the live game.
            stopped++;
            if (cpu.missing)
                stops[cpu.missing]++;
            m68k_set_reg(M68K_REG_SR, (uint32_t(cpu.sr_high) << 8) | rt_ccr(&cpu));
            for (int i = 0; i < 8; i++) {
                m68k_set_reg(m68k_register_t(M68K_REG_D0 + i), cpu.d[i]);
                m68k_set_reg(m68k_register_t(M68K_REG_A0 + i), cpu.a[i]);
            }
            m68k_set_reg(M68K_REG_PC, cpu.fail);
            *coi_m68k_usp() = cpu.usp;
            for (long n = 0; n < 2'000'000; n++) {
                if ((m68k_get_reg(nullptr, M68K_REG_PC) & 0xFFFFFF) == ret && m68k_get_reg(nullptr, M68K_REG_A7) == sp0 + 4)
                    break;
                nat.cycles += m68k_execute(1);
            }
            nat.returned = m68k_get_reg(nullptr, M68K_REG_PC) & 0xFFFFFF;
            for (int i = 0; i < 8; i++) {
                cpu.d[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i));
                cpu.a[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_A0 + i));
            }
            const uint32_t s = m68k_get_reg(nullptr, M68K_REG_SR);
            rt_set_ccr(&cpu, s & 0x1F);
            cpu.sr_high = uint16_t(s >> 8);
        }
        g_write_log = nullptr;
        g_trace = nullptr;
        for (int i = 0; i < 8; i++) {
            nat.regs[i] = cpu.d[i];
            nat.regs[8 + i] = cpu.a[i];
        }
        nat.ccr = (uint32_t(cpu.sr_high) << 8) | rt_ccr(&cpu);
        capture(nat);

        // Compare.
        char buf[200] = "";
        if (nat.returned != ref.returned)
            std::snprintf(buf, sizeof(buf), "returned to $%06X, interpreter $%06X", nat.returned, ref.returned);
        else if (nat.writes.size() != ref.writes.size() || !std::equal(nat.writes.begin(), nat.writes.end(), ref.writes.begin())) {
            size_t i = 0;
            while (i < nat.writes.size() && i < ref.writes.size() && nat.writes[i] == ref.writes[i])
                i++;
            std::snprintf(buf, sizeof(buf), "writes differ at #%zu of %zu/%zu", i, nat.writes.size(), ref.writes.size());
            if (failed < 1)
                for (size_t k = i; k < i + 6; k++)
                    std::printf("  #%zu native %06X/%u=%08X  interpreter %06X/%u=%08X\n", k,
                                k < nat.writes.size() ? nat.writes[k].address : 0, k < nat.writes.size() ? nat.writes[k].size : 0,
                                k < nat.writes.size() ? nat.writes[k].value : 0, k < ref.writes.size() ? ref.writes[k].address : 0,
                                k < ref.writes.size() ? ref.writes[k].size : 0, k < ref.writes.size() ? ref.writes[k].value : 0);
        } else {
            for (int i = 0; i < 16 && !buf[0]; i++)
                if (nat.regs[i] != ref.regs[i])
                    std::snprintf(buf, sizeof(buf), "%c%d = %08X, interpreter %08X", i < 8 ? 'd' : 'a', i & 7, nat.regs[i], ref.regs[i]);
            if (!buf[0] && nat.cycles != ref.cycles)
                std::snprintf(buf, sizeof(buf), "%lld cycles, interpreter %lld", (long long)nat.cycles, (long long)ref.cycles);
            if (!buf[0] && nat.ccr != ref.ccr)
                std::snprintf(buf, sizeof(buf), "SR = %04X, interpreter %04X", nat.ccr, ref.ccr);
            if (!buf[0] && nat.ram != ref.ram)
                std::snprintf(buf, sizeof(buf), "RAM differs");
            if (!buf[0] && (nat.vdp.vram != ref.vdp.vram || nat.vdp.cram != ref.vdp.cram || nat.vdp.vsram != ref.vdp.vsram))
                std::snprintf(buf, sizeof(buf), "VDP memory differs");
        }
        auto& t = tally[call.entry];
        t.first++;
        if (buf[0] && failed == 0) {        // the first failure: where the two runs part
            size_t i = 0;
            while (i < nat_trace.size() && i < ref_trace.size() && nat_trace[i] == ref_trace[i])
                i++;
            std::printf("  traces agree for %zu instructions (of %zu/%zu)\n", i, nat_trace.size(), ref_trace.size());
            for (size_t k = i > 0 ? i - 1 : 0; k <= i; k++)
                for (const auto* tr : {&nat_trace, &ref_trace})
                    if (k < tr->size()) {
                        const TraceStep& s = (*tr)[k];
                        std::printf("  %s #%zu $%06X ccr %02X d:", tr == &nat_trace ? "native     " : "interpreter", k, s.pc, s.regs[16]);
                        for (int r = 0; r < 16; r++)
                            std::printf(r == 8 ? " a: %08X" : " %08X", s.regs[r]);
                        std::printf("\n");
                    }
        }
        if (buf[0]) {
            failed++;
            if (!differs.count(call.entry))
                differs[call.entry] = buf;
        } else {
            t.second++;
        }
    }
    int functions_ok = 0, calls = 0, matched = 0;
    for (const auto& [entry, t] : tally) {
        calls += t.first;
        matched += t.second;
        if (t.second == t.first && t.first)
            functions_ok++;
        if (tally.size() <= 20)
            std::printf("native $%06X: %d of %d calls match the interpreter\n", entry, t.second, t.first);
    }
    for (const auto& [entry, why] : differs)
        std::printf("native $%06X differs: %s\n", entry, why.c_str());
    std::vector<std::pair<int, uint32_t>> top;
    for (const auto& [a, n] : stops)
        top.push_back({n, a});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 12; i++)
        std::printf("native: stopped %d times at a call to $%06X (not translated yet)\n", top[i].first, top[i].second);
    std::printf("native: %zu calls saved; %d replayed: %d match, %d differ (%d stopped partway, finished by the interpreter); "
                "%d didn't return without interrupts. %d of %zu functions match on every call.\n",
                g_native_calls.size(), calls, matched, failed, stopped, waited, functions_ok, tally.size());
    g_native_replaying = false;
    coi_native_tracing = 0;
    return failed;
}

// --native-probe F1,F2,...: translated code tried from every instruction it has, not only from
// where play reaches it. The machine is saved at the start of each frame listed; from each such
// state and each translated instruction, with two sets of registers (the state's own, and small
// data with address registers into the object RAM), a few scanlines' slices run interpreted
// (Musashi) and then translated (the native loop, as in play), interrupts held off (SR $27xx).
// Every slice's cycles, the registers, SR, PC, the stopped state, the writes, RAM, Z80 RAM and
// VDP memory must match. Code started out of its context does odd things (stray pointers,
// returns to wherever the stack says), and both must do the same odd things: that reaches the
// branches and states play doesn't. COI_PROBE_SLICES=N (8), COI_PROBE_STEP=N (every Nth
// instruction, 1), COI_PROBE_LIST=FILE (each address and its result), COI_PROBE_ONLY=ADDR (one,
// with instruction traces of the first difference). Exits 0 if all match.
struct ProbeState {
    int frame;
    std::vector<uint8_t> context;
    Bus bus;                                    // (its ROM left out: it doesn't change)
    VdpSnapshot vdp;
    int irq_pending, irq_level;
    bool nmi;
};
std::vector<int> g_probe_frames;
std::vector<ProbeState> g_probe_states;

void probe_capture(int frame)
{
    ProbeState s{frame, std::vector<uint8_t>(m68k_context_size()), Bus{}, save_vdp(), g_irq_pending, g_irq_level, g_nmi_pending};
    native_sync();
    m68k_get_context(s.context.data());
    std::vector<uint8_t> rom = std::move(g_bus.rom);
    s.bus = g_bus;
    g_bus.rom = std::move(rom);
    g_probe_states.push_back(std::move(s));
}

struct ProbeRun {
    std::vector<int> slices;
    std::vector<uint32_t> pcs;
    uint32_t regs[20] = {};                     // D0-D7, A0-A7, PC, SR, USP, ISP
    bool stopped = false;
    std::vector<WriteRecord> writes;
    std::vector<uint8_t> ram, z80;
    VdpSnapshot vdp;
    int64_t fallbacks = 0, native = 0;
};

int native_probe()
{
    JitInput in = g_jit_input;
    const uint32_t only = std::getenv("COI_PROBE_ONLY") ? std::strtoul(std::getenv("COI_PROBE_ONLY"), nullptr, 16) : 0;
    in.trace = only != 0;
    const JitResult tr = jit_translate(in, g_bus.rom, kArm64);
    if (!tr.code) {
        std::printf("probe: translating failed: %s\n", tr.error.c_str());
        return 1;
    }
    g_native_code = tr.code;
    recomp::jit::select(tr.code);
    g_native_rom_ok = true;
    const bool self_test = std::getenv("COI_PROBE_SELF") != nullptr;   // (the harness: interpreter twice)
    const bool native_twice = std::getenv("COI_PROBE_NATIVE_TWICE") != nullptr;     // (translated twice)
    const int nslices = std::getenv("COI_PROBE_SLICES") ? std::atoi(std::getenv("COI_PROBE_SLICES")) : 8;
    const int step = std::getenv("COI_PROBE_STEP") ? std::max(1, std::atoi(std::getenv("COI_PROBE_STEP"))) : 1;
    std::vector<uint32_t> targets;
    for (int i = 0; i < tr.code->resume_count; i++)
        if (tr.code->resume[i].address < 0x80000 && (only ? tr.code->resume[i].address == only : i % step == 0))
            targets.push_back(tr.code->resume[i].address);
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (only) {
        coi_native_tracing = 1;
        g_native_replaying = true;
        m68k_set_instr_hook_callback(native_hook);
    }
    // COI_PROBE_SEEN=FILE: the address of every instruction the interpreter ran in the probes,
    // one a line (what they reached, to set beside what play reaches).
    std::vector<uint8_t> seen;
    const char* seen_path = std::getenv("COI_PROBE_SEEN");
    if (seen_path) {
        seen.assign(0x80000, 0);
        g_native_replaying = true;
    }
    const bool hook = only || seen_path;

    auto run = [&](const ProbeState& st, uint32_t at, int seed, bool native, std::vector<TraceStep>* trace) {
        ProbeRun out;
        m68k_set_context(const_cast<uint8_t*>(st.context.data()));
        if (hook)                               // (the context holds Musashi's callbacks too)
            m68k_set_instr_hook_callback(native_hook);
        g_probe_seen = seen_path && !native ? &seen : nullptr;
        std::vector<uint8_t> rom = std::move(g_bus.rom);
        g_bus = st.bus;
        g_bus.rom = std::move(rom);
        restore_vdp(st.vdp);
        g_irq_pending = st.irq_pending;
        g_irq_level = st.irq_level;
        g_nmi_pending = st.nmi;
        g_ns.ours = false;
        g_reset_cycles = 0;
        if (seed) {                             // small data, address registers into the object RAM
            uint32_t h = at * 2654435761u + uint32_t(seed) * 40503u;
            auto next = [&] { h ^= h << 13; h ^= h >> 17; h ^= h << 5; return h; };
            for (int i = 0; i < 8; i++)
                m68k_set_reg(m68k_register_t(M68K_REG_D0 + i), next() % 0x40);
            for (int i = 0; i < 7; i++)
                m68k_set_reg(m68k_register_t(M68K_REG_A0 + i), 0xFFD000u + (next() % 0x60) * 0x40);
        }
        m68k_set_reg(M68K_REG_SR, (m68k_get_reg(nullptr, M68K_REG_SR) & 0x1F) | 0x2700);
        m68k_set_reg(M68K_REG_PC, at);
        m68k_set_reg(M68K_REG_PPC, at);
        coi_m68k_set_stopped(0);
        g_native_live = native;
        const int64_t fallbacks0 = g_native_fallbacks, native0 = g_native_cycles;
        g_write_log = &out.writes;
        g_trace = trace;
        g_probe_slice_used = 0;
        for (int s = 0; s < nslices; s++) {
            out.slices.push_back(cpu_execute(488));
            out.pcs.push_back(cpu_reg(M68K_REG_PC));
            g_probe_slice_used += out.slices.back();
        }
        g_write_log = nullptr;
        g_trace = nullptr;
        g_probe_seen = nullptr;
        native_sync();
        g_native_live = false;
        for (int i = 0; i < 16; i++)
            out.regs[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i));
        out.regs[16] = m68k_get_reg(nullptr, M68K_REG_PC);
        out.regs[17] = m68k_get_reg(nullptr, M68K_REG_SR);
        out.regs[18] = m68k_get_reg(nullptr, M68K_REG_USP);
        out.regs[19] = m68k_get_reg(nullptr, M68K_REG_ISP);
        out.stopped = coi_m68k_stopped();
        out.ram.assign(g_bus.ram, g_bus.ram + 0x10000);
        out.z80.assign(g_bus.z80_ram, g_bus.z80_ram + 0x2000);
        out.vdp = save_vdp();
        out.fallbacks = g_native_fallbacks - fallbacks0;
        out.native = g_native_cycles - native0;
        return out;
    };
    auto compare = [](const ProbeRun& n, const ProbeRun& r) -> std::string {
        char b[160];
        for (size_t s = 0; s < r.slices.size(); s++)
            if (n.slices[s] != r.slices[s] || n.pcs[s] != r.pcs[s]) {
                std::snprintf(b, sizeof(b), "slice %zu: %d cycles to $%06X, interpreter %d to $%06X", s, n.slices[s], n.pcs[s],
                              r.slices[s], r.pcs[s]);
                return b;
            }
        static const char* names[20] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1",
                                        "a2", "a3", "a4", "a5", "a6", "a7", "pc", "sr", "usp", "isp"};
        for (int i = 0; i < 20; i++)
            if (n.regs[i] != r.regs[i]) {
                std::snprintf(b, sizeof(b), "%s = %08X, interpreter %08X", names[i], n.regs[i], r.regs[i]);
                return b;
            }
        if (n.stopped != r.stopped)
            return "stopped state";
        if (n.writes != r.writes) {
            size_t i = 0;
            while (i < n.writes.size() && i < r.writes.size() && n.writes[i] == r.writes[i])
                i++;
            std::snprintf(b, sizeof(b), "write #%zu: $%06X/%u=%X, interpreter $%06X/%u=%X", i,
                          i < n.writes.size() ? n.writes[i].address : 0, i < n.writes.size() ? n.writes[i].size : 0,
                          i < n.writes.size() ? n.writes[i].value : 0, i < r.writes.size() ? r.writes[i].address : 0,
                          i < r.writes.size() ? r.writes[i].size : 0, i < r.writes.size() ? r.writes[i].value : 0);
            return b;
        }
        if (n.ram != r.ram)
            return "RAM";
        if (n.z80 != r.z80)
            return "Z80 RAM";
        if (n.vdp.vram != r.vdp.vram || n.vdp.cram != r.vdp.cram || n.vdp.vsram != r.vdp.vsram || n.vdp.reg != r.vdp.reg)
            return "VDP";
        return "";
    };

    FILE* list = std::getenv("COI_PROBE_LIST") ? std::fopen(std::getenv("COI_PROBE_LIST"), "w") : nullptr;
    int64_t probes = 0, differ = 0, handed = 0, untranslated = 0;
    std::map<uint32_t, std::string> first;      // address -> its first difference
    std::map<std::string, int> kinds;
    for (uint32_t at : targets) {
        std::string result;
        bool any_native = false;
        for (const ProbeState& st : g_probe_states)
            for (int seed = 0; seed < 2; seed++) {
                std::vector<TraceStep> rt, nt;      // (COI_PROBE_ONLY: the compared runs' traces)
                const ProbeRun r = run(st, at, seed, native_twice, only ? &rt : nullptr);
                const ProbeRun n = run(st, at, seed, !self_test, only ? &nt : nullptr);
                probes++;
                handed += n.fallbacks != 0;
                any_native |= n.native != 0;
                const std::string why = compare(n, r);
                if (!why.empty()) {
                    differ++;
                    if (result.empty()) {
                        char b[64];
                        std::snprintf(b, sizeof(b), "frame %d, seed %d: ", st.frame, seed);
                        result = b + why;
                        kinds[why.substr(0, why.find_first_of(" :="))]++;
                    }
                    if (only) {
                        // (Translated code reports an instruction, then may stop before it at the
                        // slice's end and report it again when the next slice resumes there.)
                        nt.erase(std::unique(nt.begin(), nt.end()), nt.end());
                        size_t i = 0;
                        while (i < nt.size() && i < rt.size() && nt[i] == rt[i])
                            i++;
                        if (const char* dump = std::getenv("COI_PROBE_DUMP"))   // FILE: both traces, in full
                            if (FILE* d = std::fopen(dump, "w")) {
                                for (size_t k = 0; k < std::max(nt.size(), rt.size()); k++)
                                    std::fprintf(d, "%5zu  %06X %6lld   %06X %6lld\n", k, k < nt.size() ? nt[k].pc : 0,
                                                 k < nt.size() ? (long long)nt[k].cycles : -1, k < rt.size() ? rt[k].pc : 0,
                                                 k < rt.size() ? (long long)rt[k].cycles : -1);
                                std::fclose(d);
                            }
                        size_t c = 0;                   // the first instruction started at another cycle
                        while (c < i && nt[c].cycles == rt[c].cycles)
                            c++;
                        if (c < i)
                            std::printf("probe $%06X: cycles part before #%zu: $%06X at %lld natively, %lld interpreted "
                                        "(the one before: $%06X)\n", at, c, nt[c].pc, (long long)nt[c].cycles,
                                        (long long)rt[c].cycles, c ? nt[c - 1].pc : 0);
                        std::printf("probe $%06X frame %d seed %d: %s; traces agree for %zu instructions (of %zu/%zu)\n", at,
                                    st.frame, seed, why.c_str(), i, nt.size(), rt.size());
                        for (size_t k = i > 2 ? i - 2 : 0; k <= i + 1; k++)
                            for (const auto* t : {&nt, &rt})
                                if (k < t->size()) {
                                    const TraceStep& s = (*t)[k];
                                    std::printf("  %s #%zu $%06X at %lld ccr %02X d:", t == &nt ? "native     " : "interpreter", k,
                                                s.pc, (long long)s.cycles, s.regs[16]);
                                    for (int q = 0; q < 16; q++)
                                        std::printf(q == 8 ? " a: %08X" : " %08X", s.regs[q]);
                                    std::printf("\n");
                                }
                    }
                }
            }
        if (!any_native)
            untranslated++;
        if (!result.empty())
            first[at] = result;
        if (list)
            std::fprintf(list, "%06X %s\n", at, result.empty() ? (any_native ? "ok" : "not run natively") : result.c_str());
    }
    if (list)
        std::fclose(list);
    if (seen_path)
        if (FILE* f = std::fopen(seen_path, "w")) {
            for (uint32_t a = 0; a < seen.size(); a++)
                if (seen[a])
                    std::fprintf(f, "%06X\n", a);
            std::fclose(f);
        }
    g_native_replaying = false;
    coi_native_tracing = 0;
    int shown = 0;
    for (const auto& [a, why] : first)
        if (shown++ < 40)
            std::printf("probe $%06X differs: %s\n", a, why.c_str());
    for (const auto& [k, n] : kinds)
        std::printf("probe: %d addresses first differ in %s\n", n, k.c_str());
    std::printf("probe: %zu instructions from %zu states, %lld runs: %lld differ (%zu addresses), %lld handed part of a slice "
                "to Musashi, %lld addresses never ran translated\n",
                targets.size(), g_probe_states.size(), (long long)probes, (long long)differ, first.size(), (long long)handed,
                (long long)untranslated);
    return first.empty() ? 0 : 1;
}
#endif  // COI_NO_INTERPRETER
#endif

#if defined(COI_NATIVE) && !defined(COI_NO_INTERPRETER)
namespace {
bool g_native_selftest = false;     // --native-selftest

// --native-selftest: the exceptions the game never takes (trap, trapv, chk, a divide by zero,
// illegal, line A/F, stop), each checked against Musashi on a small image of our own, not the
// game's: vectors, a handler per exception that records what it was given (the frame's SR and
// PC, A7, its own SR, a count) in RAM and returns (past the instruction, for illegal and line
// A/F), and short routines that raise them, mid-block too. Each test runs interpreted (Musashi
// alone) and then translated (the native loop), slice by slice as the game runs, at several
// slice sizes so the budget runs out at many points: every slice's cycles, then the registers,
// SR, PC, the stopped state and RAM must match, and the translated run must hand nothing to
// Musashi. Translated with the JIT for the CPU it runs on. Exits 0 if all match.
struct SelfAsm {
    std::vector<uint8_t>& m;
    uint32_t pc;
    std::vector<uint32_t>* entries;
    void w(uint32_t v) { m[pc] = uint8_t(v >> 8); m[pc + 1] = uint8_t(v); pc += 2; }
    void l(uint32_t v) { w(v >> 16); w(v & 0xFFFF); }
    void resume() { entries->push_back(pc); }   // code reached only from a handler's rte
    void mark() { w(0x5279); l(0xFF0000); }     // addq.w #1,($FF0000).l: a step done
};

struct SelfRun {
    std::vector<int> slices;
    std::vector<uint32_t> pcs;                  // where each slice ended
    uint32_t regs[20] = {};                     // D0-D7, A0-A7, PC, SR, USP, SSP
    bool stopped = false;
    std::vector<uint8_t> ram;
    int64_t fallbacks = 0;
};

int native_selftest()
{
    std::vector<uint8_t> img(0x4000, 0);
    std::vector<uint32_t> entries;
    SelfAsm a{img, 0, &entries};
    // Vectors: the SSP, then every exception to a handler that records what it saw.
    const uint32_t kBad = 0x3F00;
    a.pc = 0; a.l(0x00FFFE00); a.l(0x1000);
    for (int v = 2; v < 64; v++) a.l(kBad);
    a.pc = kBad; a.w(0x60FE);                   // bra.s * (an exception no test expects)
    entries.push_back(kBad);
    const int vectors[] = {4, 5, 6, 7, 8, 10, 11, 30, 32, 33, 34, 35, 39, 47};
    uint32_t h = 0x200;
    for (int v : vectors) {
        a.pc = uint32_t(v) * 4; a.l(h);
        a.pc = h;
        entries.push_back(h);
        const uint32_t slot = 0xFF0100 + uint32_t(v) * 16;
        a.w(0x33D7); a.l(slot);                 // move.w (a7),slot.l: the frame's SR
        a.w(0x23EF); a.w(2); a.l(slot + 2);     // move.l 2(a7),slot+2.l: the frame's PC
        a.w(0x23CF); a.l(slot + 6);             // move.l a7,slot+6.l
        a.w(0x40F9); a.l(slot + 10);            // move.w sr,slot+10.l: the handler's SR
        a.w(0x5279); a.l(slot + 12);            // addq.w #1,slot+12.l
        if (v == 4 || v == 10 || v == 11) { a.w(0x54AF); a.w(2); }     // addq.l #2,2(a7): past it
        if (v == 8) { a.w(0x58AF); a.w(2); }    // addq.l #4,2(a7): past a privileged 4 bytes
        a.w(0x4E73);                            // rte
        h = a.pc + 2;
    }
    // handover: what the translator doesn't take (movep, a branch to an odd address) is the
    // interpreter's: those slices may go to Musashi, and everything must still match.
    struct Test { const char* name; uint32_t entry; int irq_at[2]; bool handover = false; };
    std::vector<Test> tests;
    auto begin = [&](const char* name, uint32_t at, int irq0 = -1, int irq1 = -1) {
        a.pc = at;
        entries.push_back(at);
        tests.push_back({name, at, {irq0, irq1}});
    };
    auto addq_d0 = [&](int n) { for (int i = 0; i < n; i++) a.w(0x5280); };   // addq.l #1,d0

    begin("trap #0, #1, #7, #15", 0x1000);
    a.w(0x7005);                                // moveq #5,d0
    a.w(0x4E40); a.resume();                    // trap #0
    addq_d0(1); a.w(0x4E4F); a.resume();        // trap #15
    a.w(0x4E71); a.w(0x4E47); a.resume();       // nop; trap #7
    a.w(0x4E41); a.resume();                    // trap #1
    a.mark(); a.w(0x60FE);

    begin("divu/divs by zero", 0x1100);
    a.w(0x203C); a.l(1234);                     // move.l #1234,d0
    a.w(0x7200);                                // moveq #0,d1
    a.w(0x80C1); a.mark();                      // divu.w d1,d0
    a.w(0x81C1);                                // divs.w d1,d0
    a.w(0x41F9); a.l(0xFF8000);                 // lea $FF8000,a0 (a zero word)
    a.w(0x80D0);                                // divu.w (a0),d0
    a.w(0x80D8);                                // divu.w (a0)+,d0
    a.w(0x81E0);                                // divs.w -(a0),d0
    a.w(0x80FC); a.w(0);                        // divu.w #0,d0
    a.w(0x80FC); a.w(7);                        // divu.w #7,d0 (no exception)
    a.w(0x81F8); a.w(0x8000);                   // divs.w ($FFFF8000).w,d0
    a.mark(); a.w(0x60FE);

    begin("chk below 0, above, within", 0x1200);
    a.w(0x303C); a.w(0xFFFF);                   // move.w #-1,d0
    a.w(0x41BC); a.w(10);                       // chk.w #10,d0: below 0
    a.w(0x303C); a.w(11);                       // move.w #11,d0
    a.w(0x41BC); a.w(10);                       // chk.w #10,d0: above
    a.w(0x303C); a.w(5);                        // move.w #5,d0
    a.w(0x41BC); a.w(10);                       // chk.w #10,d0: within
    a.w(0x323C); a.w(10);                       // move.w #10,d1
    a.w(0x4181);                                // chk.w d1,d0
    a.w(0x303C); a.w(0);                        // move.w #0,d0
    a.w(0x44FC); a.w(0x0F);                     // move #$0F,ccr
    a.w(0x41BC); a.w(0xFFFD);                   // chk.w #-3,d0: a negative bound
    a.w(0x41F9); a.l(0xFF8000);                 // lea $FF8000,a0
    a.w(0x4198);                                // chk.w (a0)+,d0: bound 0, within
    a.mark(); a.w(0x60FE);

    begin("trapv with V clear and set", 0x1300);
    a.w(0x44FC); a.w(0x00); a.w(0x4E76);        // move #0,ccr; trapv
    a.w(0x44FC); a.w(0x02); a.w(0x4E76);        // move #2,ccr; trapv
    a.w(0x44FC); a.w(0x1F); a.w(0x4E76);        // move #$1F,ccr; trapv
    a.mark(); a.w(0x60FE);

    begin("illegal, line A, line F", 0x1400);
    a.w(0x7001);                                // moveq #1,d0
    a.w(0x4AFC); a.resume();                    // illegal
    a.w(0xA123); a.resume();                    // line A
    a.w(0xF456); a.resume();                    // line F
    a.mark(); a.w(0x60FE);

    begin("stop, an interrupt later; stop at mask 7", 0x1500, 3000, 9000);
    a.w(0x7400);                                // moveq #0,d2
    a.w(0x4E72); a.w(0x2000); a.resume();       // stop #$2000
    a.mark();
    a.w(0x4E72); a.w(0x2700); a.resume();       // stop #$2700: no interrupt gets in
    a.mark(); a.w(0x60FE);

    begin("stop with an interrupt waiting", 0x1600, 0);
    a.w(0x7603);                                // moveq #3,d3
    a.w(0x4E72); a.w(0x2300); a.resume();       // stop #$2300: taken at once
    a.mark(); a.w(0x60FE);

    begin("exceptions mid-block", 0x1700);
    a.w(0x7000); a.w(0x7200);                   // moveq #0,d0; moveq #0,d1
    addq_d0(5); a.w(0x80C1);                    // divu.w d1,d0
    addq_d0(5); a.w(0x41BC); a.w(2);            // chk.w #2,d0
    addq_d0(5); a.w(0x44FC); a.w(0x02); a.w(0x4E76);    // trapv
    addq_d0(5); a.w(0x4E41); a.resume();        // trap #1
    addq_d0(5); a.w(0x4AFC); a.resume();        // illegal
    addq_d0(5); a.mark(); a.w(0x60FE);

    begin("rtr", 0x1900);
    a.w(0x2F3C); a.l(0x1940);                   // move.l #$1940,-(a7)
    a.w(0x3F3C); a.w(0x0013);                   // move.w #$13,-(a7)
    a.w(0x4E77);                                // rtr (to code no entry names: on demand)
    a.pc = 0x1940;
    a.mark(); a.w(0x60FE);

    begin("the reset instruction", 0x1A00);
    a.w(0x7001); a.w(0x4E70);                   // moveq #1,d0; reset
    a.mark(); a.w(0x60FE);

    begin("move to sr from memory", 0x1B00);
    a.w(0x33FC); a.w(0x2304); a.l(0xFF8002);    // move.w #$2304,($FF8002).l
    a.w(0x46F9); a.l(0xFF8002);                 // move.w ($FF8002).l,sr
    a.w(0x41F9); a.l(0xFF8002);                 // lea $FF8002,a0
    a.w(0x46D8);                                // move.w (a0)+,sr
    a.w(0x46FC); a.w(0x2711);                   // move #$2711,sr
    a.mark(); a.w(0x60FE);

    // User mode: entered with move to sr, andi and eori; privileged instructions there (the
    // violation's handler steps over 4 bytes: each is padded with a nop to 4), a trap and an
    // interrupt taken from it, and the stack pointers swapped each way.
    for (int how = 0; how < 3; how++) {
        static const char* names[] = {"user mode (move to sr), privilege, trap, interrupt",
                                      "user mode (andi to sr)", "user mode (eori to sr)"};
        begin(names[how], 0x1C00 + uint32_t(how) * 0x100, 4000);
        a.w(0x207C); a.l(0xFFC000);             // movea.l #$FFC000,a0
        a.w(0x4E60);                            // move a0,usp
        if (how == 0) { a.w(0x46FC); a.w(0x0000); }         // move #0,sr
        if (how == 1) { a.w(0x027C); a.w(0xD8FF); }         // andi #$D8FF,sr
        if (how == 2) { a.w(0x0A7C); a.w(0x2700); }         // eori #$2700,sr
        a.w(0x23CF); a.l(0xFF0020);             // move.l a7,($FF0020).l: the USP
        a.w(0x4E42); a.resume();                // trap #2 (to supervisor mode and back)
        a.w(0x46FC); a.w(0x2700);               // move #$2700,sr: privileged
        a.w(0x027C); a.w(0xDFFF);               // andi #$DFFF,sr: privileged
        a.w(0x4E69); a.w(0x4E71);               // move usp,a1: privileged; nop
        a.w(0x4E73); a.w(0x4E71);               // rte: privileged; nop
        a.w(0x4E70); a.w(0x4E71);               // reset: privileged; nop
        a.w(0x4E72); a.w(0x2000);               // stop #$2000: privileged
        a.w(0x2F00);                            // move.l d0,-(a7): on the USP
        a.w(0x4E43); a.resume();                // trap #3
        a.mark(); a.w(0x60FE);                  // (the interrupt comes here, in user mode)
    }

    // movep (the game has none; the decoder doesn't take it): all four forms, mid-block. These
    // two go in the list as the tracer's functions do (in the game a function the decoder can't
    // take is left out, and the interpreter runs it), not as entries, which must translate.
    std::vector<uint32_t> listed;
    begin("movep (the interpreter's)", 0x2000);
    tests.back().handover = true;
    entries.pop_back(); listed.push_back(0x2000);
    a.w(0x203C); a.l(0x12345678);               // move.l #$12345678,d0
    a.w(0x41F9); a.l(0xFF8100);                 // lea $FF8100,a0
    addq_d0(3);
    a.w(0x01C8); a.w(0x0000);                   // movep.l d0,0(a0)
    a.w(0x0188); a.w(0x0009);                   // movep.w d0,9(a0)
    a.w(0x0348); a.w(0x0000);                   // movep.l 0(a0),d1
    a.w(0x0508); a.w(0x0009);                   // movep.w 9(a0),d2
    addq_d0(3); a.mark(); a.w(0x60FE);

    // A short branch with displacement $FF: on a 68000 a branch to an odd address (Musashi
    // emulates no address error and runs on from there). Not taken, then taken.
    begin("bcc.s/bra.s $FF (the interpreter's)", 0x2100);
    tests.back().handover = true;
    entries.pop_back(); listed.push_back(0x2100);
    a.w(0x7000);                                // moveq #0,d0
    a.w(0x44FC); a.w(0x00);                     // move #0,ccr
    a.w(0x67FF);                                // beq.s $FF: not taken
    addq_d0(2); a.mark();
    a.w(0x60FF);                                // bra.s $FF: to the odd address after it
    for (int i = 0; i < 8; i++) a.w(0x4E71);    // nop (what the odd fetches read through)
    a.w(0x60FE);

    // Code no entry names: reached through registers, so translated on demand.
    begin("jsr and jmp through a register, on demand", 0x1800);
    a.w(0x41F9); a.l(0x1880);                   // lea $1880,a0
    a.w(0x4E90);                                // jsr (a0)
    a.mark();
    a.w(0x43F9); a.l(0x18A0);                   // lea $18A0,a1
    a.w(0x4ED1);                                // jmp (a1)
    entries.pop_back();                         // (begin listed it: this one isn't)
    a.pc = 0x1880;
    a.w(0x7807); a.w(0x5284); a.w(0x4E75);      // moveq #7,d4; addq.l #1,d4; rts
    a.pc = 0x18A0;
    a.w(0x7A09); a.mark(); a.w(0x60FE);         // moveq #9,d5; mark; bra.s *
    entries.push_back(0x1800);

    JitInput in;
    in.config.functions = entries;
    in.list = listed;
    g_jit_input = in;                           // (what on-demand translations use)
    const JitResult r = jit_translate(in, img, kArm64);
    if (!r.code) {
        std::printf("selftest: translating failed: %s\n", r.error.c_str());
        return 1;
    }
    for (const auto& [address, why] : r.failed)
        std::printf("selftest: left out $%06X: %s\n", address, why.c_str());

    g_bus.rom = img;
    g_rom_hash_known = false;
    g_native_code = r.code;
    recomp::jit::select(r.code);
    g_native_rom_ok = true;

    auto run = [&](const Test& t, int slice, bool native) {
        SelfRun out;
        std::fill(g_bus.ram, g_bus.ram + 0x10000, 0);
        g_irq_pending = 0;
        set_irq(0);
        g_native_live = native;
        native_reset();
        m68k_pulse_reset();
        if (native)
            g_reset_cycles = coi_m68k_take_reset_cycles();
        for (int i = 0; i < 8; i++) {
            m68k_set_reg(m68k_register_t(M68K_REG_D0 + i), 0x11111111u * uint32_t(i + 1));
            if (i < 7)
                m68k_set_reg(m68k_register_t(M68K_REG_A0 + i), 0xFF9000u + uint32_t(i) * 0x100);
        }
        m68k_set_reg(M68K_REG_SR, 0x2700);
        m68k_set_reg(M68K_REG_USP, 0xFFD000);
        m68k_set_reg(M68K_REG_PC, t.entry);
        const int64_t fallbacks0 = g_native_fallbacks;
        int64_t used = 0;
        const int n = 12000 / slice + 1;
        for (int s = 0; s < n; s++) {
            for (int at : t.irq_at)
                if (at >= 0 && s == at / slice)
                    raise_irq(6);
            const int c = cpu_execute(slice);
            out.slices.push_back(c);
            out.pcs.push_back(cpu_reg(M68K_REG_PC));
            used += c;
        }
        native_sync();
        for (int i = 0; i < 16; i++)
            out.regs[i] = m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i));
        out.regs[16] = m68k_get_reg(nullptr, M68K_REG_PC);
        out.regs[17] = m68k_get_reg(nullptr, M68K_REG_SR);
        out.regs[18] = m68k_get_reg(nullptr, M68K_REG_USP);
        out.regs[19] = m68k_get_reg(nullptr, M68K_REG_ISP);
        out.stopped = coi_m68k_stopped();
        out.ram.assign(g_bus.ram, g_bus.ram + 0x10000);
        out.fallbacks = g_native_fallbacks - fallbacks0;
        g_native_live = false;
        return out;
    };
    std::vector<int> slices = {488, 100, 61, 23, 7, 1};    // COI_SELFTEST_SLICES=a,b,...: others
    if (const char* e = std::getenv("COI_SELFTEST_SLICES")) {
        slices.clear();
        for (const char* p = e; *p; ) {
            slices.push_back(std::atoi(p));
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
    }
    int failed = 0, runs = 0;
    const int64_t exceptions0 = g_native_exceptions, interrupts0 = g_native_interrupts;
    for (const Test& t : tests)
        for (int slice : slices) {
            const SelfRun i = run(t, slice, false);
            const SelfRun n = run(t, slice, true);
            runs++;
            char why[200] = "";
            for (size_t s = 0; s < i.slices.size() && !why[0]; s++)
                if (i.slices[s] != n.slices[s])
                    std::snprintf(why, sizeof(why), "slice %zu used %d cycles, interpreter %d (ended at $%06X, interpreter $%06X; "
                                  "the slice before at $%06X, $%06X)", s, n.slices[s], i.slices[s], n.pcs[s], i.pcs[s],
                                  s ? n.pcs[s - 1] : 0, s ? i.pcs[s - 1] : 0);
            static const char* names[20] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2",
                                            "a3", "a4", "a5", "a6", "a7", "PC", "SR", "USP", "SSP"};
            for (int k = 0; k < 20 && !why[0]; k++)
                if (i.regs[k] != n.regs[k])
                    std::snprintf(why, sizeof(why), "%s = %08X, interpreter %08X", names[k], n.regs[k], i.regs[k]);
            if (!why[0] && i.stopped != n.stopped)
                std::snprintf(why, sizeof(why), "stopped %d, interpreter %d", n.stopped, i.stopped);
            for (int k = 0; k < 0x10000 && !why[0]; k++)
                if (i.ram[k] != n.ram[k])
                    std::snprintf(why, sizeof(why), "RAM $FF%04X = %02X, interpreter %02X", k, n.ram[k], i.ram[k]);
            if (!why[0] && n.fallbacks && !t.handover)
                std::snprintf(why, sizeof(why), "%lld slices handed to Musashi", (long long)n.fallbacks);
            if (!why[0] && i.ram[1] == 0 && i.ram[0] == 0)
                std::snprintf(why, sizeof(why), "the test never got to its end");
            std::printf("selftest: %-42s slices of %3d: %s", t.name, slice, why[0] ? why : "identical");
            if (t.handover)
                std::printf(" (%lld slices handed to Musashi)", (long long)n.fallbacks);
            std::printf("\n");
            if (std::getenv("COI_SELFTEST_DUMP"))      // what the handlers saw: counts per vector, $FF0020
                std::printf("  privilege %d, trap #2 %d, trap #3 %d, level 6 %d; USP seen %08X; SR in the level 6 frame %04X\n",
                            n.ram[0x18D], n.ram[0x32D], n.ram[0x33D], n.ram[0x2ED],
                            uint32_t(n.ram[0x20] << 24 | n.ram[0x21] << 16 | n.ram[0x22] << 8 | n.ram[0x23]),
                            n.ram[0x2E0] << 8 | n.ram[0x2E1]);
            failed += why[0] != 0;
        }
    std::printf("selftest: %d of %d runs identical; translated code took %lld exceptions and %lld interrupts; "
                "%zu translated on demand\n", runs - failed, runs, (long long)(g_native_exceptions - exceptions0),
                (long long)(g_native_interrupts - interrupts0), g_on_demand.size());
    return failed ? 1 : 0;
}
}  // namespace
#endif

// The PC and SR for the headless report, wherever the CPU state is.
uint32_t cpu_pc_for_report()
{
#ifdef COI_NATIVE
    return cpu_reg(M68K_REG_PC);
#else
    return m68k_get_reg(nullptr, M68K_REG_PC);
#endif
}

uint32_t cpu_sr_for_report()
{
#ifdef COI_NATIVE
    return cpu_reg(M68K_REG_SR);
#else
    return m68k_get_reg(nullptr, M68K_REG_SR);
#endif
}

#if defined(COI_NATIVE) && !defined(COI_NO_INTERPRETER)
// --write-cycles FILE: Musashi's 68000 cycle tables (per opcode, run-length coded, and per
// exception) as C, for builds without Musashi (port/recomp/cycles68000.c); and the check, at
// every start-up of a development build, that the file still matches Musashi.
std::string g_write_cycles;

// --decode-check: the decoder against Musashi's opcode table, over all 65,536 opcodes (with
// zero extension words): which ones Musashi runs as 68000 instructions (a cycle count; 0 means
// it takes the illegal-instruction exception) that the decoder rejects, and the other way.
int native_decode_check()
{
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    if (std::getenv("COI_DECODE_TABLE"))        // (with the opcode table, as the game decodes)
        recomp::use_opcode_table(m68ki_cycles[0]);
    std::vector<uint8_t> bytes(16, 0);
    const recomp::Rom rom{bytes};
    std::map<std::string, std::vector<uint16_t>> ours_only, musashi_only;
    static const char* names[] = {"Unknown", "Move", "Movea", "Moveq", "Movem", "Lea", "Pea", "Exg", "Swap", "Clr",
        "Link", "Unlk", "Add", "Adda", "Addq", "Addx", "Sub", "Suba", "Subq", "Subx", "Cmp", "Cmpa", "Cmpm", "And",
        "Or", "Eor", "Neg", "Negx", "Not", "Ext", "Tst", "Tas", "Mulu", "Muls", "Divu", "Divs", "Abcd", "Sbcd",
        "Nbcd", "Asl", "Asr", "Lsl", "Lsr", "Rol", "Ror", "Roxl", "Roxr", "Btst", "Bchg", "Bclr", "Bset", "Bra",
        "Bsr", "Bcc", "DBcc", "Scc", "Jmp", "Jsr", "Rts", "Rte", "Rtr", "Trap", "Nop", "AndiCcr", "OriCcr",
        "EoriCcr", "AndiSr", "OriSr", "EoriSr", "MoveToCcr", "MoveToSr", "MoveFromSr", "MoveToUsp", "MoveFromUsp",
        "Stop", "Reset", "Illegal", "Trapv", "Chk", "LineA", "LineF"};
    for (uint32_t op = 0; op < 0x10000; op++) {
        bytes[0] = uint8_t(op >> 8);
        bytes[1] = uint8_t(op);
        const recomp::Insn in = recomp::decode(rom, 0);
        const bool ours = in.op != recomp::Op::Unknown && in.op != recomp::Op::Illegal && in.op != recomp::Op::LineA
                          && in.op != recomp::Op::LineF;
        const bool musashi = m68ki_cycles[0][op] != 0 && op != 0x4AFC && (op >> 12) != 0xA && (op >> 12) != 0xF;
        if (ours && !musashi)
            ours_only[names[int(in.op)]].push_back(uint16_t(op));
        else if (musashi && !ours)
            musashi_only[std::to_string(op >> 12)].push_back(uint16_t(op));
    }
    auto show = [](const char* what, const std::map<std::string, std::vector<uint16_t>>& m) {
        size_t n = 0;
        for (const auto& [k, v] : m)
            n += v.size();
        std::printf("%s: %zu opcodes\n", what, n);
        for (const auto& [k, v] : m) {
            std::printf("  %-10s %5zu:", k.c_str(), v.size());
            for (size_t i = 0; i < v.size() && i < 10; i++)
                std::printf(" %04X", v[i]);
            std::printf("\n");
        }
    };
    show("decoded but illegal to Musashi", ours_only);
    show("run by Musashi but not decoded (by top nibble)", musashi_only);
    return 0;
}

bool native_write_cycles(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;
    std::fprintf(f, "/* The 68000's cycle counts as Musashi counts them (third_party/Musashi, MIT licence): per\n"
                    "   opcode (m68ki_cycles[0], each opcode with its addressing; runs of a count and a value)\n"
                    "   and per exception vector (m68ki_exception_cycle_table[0]). Written by a development build\n"
                    "   (coi --write-cycles port/recomp/cycles68000.c), which checks it against Musashi's own at\n"
                    "   start-up; builds without Musashi (COI_NO_INTERPRETER) count with it. */\n"
                    "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n\n"
                    "#ifdef COI_NO_INTERPRETER\nunsigned char m68ki_cycles[1][0x10000];    /* (native_rt.h's name for it) */\n#endif\n\n"
                    "static const uint16_t kRuns[] = {");
    int n = 0;
    for (uint32_t op = 0; op < 0x10000;) {
        uint32_t end = op;
        while (end < 0x10000 && end - op < 0xFFFF && m68ki_cycles[0][end] == m68ki_cycles[0][op])
            end++;
        std::fprintf(f, "%s%u,%u,", n++ % 12 ? " " : "\n    ", end - op, m68ki_cycles[0][op]);
        op = end;
    }
    std::fprintf(f, "\n};\n\nconst uint8_t coi_exception_cycles_68000[256] = {");
    for (int v = 0; v < 256; v++)
        std::fprintf(f, "%s%u,", v % 16 ? " " : "\n    ", coi_m68k_exception_cycles(unsigned(v)));
    std::fprintf(f, "\n};\n\nvoid coi_cycles_68000(uint8_t* out)\n{\n    size_t at = 0;\n"
                    "    for (size_t i = 0; i + 1 < sizeof(kRuns) / sizeof(kRuns[0]); i += 2) {\n"
                    "        memset(out + at, kRuns[i + 1], kRuns[i]);\n        at += kRuns[i];\n    }\n}\n");
    std::fclose(f);
    return true;
}

bool native_check_cycles()
{
    if (!g_write_cycles.empty()) {
        if (!native_write_cycles(g_write_cycles)) {
            std::fprintf(stderr, "can't write %s\n", g_write_cycles.c_str());
            return false;
        }
        std::printf("wrote %s\n", g_write_cycles.c_str());
        std::exit(0);
    }
    static uint8_t ours[0x10000];
    coi_cycles_68000(ours);
    bool same = std::memcmp(ours, m68ki_cycles[0], sizeof(ours)) == 0;
    for (int v = 0; v < 256; v++)
        same = same && coi_exception_cycles_68000[v] == coi_m68k_exception_cycles(unsigned(v));
    if (!same)
        std::fprintf(stderr, "port/recomp/cycles68000.c doesn't match Musashi's cycle tables: write it again with "
                             "coi --write-cycles port/recomp/cycles68000.c\n");
    return same;
}
#endif

uint32_t current_pc()
{
#ifdef COI_NATIVE
    if (g_native_cpu)
        return g_native_cpu->pc;
    return cpu_reg(M68K_REG_PPC);
#else
    return m68k_get_reg(nullptr, M68K_REG_PPC);
#endif
}

// A data (0-7) or address (8-15) register as the instruction running sees it.
uint32_t current_reg(int r)
{
#ifdef COI_NATIVE
    if (g_native_cpu)
        return r < 8 ? g_native_cpu->d[r] : g_native_cpu->a[r - 8];
    return cpu_reg(m68k_register_t(M68K_REG_D0 + r));
#else
    return m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + r));
#endif
}

extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return g_bus.read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return g_bus.read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return g_bus.read16(a) << 16 | g_bus.read16(a + 2); }
#ifdef COI_NATIVE
#define LOG_WRITE(a, size, v) if (g_write_log) g_write_log->push_back({(a) & 0xFFFFFF, size, (v)})
#else
#define LOG_WRITE(a, size, v)
#endif
void m68k_write_memory_8(unsigned int a, unsigned int v) { LOG_WRITE(a, 1, v); g_bus.write8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { LOG_WRITE(a, 2, v); g_bus.write16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { LOG_WRITE(a, 4, v); g_bus.write16(a, v >> 16); g_bus.write16(a + 2, v & 0xFFFF); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return g_bus.read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return g_bus.read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

int genesis_int_ack(int level)
{
#ifdef COI_NATIVE
    if (g_native_strict && g_native_live && g_native_rom_ok && !g_native_acking && !kNoInterpreter) {
        std::fflush(stdout);
        std::fprintf(stderr, "native strict: frame %d: Musashi took a level %d interrupt at $%06X\n", g_native_frame, level,
                     cpu_reg(M68K_REG_PC));
        std::fflush(stderr);
        std::_Exit(3);
    }
#endif
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

#ifdef __ANDROID__
// Android drops what a program writes to stdout and stderr; this sends it, line by line, to the
// log instead (adb logcat -s COI), so the port's messages (a shader that won't compile, an MD+
// pack not used) can be read on a phone too.
void log_output()
{
    int fds[2];
    if (pipe(fds) != 0)
        return;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    std::thread([fd = fds[0]] {
        std::string line;
        char buffer[512];
        for (ssize_t n; (n = read(fd, buffer, sizeof(buffer))) > 0;)
            for (ssize_t i = 0; i < n; i++) {
                if (buffer[i] != '\n') {
                    line += buffer[i];
                    continue;
                }
                __android_log_write(ANDROID_LOG_INFO, "COI", line.c_str());
                line.clear();
            }
    }).detach();
}
#endif

int main(int argc, char** argv)
{
#ifdef __ANDROID__
    log_output();
#endif
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], "--console") == 0)
            show_console();
    std::string rom_path, dump_path, wav_path, ram_path, vram_path, cram_path, snapshot_dir, sprite_capture_dir;
    int headless_frames = 0;
    std::vector<Press> presses;
    std::vector<std::pair<int, int>> fast_forwards; // --fast-forward FIRST-LAST, in pictures
    std::string state_hash_path;                    // --state-hash: one line per game frame
    std::vector<uint32_t> peeks;                   // RAM words to print after a headless run
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
    bool no_widescreen = false, easy_bounce = false, smooth_swing = false, no_smooth_swing = false, smooth_scroll = false, crt = false;
    bool no_smooth_scroll = false, vanilla_rock_edges = false, vanilla_fades = false, vanilla_water = false, vanilla_glass = false, show_fps = false, show_coordinates = false, interpolate = false;
    bool no_interpolate = false, log_mdplus = false;
    int mdplus = -1;                                // --mdplus / --no-mdplus
    double interp_back = -1;                        // --interp-back (testing)
    double refresh_override = 0;                    // --refresh (testing)
    int frame_rate = -1;                            // --frame-rate
    std::vector<std::string> cheats;                // --cheat NAME (this run only)
    int scanlines = -1;                             // percent, -1 = the saved setting
    int hqx = -1;                                   // scale, -1 = the saved setting
    float clean_edge_width = -1;                    // cleanEdge's line width, -1 = the saved setting
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
        else if (arg == "--no-smooth-swing") no_smooth_swing = true;
        else if (arg == "--smooth-scroll") smooth_scroll = true;
        else if (arg == "--no-smooth-scroll") no_smooth_scroll = true;
        else if (arg == "--vanilla-rock-edges") vanilla_rock_edges = true;
        else if (arg == "--vanilla-fades") vanilla_fades = true;
        else if (arg == "--vanilla-water") vanilla_water = true;
        else if (arg == "--vanilla-glass") vanilla_glass = true;
        else if (arg == "--show-fps") show_fps = true;
        else if (arg == "--show-coordinates") show_coordinates = true;
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
        else if (arg == "--stdout" && i + 1 < argc) {   // (Android has none; an app's process outlives main)
            if (std::freopen(argv[++i], "w", stdout))
                std::setvbuf(stdout, nullptr, _IOLBF, 1024);
        }
        else if (arg == "--scanlines" && i + 1 < argc) scanlines = std::atoi(argv[++i]);
        else if (arg == "--hqx" && i + 1 < argc) hqx = std::atoi(argv[++i]);
        else if (arg == "--clean-edge-width" && i + 1 < argc) clean_edge_width = (float)std::atof(argv[++i]);
        else if (arg == "--capture" && i + 2 < argc) {
            capture_frame = std::atoi(argv[++i]);
            capture_path = argv[++i];
        }
        else if (arg == "--peek" && i + 1 < argc) peeks.push_back(std::strtoul(argv[++i], nullptr, 16));
        else if (arg == "--ram" && i + 1 < argc) ram_path = argv[++i];
#ifndef NDEBUG
        else if (arg == "--snapshots" && i + 1 < argc) snapshot_dir = argv[++i];
        else if (arg == "--sprite-capture" && i + 1 < argc) sprite_capture_dir = argv[++i];
#endif
        else if (arg == "--save-rom" && i + 1 < argc) g_save_rom_path = argv[++i];
        else if (arg == "--save-rom-family" && i + 1 < argc) g_save_rom_family = argv[++i];
#ifdef COI_NATIVE_DEV
        else if (native_dev_arg(arg, i, argc, argv)) {}
#endif
#ifdef COI_NATIVE
        else if (arg == "--native-test" && i + 1 < argc) {
            g_native_limit = std::atoi(argv[++i]);
            g_native_live = false;                  // (it saves the calls the interpreter makes)
        }
#ifndef COI_NO_INTERPRETER
        else if (arg == "--decode-check") return native_decode_check();
#endif
#ifndef COI_NO_INTERPRETER
        else if (arg == "--native-probe" && i + 1 < argc) {
            for (const char* p = argv[++i]; *p;) {
                g_probe_frames.push_back(std::atoi(p));
                while (*p && *p != ',') p++;
                if (*p) p++;
            }
        }
#endif
        else if (arg == "--native") g_native_live = true;
        else if (arg == "--no-native") {
            if (kNoInterpreter)
                std::fprintf(stderr, "--no-native: this build has no interpreter, so translated code runs\n");
            else
                g_native_live = false;
        }
#ifndef COI_NO_INTERPRETER
        else if (arg == "--native-selftest") g_native_selftest = true;
        else if (arg == "--write-cycles" && i + 1 < argc) g_write_cycles = argv[++i];
#endif
        else if (arg == "--jit") g_native_jit = true;
#else
        else if (arg == "--no-native") {}           // (no translator built in: the interpreter runs anyway)
#endif
        else if (arg == "--vram" && i + 1 < argc) vram_path = argv[++i];
        else if (arg == "--cram" && i + 1 < argc) cram_path = argv[++i];
        else if (arg == "--log-state") log_state = true;
        else if (arg == "--show-state") show_state = true;
        else if (arg == "--show-touch") touch_force_shown();
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
        } else if (arg == "--fast-forward" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const size_t dash = spec.find('-');
            if (dash == std::string::npos) {
                std::fprintf(stderr, "bad --fast-forward value (FIRST-LAST): %s\n", spec.c_str());
                return 1;
            }
            fast_forwards.push_back({std::atoi(spec.c_str()), std::atoi(spec.c_str() + dash + 1)});
        } else if (arg == "--state-hash" && i + 1 < argc) {
            state_hash_path = argv[++i];
        } else rom_path = arg;
    }

    g_rom_original = load_rom(rom_path);
#ifdef __ANDROID__
    if (g_rom_original.empty())                     // first start: ask for it
        g_rom_original = android_pick_rom();
    if (g_rom_original.empty())
        return 0;
#endif
    if (g_rom_original.empty()) {
        std::fprintf(stderr, "No 512 KB ROM found (pass a path, or put it in rom/).\n");
        if (headless_frames == 0)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Castle of Illusion",
                                     "No ROM found.\n\nPut your own dump of the US Genesis release (a 512 KB .bin file) "
                                     "in the rom folder beside coi.exe, then start it again.", nullptr);
        return 1;
    }
    g_rom_cartridge = g_rom_original;
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
    }
    find_mdplus_pack();
    Settings settings;                              // defaults for headless runs, so tests repeat
    if (headless_frames == 0)
        settings = load_settings();
#ifdef __ANDROID__
    // A phone's GPU can't keep up with hq3x or hq4x (the menu offers only hq2x there): hq2x.
    for (int* hqx : {&settings.hqx, &settings.custom.hqx})
        if (*hqx == 3 || *hqx == 4)
            *hqx = 2;
#endif
    g_easy_bounce = settings.easy_bounce || easy_bounce;
    if (vanilla_rock_edges)
        settings.clean_rock_edges = false;
    if (vanilla_fades)
        settings.smooth_fades = false;
    if (vanilla_water)
        settings.smooth_water = false;
    if (vanilla_glass)
        settings.smooth_glass = false;
    if (show_fps)
        settings.show_fps = true;
    if (show_coordinates)
        settings.show_coordinates = true;
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
    g_smooth_water = settings.smooth_water;
    g_smooth_glass = settings.smooth_glass;
    g_smooth_swing = (settings.smooth_swing || smooth_swing) && !no_smooth_swing && g_smooth_swing_available;
    bool smooth_scroll_on = (settings.smooth_scroll || smooth_scroll) && !no_smooth_scroll;   // chosen
    set_smooth_scroll(smooth_scroll_on);
    g_vdp.smooth_scroll = [](int line, int hs_a, int hs_b, int scale, int& sub_a, int& sub_b) {
        g_scroll_smoother.line(line, hs_a, hs_b, scale, sub_a, sub_b);
    };
    set_widescreen(settings.widescreen && !no_widescreen && g_wide_available);
    g_bus.vdp = &g_vdp;
    g_bus.sound = &g_sound;
    g_vdp.extend_plane_a = extend_plane_a;
    g_vdp.extend_plane_b = extend_plane_b;
    g_vdp.edge_tiles = {0x123, 0x124, 0x125, 0x12A, 0x12B, 0x130, 0x1B7};   // the forest's rock edge tiles (Clean Rock Edges)
    // Frame interpolation notes who writes each sprite and when the list reaches the VDP.
    g_bus.on_sprite_list_write = [](uint16_t address) {
        if (g_interp_capture)
            g_interp.sprite_written(address, current_pc(), current_reg(8), g_bus.ram);
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

#ifdef COI_NO_INTERPRETER
    coi_cycles_68000(m68ki_cycles[0]);
    g_native_jit = true;
#else
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
#ifdef COI_NATIVE
    if (!native_check_cycles())
        return 1;
#endif
#endif
#ifdef COI_NATIVE
    recomp::use_opcode_table(m68ki_cycles[0]);
#endif
#ifdef COI_NATIVE
    // --jit: translate the ROM (as patched now) into machine code here, and run that (instead of
    // a translation a development build links in). The configuration and the tracer's lists: COI_RECOMP_CONFIG's
    // and COI_RECOMP_SWEEP's (sites.txt beside it), else a recomp/ folder's (recomp.txt,
    // subroutines.txt, sites.txt; on Android in the app's folder), else the ones built in
    // (config/recomp.txt, recomp_functions.txt, recomp_sites.txt).
    // A build with a translation linked in reads the same, for the ROMs that translation doesn't fit.
#ifdef COI_NATIVE_BUILT_IN
    constexpr bool kBuiltIn = true;
#else
    constexpr bool kBuiltIn = false;
#endif
    if ((g_native_jit || kBuiltIn) && (g_native_live || g_native_limit > 0)) {
        JitResult r;
        try {
            std::string config_path, sweep;
            for (const auto& dir : search_dirs("recomp"))
                if (std::filesystem::exists(dir / "recomp.txt")) {
                    config_path = (dir / "recomp.txt").string();
                    sweep = std::filesystem::exists(dir / "subroutines.txt") ? (dir / "subroutines.txt").string() : "";
                    break;
                }
            if (const char* e = std::getenv("COI_RECOMP_CONFIG")) config_path = e;
            if (const char* e = std::getenv("COI_RECOMP_SWEEP")) sweep = e;
            if (config_path.empty())
                recomp::read_config(kRecompConfig, "the built-in config/recomp.txt", g_jit_input.config);
            else
                recomp::load_config(config_path, g_jit_input.config);
            if (!sweep.empty()) {
                g_jit_input.list = recomp::load_function_list(sweep);
                recomp::load_config(sweep.substr(0, sweep.find_last_of("/\\") + 1) + "sites.txt", g_jit_input.config);
            } else if (config_path.empty()) {
                g_jit_input.list = recomp::read_function_list(kRecompFunctions);
                recomp::read_config(kRecompSites, "the built-in config/recomp_sites.txt", g_jit_input.config);
            }
            g_jit_input.trace = g_native_limit > 0 || std::getenv("COI_NATIVE_CHECK");
        } catch (const std::exception& ex) {
            r.error = ex.what();
        }
        if (!r.error.empty()) {
            std::fprintf(stderr, "jit: %s\n", r.error.c_str());
            return 1;
        }
        // COI_JIT_ARCH=arm64/x64: translate for that CPU (with COI_JIT_DUMP, to look at it);
        // code for another CPU than this one isn't run.
        const char* arch = std::getenv("COI_JIT_ARCH");
        const bool want_arm64 = arch ? std::string(arch) == "arm64" : kArm64;
        if (!g_native_jit) {
            // The translation linked in, where it fits; the app's own for any other ROM image,
            // translated when it's met (native_rom_changed).
            g_native_variants_on = g_native_live;
            native_rom_changed();
        } else {
            g_native_code = nullptr;                    // the app's own code
            g_native_rom_ok = false;
            if (g_jit_input.trace || arch || g_native_strict) {
                g_native_variants_on = g_native_live && g_native_strict;
                // At once: the harness follows translated code from the first instruction.
                r = jit_translate(g_jit_input, g_bus.rom, want_arm64);
                if (r.code && want_arm64 != kArm64) {
                    std::printf("jit: translated for another CPU (%s), not run\n", arch);
                    return 0;
                }
                if (!r.code) {
                    std::fprintf(stderr, "jit: %s\n", r.error.c_str());
                    return 1;
                }
                native_installed(r);
                g_native_code = r.code;
                recomp::jit::select(r.code);
                g_native_rom_ok = native_rom_matches(g_native_code);
                if (g_native_variants_on && g_native_rom_ok)
                    native_ahead_queue();
            } else {
                // In the game: on a thread, while the interpreter runs the boot and the Sega logo;
                // the code runs from the first frame after it's ready (native_poll).
                g_native_variants_on = g_native_live;
                g_native_pending = std::async(std::launch::async, [in = g_jit_input, rom = g_bus.rom] { return jit_translate(in, rom, kArm64); });
            }
        }
    }
#endif
    if (!g_save_rom_path.empty()) {
        if (FILE* f = std::fopen(g_save_rom_path.c_str(), "wb")) {
            std::fwrite(g_bus.rom.data(), 1, g_bus.rom.size(), f);
            std::fclose(f);
        }
    }
    if (!g_save_rom_family.empty() && !save_rom_family(g_rom_original, g_save_rom_family))
        std::fprintf(stderr, "--save-rom-family: couldn't write the images to %s\n", g_save_rom_family.c_str());
#ifdef COI_NATIVE_DEV
    native_dev_start();
#endif
#if defined(COI_NATIVE) && !defined(COI_NO_INTERPRETER)
    if (g_native_limit > 0)
        m68k_set_instr_hook_callback(native_hook);
#endif
#if defined(COI_NATIVE) && !defined(COI_NO_INTERPRETER)
    if (g_native_selftest)
        return native_selftest();
#endif
    cpu_reset();

    std::vector<uint32_t> pixels(kWideWidth * kSubpixels * Vdp::kScreenHeight);   // room for any width
    for (auto& layer : g_layers)
        layer.resize(kLayerStride * Vdp::kScreenHeight);
    for (auto& layer : g_water_layers)
        layer.resize(kLayerStride * Vdp::kScreenHeight);

    if (headless_frames > 0) {
        std::vector<int16_t> all_audio;
        g_interp_capture = interp_back >= 0;
        FILE* state_hash = state_hash_path.empty() ? nullptr : std::fopen(state_hash_path.c_str(), "w");
        // --frames counts pictures. Each runs one game frame, or with --fast-forward held over
        // it during a story scene up to kFastForward (as the windowed loop does). --press,
        // --poke and the other per-frame options count game frames, as before.
        int frame = 0;
        for (int picture = 0; picture < headless_frames; picture++) {
          bool held = false;
          for (const auto& [first, last] : fast_forwards)
              held |= picture >= first && picture <= last;
          const int steps = held && is_cutscene_state(g_bus.ram) ? kFastForward : 1;
          for (int step = 0; step < steps; step++, frame++) {
            if (step > 0 && !is_cutscene_state(g_bus.ram))
                break;                              // the scene ended within the picture
#ifdef COI_NATIVE
            g_native_frame = frame;
#endif
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
#if defined(COI_NATIVE) && !defined(COI_NO_INTERPRETER)
            for (int f : g_probe_frames)
                if (frame == f)
                    probe_capture(frame);
#endif
            run_frame(pixels.data());
            if (state_hash)                         // game frame, picture, machine_hash
                std::fprintf(state_hash, "%d %d %016llx\n", frame, picture, (unsigned long long)machine_hash());
#ifndef NDEBUG
            if (!sprite_capture_dir.empty())
                capture_sprites(sprite_capture_dir, frame);
#endif
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
                    std::printf("frame %5d (picture %5d): GameState $%02X\n", frame, picture, state);
                last = state;
            }
            if (!wav_path.empty() && steps == 1)    // muted while fast-forwarding
                all_audio.insert(all_audio.end(), g_audio.begin(), g_audio.end());
          }
        }
        if (!wav_path.empty())
            write_wav(wav_path.c_str(), all_audio, g_sound.sample_rate());
        if (state_hash)
            std::fclose(state_hash);
#ifdef COI_NATIVE_DEV
        native_dev_finish();
#endif
#ifdef COI_NATIVE
        std::printf("68000: %.0f ms for %d frames (%.3f ms a frame)\n", g_cpu_time / 1e6, headless_frames,
                    g_cpu_time / 1e6 / headless_frames);
        if (g_native_live && !g_native_rom_ok)
            std::printf("native: the ROM as patched now isn't the one translated, so the interpreter ran it\n");
        if (g_native_live) {
            std::printf("68000: %.1f%% of %lld cycles in translated code\n", 100.0 * g_native_cycles / g_cpu_cycles,
                        (long long)g_cpu_cycles);
            std::printf("native: %lld slices handed to Musashi\n", (long long)g_native_fallbacks);
            std::vector<std::pair<int64_t, uint32_t>> top;
            for (const auto& [a, n] : g_interpreted)
                top.push_back({n, a});
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 25; i++)
                std::printf("  interpreted %lld times at $%06X\n", (long long)top[i].first, top[i].second);
            strict_report();
        }
        native_sync();                      // (Musashi's registers from here on: the report, the replays)
#ifndef COI_NO_INTERPRETER
        if (g_native_limit > 0)             // (it replays saved calls, so nothing after it reports this run)
            return native_report() ? 1 : 0;
        if (!g_probe_states.empty())
            return native_probe() ? 1 : 0;
#endif
#endif
        std::printf("YM2612 timer expirations: A=%u B=%u\n", g_sound.timer_expirations[0],
                    g_sound.timer_expirations[1]);
        std::printf("Ran %d pictures, %d game frames; PC=%06X GameState=$%02X VIntMode=$%02X\n", headless_frames, frame,
                    cpu_pc_for_report(), g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301],
                    g_bus.ram[0xF302] << 8 | g_bus.ram[0xF303]);
        save_state(ram_path, vram_path, cram_path);
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
                    g_vdp.reg[0], g_vdp.reg[1], g_irq_pending, cpu_sr_for_report(),
                    g_bus.ram[0xFF08] << 8 | g_bus.ram[0xFF09], g_bus.ram[0xFF0A] << 8 | g_bus.ram[0xFF0B]);
        if (interp_back >= 0 && g_interp.ready())   // --interp-back: the last frame, drawn earlier
            render_interpolated(pixels.data(), interp_back);
        if (!dump_path.empty() && !save_ppm(dump_path.c_str(), pixels.data())) {
            std::fprintf(stderr, "Could not write %s\n", dump_path.c_str());
            return 1;
        }
        return 0;
    }

#ifdef __ANDROID__
    // Landscape only, either way up, and always fullscreen (the status and navigation bars hidden).
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // Back (the gesture or button) opens and closes the menu instead of closing the app.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    settings.fullscreen = true;
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    // OpenGL 3.3 core: the display draws the frame (through the CRT shader when it is on)
    // and the menu draws over it at the window's own resolution.
#ifdef __ANDROID__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);       // OpenGL ES 3.0
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
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
#ifndef __ANDROID__
    // The window's icon: the 32 x 32 pixel-art apple, with whole-number enlargements for
    // the sizes a taskbar or window switcher may want, so its pixels stay sharp.
    if (SDL_Surface* icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(kIconPng, sizeof(kIconPng)), true)) {
        for (int scale : {2, 4, 8})
            if (SDL_Surface* big = SDL_ScaleSurface(icon, icon->w * scale, icon->h * scale, SDL_SCALEMODE_NEAREST)) {
                SDL_AddSurfaceAlternateImage(icon, big);
                SDL_DestroySurface(big);
            }
        SDL_SetWindowIcon(window, icon);
        SDL_DestroySurface(icon);
    }
#endif
#ifdef __ANDROID__
    android_pacing_init(window);
#endif
#ifdef COI_TIMING_LOG
    g_gpu_timer.init();
#endif
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
    if (clean_edge_width >= 0)
        settings.clean_edge_line_width = std::clamp(clean_edge_width, 0.45f, 1.14f);
    // hqx needs whole pixels, so with smooth scrolling the frame is drawn as layers instead.
    auto hqx_on = [&] {
        return !settings.crt.enabled && (settings.hqx == kCleanEdge ? display.clean_edge_available()
                                         : settings.hqx >= 2 && settings.hqx <= 4 && display.hqx_available());
    };
    // Smooth scrolling as drawn: the setting, except with cleanEdge on Android, where drawing
    // the layers through it separately is more than a phone's GPU can do 60 times a second
    // (the setting is kept for the other upscalers).
    auto scrolling_smoothly = [&] {
#ifdef __ANDROID__
        if (hqx_on() && settings.hqx == kCleanEdge)
            return false;
#endif
        return smooth_scroll_on;
    };
    menu_init(window, gl);
    touch_init(window);

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
#ifdef __ANDROID__
    bool importing_pack = false;                    // an MD+ pack import is under way
#endif
    // MD+ on or off: a different cartridge (the pack's patched ROM or the original), from a reset.
    auto switch_mdplus = [&](bool on) {
        g_mdplus_on = on && g_mdplus.loaded();
        settings.mdplus = g_mdplus_on;
        g_rom_original = g_mdplus_on ? g_rom_mdplus : g_rom_cartridge;
        build_rom();
        power_on();
        if (music)
            SDL_ClearAudioStream(music);
        if (audio)
            SDL_ClearAudioStream(audio);
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
            // file:///C:/... on Windows, file:///home/... elsewhere (the path starts with its own /).
            if (const std::string dir = settings_folder();
                !dir.empty() && !SDL_OpenURL(((dir[0] == '/' ? "file://" : "file:///") + dir).c_str()))
                std::fprintf(stderr, "Could not open %s: %s\n", dir.c_str(), SDL_GetError());
            break;
        case MenuAction::ToggleMdPlus:
            switch_mdplus(!g_mdplus_on);
            break;
        case MenuAction::ImportMdPlus:
#ifdef __ANDROID__
            android_import_pack();
            importing_pack = true;
            if (menu_open())
                menu_toggle();
#endif
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
    int pictures_shown = 0;                         // pictures since start (--fast-forward)
    bool fast_forwarding = false;                   // this picture runs kFastForward game frames

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
    bool was_counted = false;                       // game frames followed the pictures shown
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
        double refresh = refresh_override > 0 ? refresh_override : mode ? mode->refresh_rate : 0.0;
#ifdef __ANDROID__
        // Swappy (android_pacing.cpp) shows every picture for the same number of the panel's
        // refreshes, about a 60th of a second each: the game is locked to that rate, whatever the
        // phone reports (Android gives the panel's highest rate, while phones switch by themselves).
        if (vsync_on && android_pacing_active() && refresh_override <= 0)
            refresh = android_pacing_rate();
#endif
        // On a display at a whole multiple of 60 Hz (60, 120, 180, 240) each game frame is
        // shown for exactly that many refreshes, counted by refresh rather than by the clock,
        // so every picture lasts the same time (the game runs a fraction faster or slower than
        // the console to match; the audio is resampled the same way).
        const int multiple = refresh > 0 ? (int)std::lround(refresh / native_hz) : 0;
        // Only while the swaps really wait for the refresh: shown faster than it, V-Sync isn't
        // working (some drivers ignore it), and counting refreshes would run the game too fast.
        // Nor while pictures can't keep up with it (drawing one takes longer than a refresh,
        // as the CRT shader can at 4K and 120 Hz): every swap then waits two refreshes, and
        // counting them would run the game at half speed. Frames then come due by the clock.
        // Neither applies under Swappy on Android, which always waits for the refresh: a phone
        // keeps up with ease, but a slow second (starting up, or Android putting the game on a
        // slower core for a while) would drop the lock, and frames by the clock then come two to
        // a picture, which keeps the pictures at 30 a second and the lock from coming back (and
        // the quick pictures that follow a late one would drop it the other way). Locked, a late
        // picture just shows its frame late.
        bool pictures_off = fps > refresh * 1.03 || (fps > 0 && fps < refresh * 0.95);
#ifdef __ANDROID__
        if (vsync_on && android_pacing_active())
            pictures_off = false;
#endif
        const bool locked = vsync_on && multiple >= 1 && std::fabs(refresh - multiple * native_hz) < multiple
                            && !pictures_off;
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
        // A change of pacing starts the counts afresh (in particular, frames coming due by the
        // clock again after being counted mustn't catch up on the time spent counting).
        if (per_frame != refreshes_per_frame || fixed != fixed_rate || counted != was_counted) {
            refreshes_per_frame = per_frame;
            fixed_rate = fixed;
            was_counted = counted;
#ifdef COI_TIMING_LOG
            g_lock_changes++;
#endif
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
        // The sound plays faster or slower with the game when it is locked to the display, plus
        // a small correction (at most 2%, too little to hear) that holds the sound waiting to be
        // played near 50 ms: a display whose real rate differs a little from the one it reports
        // (a phone's "60 Hz" panel may run at 60.5), or a sound device's own clock, then never
        // makes it pile up to the limit (and be dropped, a skip) or run out (a gap). The correction
        // has a part that answers how far the queue is from 50 ms now, and a part that builds up
        // slowly while it stays off, so a steady difference (a Galaxy S22+'s "60 Hz" panel runs at
        // 59.77) is taken up in full and the queue settles back at 50 ms, not short of it.
        float ratio = locked && !fixed ? (float)(refresh / (per_frame * native_hz)) : 1.0f;
        if (audio) {
            static double drift = 0;                // the built-up part
            const double queued_ms = SDL_GetAudioStreamQueued(audio) * 250.0 / g_sound.sample_rate();
            if (!fast_forwarding)                   // the queue runs dry while muted: not a drift
                drift = std::clamp(drift + (queued_ms - 50.0) * 1e-5 / 60.0, -0.015, 0.015);
            ratio *= (float)(1.0 + std::clamp((queued_ms - 50.0) / 50.0 * 0.01 + drift, -0.02, 0.02));
        }
        if (audio && std::fabs(ratio - audio_ratio) > 0.0005f) {
            SDL_SetAudioStreamFrequencyRatio(audio, ratio);
            audio_ratio = ratio;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            const bool finger = event.type >= SDL_EVENT_FINGER_DOWN && event.type <= SDL_EVENT_FINGER_CANCELED;
            if (!finger || !menu_open())            // the menu takes touches as clicks while open
                touch_event(event);
            if (menu_event(event))
                continue;
            if (event.type == SDL_EVENT_QUIT)
                running = false;
#ifdef __ANDROID__
            // While in the background the app waits in SDL_PollEvent with its sound paused;
            // coming back, its window has a new surface.
            else if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND)
                android_pacing_set_window(window);
#endif
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
                if (key == SDLK_F1 || key == SDLK_AC_BACK) {    // Back on Android
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
#ifndef NDEBUG
                } else if (key == SDLK_F9) {
                    if (snapshot_dir.empty()) {
                        const char* exe_dir = SDL_GetBasePath();
                        snapshot_dir = std::string(exe_dir ? exe_dir : "") + "snapshots";
                    }
                    std::error_code error;
                    std::filesystem::create_directories(snapshot_dir, error);
                    static int snapshots = 0;
                    const std::string base = snapshot_dir + "/snap" + std::to_string(++snapshots);
                    save_state(base + ".ram", base + ".vram", base + ".cram");
                    save_ppm((base + ".ppm").c_str(), pixels.data());
                    std::printf("Saved %s (frame %d)\n", base.c_str(), frames_run);
#endif
                }
            }
        }
        if (touch_menu_tapped()) {
            menu_toggle();
            update_cursor();
        }
        if (menu_open())
            touch_release();
#ifdef __ANDROID__
        // An MD+ pack being imported: its progress over the game, then the pack switched on.
        std::string busy;
        float busy_progress = -1;
        if (importing_pack) {
            const PackImportStatus status = android_import_status();
            switch (status.state) {
            case PackImport::Choosing:
                busy = "Choose the MD+ pack's folder";
                break;
            case PackImport::Copying:
                busy = "Copying the MD+ pack";
                busy_progress = (float)status.progress;
                break;
            case PackImport::Done: {
                importing_pack = false;
                android_import_clear();
                const std::string why = find_mdplus_pack();
                switch_mdplus(why.empty());
                if (why.empty())
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Castle of Illusion",
                                             ("MD+ pack imported: " + g_mdplus.name()
                                              + "\n\nIts soundtrack is on, and the game has restarted. "
                                                "Switch it in Enhancements > Audio.").c_str(), window);
                else
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Castle of Illusion",
                                             ("The pack was copied, but it can't be used: " + why + ".").c_str(), window);
                break;
            }
            case PackImport::Failed:
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Castle of Illusion", status.message.c_str(), window);
                [[fallthrough]];
            default:                                // cancelled
                importing_pack = false;
                android_import_clear();
                break;
            }
        }
#else
        const std::string busy;
        const float busy_progress = -1;
#endif
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
                        &settings.scanlines, &settings.hqx, &settings.clean_edge_line_width, &settings.clean_rock_edges, &settings.smooth_fades, &settings.smooth_water, &settings.smooth_glass,
                        &settings.remove_sprite_limit, &settings.vsync, &settings.show_fps, fps, game_fps,
                        &settings.cheats, &settings.interpolate, refresh, &settings.frame_rate,
                        g_mdplus_on, g_mdplus.loaded() ? g_mdplus.name().c_str() : nullptr, settings.quick,
                        busy.empty() ? nullptr : busy.c_str(), busy_progress, &settings.show_coordinates,
                        g_bus.ram[0xFF42] << 8 | g_bus.ram[0xFF43], g_bus.ram[0xF300] << 8 | g_bus.ram[0xF301],
                        (int16_t)(g_bus.ram[0xDC10] << 8 | g_bus.ram[0xDC11]), (int16_t)(g_bus.ram[0xDC14] << 8 | g_bus.ram[0xDC15]),
                        (int16_t)(g_bus.ram[0xFA84] << 8 | g_bus.ram[0xFA85]), (int16_t)(g_bus.ram[0xFA80] << 8 | g_bus.ram[0xFA81])}),
            running);
        if ((Quick)settings.quick != Quick::Custom && look_of(settings) != quick_look((Quick)settings.quick)) {
            settings.quick = (int)Quick::Custom;    // one of its settings was changed by hand
            save_settings(settings);
        }
        g_clean_rock_edges = settings.clean_rock_edges;
        g_smooth_fades = settings.smooth_fades;
        g_smooth_water = settings.smooth_water;
        g_smooth_glass = settings.smooth_glass;
        g_vdp.sprite_limit = !settings.remove_sprite_limit;
        if ((scrolling_smoothly() && !hqx_on()) != (g_scale > 1))
            set_smooth_scroll(scrolling_smoothly() && !hqx_on());
        if ((scrolling_smoothly() && hqx_on()) != g_layered) {
            g_layered = !g_layered;
            g_scroll_smoother.reset();
        }

        const uint8_t buttons = (keyboard_free ? keyboard_buttons() : 0) | pad | touch_buttons();
        // Fast forward: held, with the menu closed, while a story scene plays (is_cutscene_state).
        // The touch controls show their >> button only then.
        const bool in_scene = is_cutscene_state(g_bus.ram);
        touch_show_fast_forward(in_scene);
        bool fast_forward_held = (keyboard_free && keyboard_fast_forward()) || (!pad_held_over && gamepad_fast_forward())
                                 || touch_fast_forward();
        for (const auto& [first, last] : fast_forwards)  // --fast-forward works in a window too
            fast_forward_held |= pictures_shown >= first && pictures_shown <= last;
        const bool was_fast_forwarding = fast_forwarding;
        fast_forwarding = fast_forward_held && !menu_open() && in_scene;
        if (music && fast_forwarding != was_fast_forwarding)
            SDL_SetAudioStreamGain(music, fast_forwarding ? 0.0f : 1.0f);
#ifdef COI_TIMING_LOG
        const Uint64 timing_start = SDL_GetTicksNS();
        const Uint64 video_start = g_time_video, cpu_start = g_time_cpu, sound_start = g_time_sound;
#ifdef COI_NATIVE
        const bool translating_start = g_native_pending.valid();
        const size_t variants_start = g_native_variants.size();
#endif
#endif
        for (int i = 0; i < due && running; i++) {
          const int steps = fast_forwarding && is_cutscene_state(g_bus.ram) ? kFastForward : 1;
          for (int step = 0; step < steps && running; step++) {
            if (step > 0 && !is_cutscene_state(g_bus.ram))
                break;                              // the scene ended within the picture
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
            if (audio && !fast_forwarding) {        // muted while fast-forwarding
#ifdef COI_TIMING_LOG
                {
                    const int queued = SDL_GetAudioStreamQueued(audio);
                    g_audio_min = std::min(g_audio_min, queued);
                    g_audio_max = std::max(g_audio_max, queued);
                    if (queued > max_queued)
                        g_audio_clears++;
                }
#endif
                if (SDL_GetAudioStreamQueued(audio) > max_queued)
                    SDL_ClearAudioStream(audio);
                SDL_PutAudioStreamData(audio, g_audio.data(), (int)(g_audio.size() * sizeof(int16_t)));
            }
          }
        }
        pictures_shown++;
        g_mdplus.feed(music);
#ifdef COI_TIMING_LOG
        const Uint64 timing_emulated = SDL_GetTicksNS();
        g_gpu_timer.begin();
#endif
        if (interpolating && g_interp.ready())
            render_interpolated(pixels.data(), back);
        display.clean_edge_line_width = settings.clean_edge_line_width;
        if (g_layered) {
            LayeredFrame frame;
            for (int i = 0; i < LayeredFrame::kLayers; i++)
                frame.layers[i] = g_layers[i].data();
            for (int i = 0; i < 2; i++)
                frame.water[i] = g_vdp.water_tiles.empty() && g_vdp.tile_rgba_index.empty()
                                 ? nullptr : g_water_layers[i].data();   // (tile pictures draw there too)
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
#ifdef COI_TIMING_LOG
        g_gpu_timer.end();
        const Uint64 timing_drawn = SDL_GetTicksNS();
#endif
#ifdef __ANDROID__
        if (!android_pacing_swap())
#endif
        SDL_GL_SwapWindow(window);
#ifdef COI_TIMING_LOG
        // Performance check: once every 2 seconds, the frame rates and where a frame's time
        // goes (emulation per game frame, drawing and the swap per picture) to the log.
        {
            static Uint64 since = 0, emulate = 0, draw = 0, swap = 0, last_swap = 0, longest_gap = 0;
            static int pictures = 0, games = 0, late = 0;
            const Uint64 swapped = SDL_GetTicksNS();
            // Pictures that missed their refresh: more than one and a half refreshes since the last.
            if (last_swap) {
                const Uint64 gap = swapped - last_swap;
                longest_gap = std::max(longest_gap, gap);
                if (gap > 25000000)
                    late++;
                // A hitch (a picture more than 45 ms after the last, about three refreshes):
                // where that picture's time went, and what the translator was doing.
                if (gap > 45000000) {
                    const Uint64 work = swapped - timing_start;
                    const char* native = "";
#ifdef COI_NATIVE
                    native = g_native_variants.size() != variants_start ? ", a translation installed"
                             : translating_start || g_native_pending.valid() ? ", translating"
                             : g_native_live && !g_native_rom_ok ? ", ROM not translated (interpreted)" : "";
#endif
                    SDL_Log("hitch: %.1f ms since the last picture; %d game frame(s): emulation %.2f ms (video %.2f, "
                            "68000 %.2f, Z80+sound %.2f), drawing %.2f, swap %.2f, outside those %.2f ms; "
                            "frame %d, GameState $%02X, stage %d%s",
                            gap / 1e6, due, (timing_emulated - timing_start) / 1e6, (g_time_video - video_start) / 1e6,
                            (g_time_cpu - cpu_start) / 1e6, (g_time_sound - sound_start) / 1e6,
                            (timing_drawn - timing_emulated) / 1e6, (swapped - timing_drawn) / 1e6,
                            (gap - std::min(gap, work)) / 1e6, frames_run, g_bus.ram[0xF301], current_stage(), native);
                }
            }
            last_swap = swapped;
            emulate += timing_emulated - timing_start;
            draw += timing_drawn - timing_emulated;
            swap += swapped - timing_drawn;
            pictures++;
            games += due;
            if (since == 0)
                since = swapped;
            if (swapped - since >= 2000000000) {
                const double seconds = (swapped - since) / 1e9;
                const double per_game = 1e6 * (games ? games : 1);
                SDL_Log("timing: %.1f pictures/s, %.1f game frames/s; emulation %.2f ms/game frame "
                        "(video %.2f, 68000 %.2f, Z80+sound %.2f), drawing %.2f ms, swap %.2f ms/picture; "
                        "refresh %.2f Hz, locked %d, lock changes %d; audio queued %.0f-%.0f ms, cleared %d; "
                        "GPU %.2f ms/picture (longest %.2f); late pictures %d, longest gap %.1f ms",
                        pictures / seconds, games / seconds, emulate / per_game,
                        g_time_video / per_game, g_time_cpu / per_game, g_time_sound / per_game,
                        draw / 1e6 / pictures, swap / 1e6 / pictures, refresh, (int)was_counted, g_lock_changes,
                        g_audio_min * 250.0 / g_sound.sample_rate(), g_audio_max * 250.0 / g_sound.sample_rate(),
                        g_audio_clears, g_gpu_timer.count ? g_gpu_timer.total / 1e6 / g_gpu_timer.count : 0.0,
                        g_gpu_timer.longest / 1e6, late, longest_gap / 1e6);
                g_gpu_timer.total = g_gpu_timer.longest = 0;
                g_gpu_timer.count = 0;
                late = 0;
                longest_gap = 0;
                g_lock_changes = 0;
                g_audio_min = 1 << 30, g_audio_max = 0, g_audio_clears = 0;
                g_time_video = g_time_cpu = g_time_sound = 0;
                since = swapped;
                emulate = draw = swap = 0;
                pictures = games = 0;
            }
        }
#endif

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
