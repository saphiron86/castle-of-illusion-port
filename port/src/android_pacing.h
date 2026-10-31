// Android frame pacing with Swappy (the Android Games SDK's frame pacing library). See
// android_pacing.cpp.
#pragma once

#include <SDL3/SDL.h>

// Sets up Swappy for the window's OpenGL ES surface, pacing pictures at 60 a second. Call
// with the context current. Without it (it failed to start) the port swaps as usual.
void android_pacing_init(SDL_Window* window);

// Gives Swappy the window's surface again: Android makes a new one when the app comes back
// from the background.
void android_pacing_set_window(SDL_Window* window);

// Swappy is pacing the pictures.
bool android_pacing_active();

// The rate the pictures are shown at while Swappy paces them: the panel's real refresh rate
// divided by the refreshes each picture lasts (a "60 Hz" panel may really run at 60.48).
double android_pacing_rate();

// Swaps through Swappy, which waits so each picture stays up for the same number of the
// panel's refreshes. Returns false (having done nothing) when Swappy isn't active.
bool android_pacing_swap();
