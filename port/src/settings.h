// Menu settings remembered between runs, in settings.ini in the user's app data folder
// (on Windows %APPDATA%\CastleOfIllusion\Port\settings.ini).
#pragma once

#include <string>

#include "cheats.h"
#include "display.h"

// The settings Quick Settings switches together: widescreen and the graphics enhancements.
struct Look {
    bool widescreen = true;
    bool smooth_swing = true;
    bool smooth_scroll = true;
    bool clean_rock_edges = true;
    bool smooth_fades = true;
    bool remove_sprite_limit = true;
    int hqx = 1;
    int scanlines = 0;
    bool crt = false;
    bool operator==(const Look& o) const;
    bool operator!=(const Look& o) const { return !(*this == o); }
};

// Quick Settings: Enhanced is the default look, Vanilla the original game (4:3, no graphics
// enhancements), Custom the player's own choices.
enum class Quick { Enhanced, Vanilla, Custom };
Look quick_look(Quick quick);

struct Settings {
    bool widescreen = true;
    bool fullscreen = false;
    bool easy_bounce = false;
    bool smooth_swing = true;
    bool smooth_scroll = true;
    int scanlines = 0;          // percent: 0 (off), 25, 50, 75 or 100
    int hqx = 1;                // hqx scale: 2, 3 or 4; 1 = off
    bool clean_rock_edges = true;
    bool smooth_fades = true;
    bool remove_sprite_limit = true;
    bool vsync = true;
    bool show_fps = false;
    bool interpolate = false;
    int frame_rate = 0;             // pictures a second: 0 = the display's refresh, 60, 120 or 240
    Cheats cheats;
    bool mdplus = true;             // play an MD+ soundtrack pack when one is in mdplus/
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
