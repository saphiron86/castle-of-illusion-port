// Playing with an Android TV remote (the NVIDIA Shield's) held sideways, its top to the left,
// while no controller is connected. See tv_remote.cpp.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

// Before SDL_Init: on Android TV, remotes send key presses instead of showing up as
// controllers, so "no controller connected" means just that.
void tv_remote_init();

// Running on Android TV (where the remote's Back opens the menu).
bool tv_remote_on_tv();

// The sideways scheme is in use: on Android TV, with no controller connected.
bool tv_remote_active();

// While the scheme is in use, turns the remote's D-pad keys a quarter turn in the event, so
// the menu moves the way the remote is held.
void tv_remote_event(SDL_Event& event);

// The game buttons held on the remote (BTN_* in bus.h), the D-pad turned.
uint8_t tv_remote_buttons();

// How to play with the remote, for the first-time notice.
const char* tv_remote_help();
