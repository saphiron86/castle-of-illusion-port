// Co-op: Player 2's input (drop in and out), on the port's side. See docs/notes/coop.md.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

// The game's buttons from one controller, and from one of the keyboard's two layouts
// (0: arrows, Z/X/C, Enter; 1: W/A/S/D, F/G/H, Tab).
uint8_t read_pad(SDL_Gamepad* pad);
uint8_t read_keys(int layout);
// Co-op's bubble button (not one of the game's): Y on a controller, Right Shift with the
// first keyboard layout (Left Shift too while Player 2 isn't on the second), Left Shift with
// the second.
bool read_pad_bubble(SDL_Gamepad* pad);
bool read_keys_bubble(int layout);

struct CoopInput {
    // An input device: a keyboard layout or a controller.
    struct Device {
        enum Kind { None, Keys1, Keys2, Pad } kind = None;
        SDL_JoystickID pad = 0;
        bool operator==(const Device& o) const { return kind == o.kind && (kind != Pad || pad == o.pad); }
    };

    bool joined = false;                // Player 2 is playing
    Device p2;                          // Player 2's device while joined
    Device p1_last{Device::Keys1, 0};   // the device Player 1 last pressed something on
    Device p2_left;                     // the device Player 2 left with: not Player 1's (until
                                        // Player 2 joins with it again, or it is unplugged)
    uint8_t p1 = 0, p2_buttons = 0;     // this frame's buttons (Player 2's without Start)
    bool p1_bubble = false, p2_bubble = false;  // the bubble button held

    // Set before update, from the game (coop.asm's CoopStatus): a player out of tries, and
    // Player 1 out of play (dying, waiting, in a bubble). Out of tries, Start held 2 seconds
    // continues (p1_continue, p2_continue: once), and Player 1's Start no longer pauses; the
    // other player's does (p2_start_to_game).
    bool p1_out = false, p2_out = false, p1_parked = false;
    bool p1_continue = false, p2_continue = false;
    bool p2_start_to_game = false;

    // Player 2 is leaving while Player 1 is out of play: the game brings Player 1 back first
    // (coop.asm's ReviveP1), then finish_leave.
    bool leave_pending = false;
    void finish_leave();

    // Reads every device. `keyboard_free`: the menu doesn't have the keyboard; `can_join`:
    // level play, so Player 2 may drop in. Shows the join message and the hold-to-quit one.
    void update(const std::vector<SDL_Gamepad*>& pads, bool keyboard_free, bool can_join);

    // Player 2's controller was unplugged: Player 2 drops out.
    void pad_removed(SDL_JoystickID id);

    // Back to single player at once, without a notice (the game was reset, or went back to
    // the title).
    void leave();

    // Stage 1's play has begun: while Player 1 plays on a controller, the keyboard is free for
    // Player 2, so "Player 2 press Enter to join" shows for 5 seconds.
    void stage_1_started();

    // The final boss's fade to white is over: Player 2 leaves (no notice), and the ending
    // plays as it does alone.
    void finale_leave();

private:
    void request_leave(const char* notice);
    void left();                        // Player 2 is out: their device stays theirs
    std::string leave_notice;
    Uint64 p1_start_since = 0, p2_continue_since = 0;   // Start held while out of tries
    Uint64 start_down_since = 0;        // Player 2 holding Start (0: not held)
    Uint64 quit_shown_until = 0;        // the hold-to-quit message, fading after a release
    Uint64 keys_hint_until = 0;         // "press Enter to join" (stage_1_started)
    bool p2_start_was_down = false;
};
