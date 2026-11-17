#include "coop.h"

#include <algorithm>

#include "bus.h"
#include "menu.h"

uint8_t read_pad(SDL_Gamepad* pad)
{
    constexpr Sint16 kStickThreshold = 13000;       // about 40% of full travel
    auto held = [pad](SDL_GamepadButton button) { return SDL_GetGamepadButton(pad, button); };
    const Sint16 x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    const Sint16 y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    uint8_t b = 0;
    if (held(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -kStickThreshold) b |= BTN_UP;
    if (held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > kStickThreshold) b |= BTN_DOWN;
    if (held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -kStickThreshold) b |= BTN_LEFT;
    if (held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > kStickThreshold) b |= BTN_RIGHT;
    if (held(SDL_GAMEPAD_BUTTON_WEST)) b |= BTN_A;      // Xbox X
    if (held(SDL_GAMEPAD_BUTTON_SOUTH)) b |= BTN_B;     // Xbox A
    if (held(SDL_GAMEPAD_BUTTON_EAST)) b |= BTN_C;      // Xbox B
    if (held(SDL_GAMEPAD_BUTTON_START)) b |= BTN_START; // Xbox menu
    return b;
}

uint8_t read_keys(int layout)
{
    static const SDL_Scancode kKeys[2][8] = {
        {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
         SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_Z, SDL_SCANCODE_RETURN},
        {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D,
         SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_F, SDL_SCANCODE_TAB}};
    const bool* keys = SDL_GetKeyboardState(nullptr);
    uint8_t b = 0;
    for (int i = 0; i < 8; i++)                     // in Button's bit order: up, down, left, right, B, C, A, Start
        if (keys[kKeys[layout][i]])
            b |= (uint8_t)(1 << i);
    return b;
}

bool read_pad_bubble(SDL_Gamepad* pad)
{
    return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);    // Xbox Y
}

bool read_keys_bubble(int layout)
{
    return SDL_GetKeyboardState(nullptr)[layout == 0 ? SDL_SCANCODE_RSHIFT : SDL_SCANCODE_LSHIFT];
}

namespace {

constexpr Uint64 kContinueMs = 2000;                // Start held this long, out of tries: continue
constexpr Uint64 kQuitShowMs = 1000;                // Start held this long: "Hold Start to quit"
constexpr Uint64 kQuitMs = 5000;                    // ... and this long: Player 2 drops out
constexpr Uint64 kFadeMs = 500;
constexpr Uint64 kKeysHintMs = 5000;                // "press Enter to join" at stage 1's start

}  // namespace

