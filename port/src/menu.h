// The F1 menu: a Dear ImGui menu bar over the game, in the style of Ship of Harkinian's.
#pragma once

#include <SDL3/SDL.h>

#include "cheats.h"
#include "display.h"

struct MenuState {
    bool widescreen;            // current setting
    bool widescreen_available;  // false when the patches did not apply to this ROM
    bool fullscreen;
    int stage;                  // stage being played (1-28), or 0 outside a game
    bool easy_bounce;
    bool smooth_swing;
    bool smooth_swing_available;    // false when its patches did not apply to this ROM
    bool smooth_scroll;
    CrtSettings* crt;           // edited in place by the CRT settings window
    int* scanlines;             // simple scanline strength in percent, edited in place
    int* hqx;                   // hqx scale (1 = off), edited in place
    bool* clean_rock_edges;     // edited in place
    bool* smooth_fades;         // edited in place
    bool* remove_sprite_limit;  // edited in place
    bool* vsync;                // edited in place
    bool* show_fps;             // edited in place
    float fps, game_fps;        // frames shown and game frames run per second, for the counter
    Cheats* cheats;             // edited in place
    bool* interpolate;          // edited in place
    double refresh;             // the display's refresh rate in Hz (0 = unknown)
    int* frame_rate;            // 0 (the display's), 60, 120 or 240; edited in place
    bool mdplus;                // an MD+ soundtrack pack is playing
    const char* mdplus_pack;    // the pack found in mdplus/ (nullptr: none)
    int quick;                  // the Quick Settings in use (Quick in settings.h)
};

enum class MenuAction { None, Reset, Exit, ToggleWidescreen, ToggleFullscreen, ToggleEasyBounce, ToggleSmoothSwing, ToggleSmoothScroll, ToggleMdPlus, OpenSettingsFolder, GoToStage, QuickSettings };

struct MenuChoice {
    MenuAction action = MenuAction::None;
    int stage = 0;              // for GoToStage: 1-28
    int quick = 0;              // for QuickSettings: the one chosen (Quick in settings.h)
    bool save_settings = false; // the CRT settings changed and should be saved
    bool cheats_changed = false;    // a cheat was switched: rebuild the ROM (and save)
};

void menu_init(SDL_Window* window, SDL_GLContext gl_context);
void menu_shutdown();

// Passes an event to the menu. Returns true when the menu used it and the game should not.
bool menu_event(const SDL_Event& event);

bool menu_open();
// Shows or hides the menu. Opened from a controller, it takes the controller's focus at
// once (A selects, B goes back and, at the top level, closes the menu).
void menu_toggle(bool from_gamepad = false);

// True while the menu has keyboard focus (its keys must not reach the game).
bool menu_wants_keyboard();

// Builds this frame's menu and returns what the player chose; call menu_render after
// drawing the game.
MenuChoice menu_frame(const MenuState& state);
void menu_render();
