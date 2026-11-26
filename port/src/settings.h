// Menu settings remembered between runs, in settings.ini in the user's app data folder
// (on Windows %APPDATA%\CastleOfIllusion\Port\settings.ini; on Linux
// ~/.local/share/CastleOfIllusion/Port/settings.ini).
#pragma once

#include <string>

#include "cheats.h"
#include "display.h"

// The settings Quick Settings switches together: widescreen, the graphics enhancements and
// the sound enhancements (Enhancements > Audio, but MD+).
struct Look {
    bool widescreen = true;
    bool smooth_swing = true;
    bool smooth_scroll = true;
    bool clean_rock_edges = true;
    bool smooth_fades = true;
    bool smooth_water = true;
    bool smooth_glass = true;
    bool remove_sprite_limit = true;
    bool smooth_animation = true;
    int hqx = 1;
    int scanlines = 0;
    bool crt = false;
    bool clear_fm = true;
    bool clean_psg = true;
    bool low_pass = false;
    bool stage4_storm = true;
    bool operator==(const Look& o) const;
    bool operator!=(const Look& o) const { return !(*this == o); }
};

// Quick Settings: Enhanced is the default look, Vanilla the original game (4:3, no graphics
// enhancements), Custom the player's own choices.
enum class Quick { Enhanced, Vanilla, Custom };
Look quick_look(Quick quick);

struct Settings {
    bool widescreen = true;
#if defined(__linux__) && !defined(__ANDROID__)
    bool fullscreen = true;     // Steam Deck and Linux desktops start fullscreen
#else
    bool fullscreen = false;    // Windows starts in a window (Android is always fullscreen)
#endif
    bool easy_bounce = false;
    bool smooth_swing = true;
    bool smooth_scroll = true;
    int scanlines = 0;          // percent: 0 (off), 25, 50, 75 or 100
    int hqx = 1;                // hqx scale: 2, 3 or 4; 1 = off; kCleanEdge = cleanEdge
    float clean_edge_line_width = 1.0f;     // cleanEdge's line width, 0.45-1.14
    bool clean_rock_edges = true;
    bool smooth_fades = true;
    bool smooth_water = true;
    bool smooth_glass = true;
    bool remove_sprite_limit = true;
    bool smooth_animation = true;   // Smooth Animation (Enhancements > Graphics > Enhancements; smooth_animation.h)
    bool stage4_storm = true;       // Weather - Stage 4 (Enhancements > Graphics > Enhancements; storm.h)
    bool vsync = true;
    bool show_fps = false;
    bool show_coordinates = false;  // Debug > Show Coordinates
    bool check_for_updates = true;  // Game > Check for Updates (GitHub's releases, at start-up)
    bool interpolate = false;
    int frame_rate = 0;             // pictures a second: 0 = the display's refresh, 60, 120 or 240
    bool coop_camera = true;        // Co-op Dynamic Camera (Enhancements > Graphics): zoom in close, out apart
    bool gpu_renderer = true;       // GPU Renderer (Debug menu): the picture drawn on the GPU (gpu_vdp.h)
    Cheats cheats;
    bool mdplus = true;             // play an MD+ soundtrack pack when one is in mdplus/
    bool clear_fm = true;           // Enhancements > Audio > Clear FM: no ladder effect (sound.h)
    bool clean_psg = true;          // Enhancements > Audio > Clean PSG: band-limited PSG (psg.h)
    bool low_pass = false;          // Enhancements > Audio > Model 1 Low-Pass
    bool texture_packs = true;      // Graphics > Texture Packs: draw with a pack from texture_packs/
    std::string texture_pack;       // the pack chosen (its folder's name; empty: the first found)
    int save_slot = 1;              // Game > Save Slot: the save state slot Save and Load use (1-5)
    bool tv_remote_help_shown = false;  // the TV remote's first-time notice has been shown (tv_remote.h)
    CrtSettings crt;
    int quick = 0;                  // Quick Settings in use (Quick)
    Look custom;                    // Custom's settings, kept while Enhanced or Vanilla is in use
};

// The Look part of the settings, and setting it.
Look look_of(const Settings& settings);
void set_look(Settings& settings, const Look& look);

// Reads the settings file. A missing file or unknown keys leave the defaults in place.
Settings load_settings();

// Writes all settings; prints a warning if the file can't be written.
void save_settings(const Settings& settings);

// The folder that holds the settings file (empty if there is none).
std::string settings_folder();
