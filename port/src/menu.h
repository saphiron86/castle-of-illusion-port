// The F1 menu: a Dear ImGui menu bar over the game, in the style of Ship of Harkinian's.
#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "cheats.h"
#include "display.h"

// A save slot's picture (Game > Save Slot's tooltips): an OpenGL texture, 0 for none.
struct SlotPicture {
    unsigned texture = 0;
    int width = 0, height = 0;
};

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
    float* clean_edge_line_width;   // edited in place
    bool* clean_rock_edges;     // edited in place
    bool* smooth_fades;         // edited in place
    bool* smooth_water;         // edited in place
    bool* smooth_glass;         // edited in place
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
    const char* busy;           // a task under way, shown over the game (nullptr: none)
    float busy_progress;        // its progress, 0-1 (below 0: none to show)
    bool* show_coordinates;     // edited in place
    int stage_index;            // the game's stage index ($FFFF42), for the coordinates
    int game_state;             // GameState ($FFF300)
    int mickey_x, mickey_y;     // Mickey's position in the level ($FFDC10, $FFDC14)
    int camera_x, camera_y;     // CameraA ($FFFA84, $FFFA80)
    bool check_for_updates;     // the setting
    const char* update_tag;     // a newer release's tag (nullptr: none found)
    bool* coop_camera;          // Co-op Dynamic Camera, edited in place
    bool gpu_renderer;          // GPU Renderer: the setting
    bool gpu_available;         // false once the GPU renderer could not start (the CPU draws)
    // Texture Packs (Graphics): the setting, the packs found (folder names) and the one chosen
    // (-1: none), the one loading (nullptr: none), and why packs don't show now (nullptr:
    // they do).
    bool texture_packs;
    const std::vector<std::string>* texture_pack_names;
    int texture_pack;
    const char* texture_pack_loading;
    float texture_pack_progress;    // 0-1, while one loads (shown at the top, menu open or not)
    const char* texture_pack_blocked;
    bool* smooth_animation;     // Smooth Animation (Enhancements > Graphics > Enhancements), edited in place
    bool busy_corner;           // `busy` in the bottom left corner instead of the centre (clear of the Sega
                                // logo), its bar as wide as the text and with no percentage (an estimate)
    bool* clear_fm;             // Enhancements > Audio, edited in place
    bool* clean_psg;            // edited in place
    bool* low_pass;             // edited in place
    bool* stage4_storm;         // Weather - Stage 4 (Enhancements > Graphics > Enhancements), edited in place
    // Save states (Game > Save, Load, Save Slot): the slot in use (1-5), each slot's line
    // (5), and why Save and Load are off now (nullptr: they aren't).
    int save_slot;
    const std::string* slot_labels;
    const SlotPicture* slot_pictures;   // (5) the game as each slot was saved
    const char* states_blocked;
};

enum class MenuAction { None, Reset, Exit, ToggleWidescreen, ToggleFullscreen, ToggleEasyBounce, ToggleSmoothSwing, ToggleSmoothScroll, ToggleMdPlus, ImportMdPlus, OpenSettingsFolder, GoToStage, QuickSettings, OpenReleases, ToggleUpdateCheck, ToggleGpuRenderer, ToggleTexturePacks, ChooseTexturePack, AllEnhancements, SaveState, LoadState, ChooseSaveSlot };

struct MenuChoice {
    MenuAction action = MenuAction::None;
    int stage = 0;              // for GoToStage: 1-28
    int quick = 0;              // for QuickSettings: the one chosen (Quick in settings.h)
    int texture_pack = 0;       // for ChooseTexturePack: its place in texture_pack_names
    int save_slot = 1;          // for ChooseSaveSlot: 1-5
    bool all_on = false;        // for AllEnhancements: All On (else All Off)
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

// A TV remote drives the menu (tv_remote.h): its keys keep the bar focused, as a controller's
// buttons do, since there is no mouse to click it with.
void menu_remote_nav(bool on);

// True while the menu has keyboard focus (its keys must not reach the game).
bool menu_wants_keyboard();

// Builds this frame's menu and returns what the player chose; call menu_render after
// drawing the game.
MenuChoice menu_frame(const MenuState& state);
void menu_render();

// Shows a hint in the top left corner for `seconds` (hidden while the menu is open), and
// changes its text while it shows.
void menu_hint(const char* text, double seconds);
void menu_hint_text(const char* text);

// Shows a line in the top right corner this frame (call every frame it should show), at
// `alpha` opacity (0-1) for fading.
void menu_status(const char* text, float alpha);
// The same in the top left corner.
void menu_status_left(const char* text, float alpha);

// Shows a short message at the top of the screen for `seconds` (several stack, newest last).
void menu_notice(const char* text, double seconds);
