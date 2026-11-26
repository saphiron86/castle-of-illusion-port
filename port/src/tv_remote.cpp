#include "tv_remote.h"

#include "bus.h"

// The remote held sideways, its top (the D-pad's end) to the left: the D-pad is turned a
// quarter turn, so the remote's up is the game's left, its right is up, its down is right
// and its left is down. The centre is Start, Back opens the menu (main.cpp), and the three
// buttons after Back along that edge are A, B and C: fast forward, play/pause and rewind.
//
// SDL lists TV remotes as controllers by default (two buttons and a D-pad). With
// SDL_HINT_TV_REMOTE_AS_JOYSTICK off, its Android code leaves out any device with no stick
// and no hat, and sends their presses as keys: the D-pad as the arrow keys, the centre as
// Enter, Back as Escape and the media buttons as media keys. A real controller still shows
// up as one, so the scheme is on exactly while SDL has no controller.

namespace {

bool g_tv = false;

}  // namespace

void tv_remote_init()
{
#ifdef __ANDROID__
    g_tv = SDL_IsTV();
    if (g_tv)
        SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK, "0");
#endif
}

bool tv_remote_on_tv()
{
    return g_tv;
}

bool tv_remote_active()
{
    return g_tv && !SDL_HasGamepad();
}

void tv_remote_event(SDL_Event& event)
{
    if ((event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP) || !tv_remote_active())
        return;
    SDL_Scancode turned;
    switch (event.key.scancode) {
    case SDL_SCANCODE_UP: turned = SDL_SCANCODE_LEFT; break;
    case SDL_SCANCODE_RIGHT: turned = SDL_SCANCODE_UP; break;
    case SDL_SCANCODE_DOWN: turned = SDL_SCANCODE_RIGHT; break;
    case SDL_SCANCODE_LEFT: turned = SDL_SCANCODE_DOWN; break;
    default: return;
    }
    event.key.scancode = turned;
    event.key.key = SDL_GetKeyFromScancode(turned, SDL_KMOD_NONE, false);
}

uint8_t tv_remote_buttons()
{
    const bool* keys = SDL_GetKeyboardState(nullptr);
    uint8_t b = 0;
    if (keys[SDL_SCANCODE_UP]) b |= BTN_LEFT;
    if (keys[SDL_SCANCODE_RIGHT]) b |= BTN_UP;
    if (keys[SDL_SCANCODE_DOWN]) b |= BTN_RIGHT;
    if (keys[SDL_SCANCODE_LEFT]) b |= BTN_DOWN;
    if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SELECT]) b |= BTN_START;
    if (keys[SDL_SCANCODE_MEDIA_FAST_FORWARD]) b |= BTN_A;
    if (keys[SDL_SCANCODE_MEDIA_PLAY_PAUSE] || keys[SDL_SCANCODE_MEDIA_PLAY] || keys[SDL_SCANCODE_MEDIA_PAUSE]) b |= BTN_B;
    if (keys[SDL_SCANCODE_MEDIA_REWIND]) b |= BTN_C;
    return b;
}

const char* tv_remote_help()
{
    return "Playing with the remote: hold it sideways, the D-pad on the left.\n"
           "D-pad: move    Centre: Start    Back: Menu\n"
           "The next three buttons along: A, B and C";
}