void CoopInput::update(const std::vector<SDL_Gamepad*>& pads, bool keyboard_free, bool can_join)
{
    struct Read {
        Device device;
        uint8_t buttons;
        bool bubble;
    };
    std::vector<Read> reads;
    if (keyboard_free) {
        // Player 1's bubble: Right Shift, or Left Shift too unless Player 2 plays on the
        // second layout (whose bubble it is).
        const bool keys2_p2 = joined && p2.kind == Device::Keys2;
        reads.push_back({{Device::Keys1, 0}, read_keys(0),
                         read_keys_bubble(0) || (!keys2_p2 && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_LSHIFT])});
        reads.push_back({{Device::Keys2, 0}, read_keys(1), read_keys_bubble(1)});
    }
    for (SDL_Gamepad* pad : pads)
        reads.push_back({{Device::Pad, SDL_GetGamepadID(pad)}, read_pad(pad), read_pad_bubble(pad)});

    // New presses, against last frame (devices are matched by kind and controller).
    static std::vector<Read> last;
    auto pressed = [&](const Read& r) {
        for (const Read& l : last)
            if (l.device == r.device)
                return (uint8_t)(r.buttons & ~l.buttons);
        return r.buttons;
    };

    // Drop in: Start on a device Player 1 isn't using (the second keyboard layout, or a
    // controller or the first layout when Player 1 last used something else).
    if (!joined && can_join)
        for (const Read& r : reads)
            if ((pressed(r) & BTN_START) && !(r.device == p1_last)) {
                joined = true;
                p2 = r.device;
                p2_left = Device{};
                p2_start_was_down = true;           // this Start doesn't count towards quitting
                start_down_since = 0;
                menu_notice("Player 2 joined", 3.0);
                break;
            }

    p1 = 0;
    p2_buttons = 0;
    p1_bubble = p2_bubble = false;
    for (const Read& r : reads) {
        if (joined && r.device == p2) {
            p2_buttons = r.buttons;
            p2_bubble = r.bubble;
            continue;
        }
        if (r.device.kind == Device::Keys2 || r.device == p2_left)
            continue;                               // Player 2's layout, or the device they left with
        p1 |= r.buttons;
        p1_bubble |= r.bubble;
        if (r.buttons)
            p1_last = r.device;
    }
    last = reads;

    const Uint64 now = SDL_GetTicks();
    p1_continue = p2_continue = false;
    p2_start_to_game = false;
    if (joined && leave_pending) {
        p2_buttons = 0;                             // leaving: the game brings Player 1 back first
        p2_bubble = false;
    } else if (joined) {
        // Player 1 out of tries: their Start held 2 seconds continues, and doesn't pause;
        // Player 2's pauses instead.
        if (p1_out) {
            const bool start = p1 & BTN_START;
            p1 &= (uint8_t)~BTN_START;
            if (!start)
                p1_start_since = 0;
            else if (!p1_start_since)
                p1_start_since = now;
            else if (now - p1_start_since >= kContinueMs) {
                p1_continue = true;
                p1_start_since = 0;
            }
        } else
            p1_start_since = 0;
        // Player 2's Start never pauses (but for that); held 5 seconds it drops them out, or
        // out of tries, held 2 seconds it continues.
        const bool start = p2_buttons & BTN_START;
        p2_buttons &= (uint8_t)~BTN_START;
        if (p1_out)
            p2_start_to_game = start;
        if (p2_out) {
            if (!start)
                p2_continue_since = 0;
            else if (!p2_continue_since)
                p2_continue_since = now;
            else if (now - p2_continue_since >= kContinueMs) {
                p2_continue = true;
                p2_continue_since = 0;
            }
            start_down_since = 0;
            p2_start_was_down = start;
        } else {
            p2_continue_since = 0;
            if (start && !p2_start_was_down)
                start_down_since = now;
            if (!start && start_down_since) {
                if (now - start_down_since >= kQuitShowMs)
                    quit_shown_until = now + kFadeMs;  // released early: fade out
                start_down_since = 0;
            }
            p2_start_was_down = start;
            if (start_down_since && now - start_down_since >= kQuitMs) {
                start_down_since = 0;
                quit_shown_until = 0;
                p2_buttons = 0;
                request_leave("Player 2 left");
            }
        }
    }

    // Out of tries: how to continue (Player 1 top left, Player 2 top right).
    if (joined && !leave_pending && p1_out)
        menu_status_left("Player 1, hold Start to continue", 1.0f);
    // The top right corner: how to continue, how to quit while Start is held, else how to
    // join while a second controller is there for Player 2.
    if (joined && !leave_pending && p2_out)
        menu_status("Player 2, hold Start to continue", 1.0f);
    else if (joined && start_down_since && now - start_down_since >= kQuitShowMs)
        menu_status("Hold Start to quit", 1.0f);
    else if (joined && now < quit_shown_until)
        menu_status("Hold Start to quit", (float)(quit_shown_until - now) / kFadeMs);
    else if (!joined && can_join && (pads.size() >= 2 || (pads.size() == 1 && p1_last.kind == Device::Keys1)))
        menu_status("Player 2 press Start to join", 1.0f);
    else if (!joined && keyboard_free && p1_last.kind == Device::Pad && now < keys_hint_until)
        menu_status("Player 2 press Enter to join", std::min(1.0f, (float)(keys_hint_until - now) / kFadeMs));
}

void CoopInput::request_leave(const char* notice)
{
    leave_pending = true;                           // Player 1 back in play and the camera on
    leave_notice = notice;                          // him first (main.cpp, then finish_leave)
}

void CoopInput::finale_leave()
{
    if (joined && !leave_pending)
        request_leave("");                          // (no notice: the ending is under way)
}

void CoopInput::left()
{
    joined = false;
    p2_left = p2;
}

void CoopInput::finish_leave()
{
    if (!leave_pending)
        return;
    leave_pending = false;
    left();
    p2_buttons = 0;
    p2_bubble = false;
    if (!leave_notice.empty())
        menu_notice(leave_notice.c_str(), 3.0);
}

void CoopInput::leave()
{
    leave_pending = false;
    joined = false;
    p2_buttons = 0;
    start_down_since = 0;
    quit_shown_until = 0;
    p2_start_was_down = false;
}

void CoopInput::stage_1_started()
{
    keys_hint_until = SDL_GetTicks() + kKeysHintMs;
}

void CoopInput::pad_removed(SDL_JoystickID id)
{
    if (p2_left.kind == Device::Pad && p2_left.pad == id)
        p2_left = Device{};                         // unplugged: forgotten
    if (joined && p2.kind == Device::Pad && p2.pad == id) {
        start_down_since = 0;
        quit_shown_until = 0;
        p2_buttons = 0;
        request_leave("Player 2 left (controller disconnected)");
    }
}
