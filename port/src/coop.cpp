#include "coop.h"

#include <algorithm>
#include <cmath>

#include "bus.h"
#include "menu.h"
#include "tv_remote.h"
#include "vdp.h"

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
    if (layout == 0 && tv_remote_active())         // an Android TV remote, held sideways
        return tv_remote_buttons();
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

// Co-op Zoom. Around the players' X and Y (the middle of their feet), in frame pixels: the
// room wanted for the full zoom ("room"), and the least kept in view whatever the easing
// ("keep"; Mickey is about 24 pixels across and 40 tall, a bubble 48 across). The view sits
// with the lower player's feet kFeetRoom above its bottom.
constexpr float kRoomAbove = 88, kRoomBelow = 32;
// Across, the zoom starts when the players are kZoomStart of the frame's width apart (110
// pixels in widescreen, 88 in 4:3) and is full from kZoomFull (40, 32), linear between.
constexpr float kZoomStart = 0.275f, kZoomFull = 0.1f;
// How far it zooms in: kZoomMoving, or kZoomStill once both players have stood still (X and Y
// unchanged) for kStillFrames; the spring eases between them. None in boss stages (StageIndex
// $FFFF42 in kBossStages: stages 5, 9, 13, 23, 27 and 28), where it still zooms out.
constexpr float kZoomMoving = 1.1f, kZoomStill = 1.25f;
constexpr int kStillFrames = 120;
constexpr int kBossStages[] = {4, 8, 12, 22, 26, 27};
// Zooming out (when the frame is drawn wider): from kOutStart of the view's width apart
// across (300 pixels in widescreen; the edges drag a player at 368) to all of the frame at
// kOutFull (360); or by height apart (lasting, as above), from kOutRise of the play lines
// (106) to kOutRiseFull (154).
constexpr float kOutStart = 0.75f, kOutFull = 0.9f, kOutRise = 0.55f, kOutRiseFull = 0.8f;
constexpr float kKeepSide = 24, kKeepAbove = 42, kKeepBelow = 2, kFeetRoom = 24;
// Easing: critically damped springs (rates in radians a second; about 2.5 / rate seconds to
// settle), slow for the zoom, quicker for the centre. The height between the players counts
// toward the zoom wanted only as it lasts (kRise a frame: a jump's barely does), so a jump
// doesn't make the view pump; it still never leaves the jumper out ("keep").
constexpr float kZoomRate = 2.0f, kCentreRate = 4.0f, kRise = 0.01f, kFrameTime = 1.0f / 60;
// Moving apart, the zoom wanted is no more than keeps them in ("keep") with room for where
// they will be kLead frames on at the speed they are moving apart (smoothed), plus kLeadRoom:
// the spring then zooms out ahead of them. Without it the easing fell behind and "keep" took
// over at once, the zoom-out suddenly following them three times as fast (the user's report).
constexpr float kLead = 45, kLeadRoom = 16, kSpreadSmooth = 0.1f;

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

