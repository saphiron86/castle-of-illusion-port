// Android frame pacing.
//
// On Android an OpenGL ES swap doesn't wait for the panel's refresh the way it does on a
// desktop: buffers queue up and the swaps return in bursts. Phones also report their panel's
// highest refresh rate while switching between rates by themselves (a OnePlus 9 shows this at
// 60 Hz while reporting 120). Timed by the port's own clock, the pictures then drift against
// the panel, and a game frame is now and then shown twice: a hitch.
//
// Swappy takes its timing from Android's Choreographer (the panel's real refreshes), tells the
// panel the content is 60 a second (so it settles on 60 or 120 Hz), and makes each swap wait
// until the picture has been up for a whole 60th of a second. The main loop then runs one game
// frame per picture (locked, as on a desktop at 60 Hz).

#include "android_pacing.h"

#include <EGL/egl.h>
#include <jni.h>
#include <swappy/swappyGL.h>
#include <swappy/swappyGL_extra.h>

namespace {

bool g_active = false;

}  // namespace

void android_pacing_init(SDL_Window* window)
{
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity)
        return;
    g_active = SwappyGL_init(env, activity);
    env->DeleteLocalRef(activity);
    if (!g_active) {
        SDL_Log("Swappy could not start; pictures are paced by the port's clock");
        return;
    }
    SwappyGL_setAutoSwapInterval(false);            // always 60 a second, never lowered
    SwappyGL_setSwapIntervalNS(SWAPPY_SWAP_60FPS);
    android_pacing_set_window(window);
}

void android_pacing_set_window(SDL_Window* window)
{
    if (!g_active)
        return;
    if (auto* native = static_cast<ANativeWindow*>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr)))
        SwappyGL_setWindow(native);
}

bool android_pacing_active()
{
    return g_active;
}

double android_pacing_rate()
{
    const uint64_t refresh = SwappyGL_getRefreshPeriodNanos(), interval = SwappyGL_getSwapIntervalNS();
    if (!g_active || refresh == 0)
        return 60.0;
    const uint64_t refreshes = (interval + refresh / 2) / refresh;
    return 1e9 / (double)(refresh * (refreshes ? refreshes : 1));
}

bool android_pacing_swap()
{
    if (!g_active)
        return false;
    return SwappyGL_swap(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW));
}
