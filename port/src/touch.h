// On-screen touch controls (Android): a D-pad, A, B and C, Start and a menu button, drawn over
// the game. See touch.cpp.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

// The window the controls are drawn over (its safe area keeps them off a camera cutout).
void touch_init(SDL_Window* window);

// The controls are showing (no controller in use).
bool touch_shown();

// Passes an event to the touch controls: fingers press them, and a controller hides them
// until the screen is touched again.
void touch_event(const SDL_Event& event);

// The game buttons the fingers hold (BTN_* in bus.h).
uint8_t touch_buttons();

// True once after the menu button is tapped.
bool touch_menu_tapped();

// Lets go of every finger (the menu opened over them).
void touch_release();

// Draws the controls, if they are showing, into the current ImGui frame (behind the menu).
void touch_draw();