void CoopInput::state_loaded(bool coop)
{
    if (coop && !joined) {
        joined = true;
        p2 = Device{};
        request_leave("Loaded a co-op state: Player 2, press Start to join");
    } else if (!coop && joined) {
        p2_left = p2;
        leave();
    }
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

void CoopZoom::step(const uint8_t* ram, bool active, const Frame& frame)
{
    if (frame.width != f.width || frame.rows != f.rows || frame.play_lines != f.play_lines)
        prev.width = 0;                             // (no easing from another frame's shape)
    else if (frame.view_top != f.view_top) {        // the rows above the view changed: the picture
        const float moved = (float)(frame.view_top - f.view_top);   // moved down that much in the
        cy += moved, cur.top += moved, prev.top += moved;            // frame, and the crop with it
    }
    f = frame;
    const float w = (float)f.view_width, h = (float)f.play_lines;   // the usual view of play
    auto word = [ram](uint32_t a) { return (int16_t)(ram[a] << 8 | ram[a + 1]); };
    // Zooming out: down to all of the wider frame (a tenth), keeping the view's shape, but
    // never past the level's ends (CameraA's width in blocks, $FFFA8E) where the usual view
    // doesn't already show past them: at a level's end it widens on the other side only.
    float span[2] = {(float)f.view_left, (float)f.view_left + w};      // across the frame
    if (f.extra_rows && active) {
        const int camera_x = word(0xFA84), level_width = word(0xFA8E) * 16;
        const float level_left = (float)(f.camera_left - camera_x), level_right = level_left + level_width;
        span[0] = std::clamp(level_left, 0.0f, span[0]);
        span[1] = std::clamp(level_right, span[1], (float)f.width);
    }
    const float min_zoom = f.extra_rows ? std::max(w / (span[1] - span[0]), h / (h + f.extra_rows)) : 1.0f;

    // The players to keep in view: those playing or in a bubble (coop.asm's CoopStatus 0 and
    // 3). One dying, waiting (hidden, kept on the other) or out of tries doesn't count. Their
    // places in the frame: X and Y less the camera (CameraA's X $FFFA84, Y $FFFA80), plus the
    // widescreen margin; Player 2's from his saved slot ($FFFC10).
    float box[4] = {1e9f, 1e9f, -1e9f, -1e9f};      // left, top, right, bottom of their feet
    int players = 0;
    if (active) {
        const int camera_x = word(0xFA84), camera_y = word(0xFA80);
        const uint32_t slots[2] = {0xDC00, 0xFC10};
        for (int i = 0; i < 2; i++) {
            const uint8_t status = ram[0xFD68 + i];
            if (status != 0 && status != 3)
                continue;
            const float x = (float)(word(slots[i] + 0x10) - camera_x + f.camera_left);
            const float y = (float)(word(slots[i] + 0x14) - camera_y + f.view_top);
            box[0] = std::min(box[0], x), box[1] = std::min(box[1], y);
            box[2] = std::max(box[2], x), box[3] = std::max(box[3], y);
            players++;
        }
    }
    // Still: both in play, neither moved since last frame.
    bool moved = players < 2;
    for (int i = 0; i < 2; i++) {
        const uint32_t slot = i ? 0xFC10 : 0xDC00;
        const int x = word(slot + 0x10), y = word(slot + 0x14);
        moved |= x != last_x[i] || y != last_y[i];
        last_x[i] = x, last_y[i] = y;
    }
    still = moved ? 0 : std::min(still + 1, kStillFrames);
    const int stage = word(0xFF42);
    const bool boss = std::find(std::begin(kBossStages), std::end(kBossStages), stage) != std::end(kBossStages);
    const float max_zoom = boss ? 1.0f : still >= kStillFrames ? kZoomStill : kZoomMoving;
    float target = 1, limit = max_zoom;             // the zoom wanted, and the most that keeps them in
    if (players) {
        const float spread_x = box[2] - box[0], spread_y = box[3] - box[1];
        rise = spread_y < rise ? spread_y : rise + (spread_y - rise) * kRise;
        const float across = std::clamp((kZoomStart * w - spread_x) / ((kZoomStart - kZoomFull) * w), 0.0f, 1.0f);
        target = std::min(1 + (max_zoom - 1) * across, h / (rise + kRoomAbove + kRoomBelow));
        target = std::clamp(target, 1.0f, max_zoom);
        if (target == 1.0f && min_zoom < 1.0f) {   // far apart: out
            const float out = std::max(std::clamp((spread_x - kOutStart * w) / ((kOutFull - kOutStart) * w), 0.0f, 1.0f),
                                       std::clamp((rise - kOutRise * h) / ((kOutRiseFull - kOutRise) * h), 0.0f, 1.0f));
            target = 1 - (1 - min_zoom) * out;
        }
        if (players < 2)
            target = 1;                             // one in play (the other dying, waiting or out): the usual view
        limit = std::min(w / (spread_x + 2 * kKeepSide), h / (spread_y + kKeepAbove + kKeepBelow));
        if (players == 2 && spread_last >= 0)
            spread_speed += (spread_x - spread_last - spread_speed) * kSpreadSmooth;
        else
            spread_speed = 0;
        spread_last = players == 2 ? spread_x : -1;
        const float ahead = spread_x + 2 * kKeepSide + kLeadRoom + std::max(spread_speed, 0.0f) * kLead;
        target = std::min(target, std::max(w / ahead, min_zoom));
    } else {
        rise = 0;
        spread_last = -1, spread_speed = 0;
    }

    zoom_speed += (kZoomRate * kZoomRate * (target - zoom) - 2 * kZoomRate * zoom_speed) * kFrameTime;
    zoom += zoom_speed * kFrameTime;
    if (players && zoom > std::max(limit, min_zoom)) {  // never leave a player out: out at once
        zoom = std::max(limit, min_zoom);
        zoom_speed = std::min(zoom_speed, 0.0f);
    }
    if (zoom <= min_zoom) {
        zoom = min_zoom;
        zoom_speed = std::max(zoom_speed, 0.0f);
    }
    if (target == 1.0f && std::fabs(zoom - 1.0f) < 0.0005f && std::fabs(zoom_speed) < 0.001f) {
        zoom = 1.0f;                                // at rest
        zoom_speed = 0;
    }

    // The centre: between the players across; down, the lower one's feet near the bottom
    // (the view moves up for a jump only when the jumper would leave it). Eased; at rest (no
    // zoom) it waits there.
    const float want_x = players ? (box[0] + box[2]) / 2 : cx;
    const float want_y = players ? box[3] + kFeetRoom - h / zoom / 2 : cy;
    if (zoom == 1.0f) {
        cx = want_x, cy = want_y;
        cx_speed = cy_speed = 0;
    } else {
        cx_speed += (kCentreRate * kCentreRate * (want_x - cx) - 2 * kCentreRate * cx_speed) * kFrameTime;
        cy_speed += (kCentreRate * kCentreRate * (want_y - cy) - 2 * kCentreRate * cy_speed) * kFrameTime;
        cx += cx_speed * kFrameTime;
        cy += cy_speed * kFrameTime;
    }

    // The crop: around the centre, moved as far as it must to keep both players in, and
    // inside the usual view's picture of play; zooming out, inside a region growing from it
    // to all of the frame's picture of play as the zoom goes down (so the crop never jumps).
    Crop c;
    c.width = w / zoom;
    c.height = h / zoom;
    float region[4] = {(float)f.view_left, (float)f.view_top, w, h};   // left, top, width, height
    if (zoom < 1.0f && min_zoom < 1.0f) {
        const float t = std::min((1 - zoom) / (1 - min_zoom), 1.0f);
        region[0] = f.view_left + (span[0] - f.view_left) * t;
        region[2] = w + (span[1] - span[0] - w) * t;
        region[1] = f.view_top * (1 - t);
        region[3] = h + f.extra_rows * t;
    }
    float left = cx - c.width / 2, top = cy - c.height / 2;
    if (players) {
        left = std::max(std::min(left, box[0] - kKeepSide), box[2] + kKeepSide - c.width);
        top = std::max(std::min(top, box[1] - kKeepAbove), box[3] + kKeepBelow - c.height);
    }
    left = std::clamp(left, region[0], std::max(region[0], region[0] + region[2] - c.width));
    top = std::clamp(top, region[1], std::max(region[1], region[1] + region[3] - c.height));
    if (left + c.width / 2 != cx)
        cx = left + c.width / 2, cx_speed = 0;
    if (top + c.height / 2 != cy)
        cy = top + c.height / 2, cy_speed = 0;
    c.left = left;
    c.top = top;
    prev = prev.width > 0 ? cur : c;
    cur = c;
}

ZoomView CoopZoom::view(double back) const
{
    auto usual = [this](const Crop& c) { return std::fabs(c.width - f.view_width) < 0.01f; };
    if (cur.width <= 0 || (!f.extra_rows && usual(cur) && usual(prev)))
        return {};                                  // the usual frame, not enlarged: as it is
    const float b = (float)std::clamp(back, 0.0, 1.0);
    auto mix = [b](float now, float before) { return now + (before - now) * b; };
    ZoomView v;
    v.left = mix(cur.left, prev.left) / f.width;
    v.top = mix(cur.top, prev.top) / f.rows;
    v.width = mix(cur.width, prev.width) / f.width;
    v.height = mix(cur.height, prev.height) / f.rows;
    v.split = (float)f.play_lines / Vdp::kScreenHeight;
    // The HUD: its lines, below the picture of play's rows, as wide as the usual view.
    v.hud_left = (float)f.view_left / f.width;
    v.hud_top = (float)(f.play_lines + f.extra_rows) / f.rows;
    v.hud_width = (float)f.view_width / f.width;
    v.hud_height = (float)(Vdp::kScreenHeight - f.play_lines) / f.rows;
    v.screen_width = (float)f.view_width;
    v.screen_height = (float)Vdp::kScreenHeight;
    v.resample = !usual(cur) || !usual(prev);
    return v;
}
