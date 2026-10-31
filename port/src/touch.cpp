// On-screen touch controls.
//
// Sized from the screen's height so they sit under the thumbs on any phone: the D-pad at the
// bottom left, A, B and C rising to the right like the Genesis pad's row, Start at the bottom
// in the middle and the menu button in the top right corner. A finger that lands on the D-pad
// steers it wherever it slides; any other finger presses whichever button it is over, so a
// thumb can roll from one button to the next. They show on Android while no controller is in
// use: a controller's buttons hide them, a touch brings them back.

#include "touch.h"

#include "bus.h"

#include "imgui.h"

#include <cmath>
#include <vector>

namespace {

struct Finger {
    SDL_FingerID id;
    float x, y;                                     // in ImGui's display units
    bool dpad;                                      // landed on the D-pad: steers it
};

SDL_Window* g_window = nullptr;
std::vector<Finger> g_fingers;
bool g_menu_tapped = false;
#ifdef __ANDROID__
bool g_shown = true;
#else
bool g_shown = false;                               // desktops have a keyboard
#endif

struct Layout {
    float u;                                        // a hundredth of the screen's height
    ImVec2 dpad; float dpad_radius;
    ImVec2 face[3]; float face_radius;              // A, B, C
    ImVec2 start, start_half;                       // centre and half size
    ImVec2 menu; float menu_radius;
};

Layout layout()
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    Layout l;
    const float u = l.u = size.y / 100.0f, w = size.x, h = size.y;
    l.dpad = ImVec2(24 * u, h - 27 * u);
    l.dpad_radius = 18 * u;
    l.face_radius = 8.5f * u;
    l.face[0] = ImVec2(w - 52 * u, h - 17 * u);
    l.face[1] = ImVec2(w - 32 * u, h - 25 * u);
    l.face[2] = ImVec2(w - 12 * u, h - 33 * u);
    l.start = ImVec2(w / 2, h - 6 * u);
    l.start_half = ImVec2(8 * u, 3 * u);
    // The menu button keeps inside the safe area: phones draw into the camera's cutout, which
    // sits in a corner in landscape. There is room beside a 16:9 picture on wider screens.
    float right = w, top = 0;
    SDL_Rect safe;
    if (g_window && SDL_GetWindowSafeArea(g_window, &safe) && safe.w > 0) {
        right = (float)(safe.x + safe.w);
        top = (float)safe.y;
    }
    l.menu = ImVec2(right - 7 * u, top + 7 * u);
    l.menu_radius = 4.5f * u;
    return l;
}

float distance(ImVec2 a, float x, float y)
{
    return std::hypot(x - a.x, y - a.y);
}

constexpr uint8_t kFaceButtons[3] = {BTN_A, BTN_B, BTN_C};

uint8_t dpad_buttons(const Layout& l, float x, float y)
{
    const float dx = x - l.dpad.x, dy = y - l.dpad.y;
    if (std::hypot(dx, dy) < 0.2f * l.dpad_radius)
        return 0;
    // Eight sectors of 45 degrees: the diagonals press two directions.
    const int sector = (int)std::lround(std::atan2(dy, dx) / (3.14159265f / 4)) & 7;
    static constexpr uint8_t kSectors[8] = {
        BTN_RIGHT, BTN_RIGHT | BTN_DOWN, BTN_DOWN, BTN_DOWN | BTN_LEFT,
        BTN_LEFT, BTN_LEFT | BTN_UP, BTN_UP, BTN_UP | BTN_RIGHT};
    return kSectors[sector];
}

uint8_t finger_buttons(const Layout& l, const Finger& f)
{
    if (f.dpad)
        return dpad_buttons(l, f.x, f.y);
    uint8_t b = 0;
    for (int i = 0; i < 3; i++)
        if (distance(l.face[i], f.x, f.y) < 1.2f * l.face_radius)
            b |= kFaceButtons[i];
    if (std::fabs(f.x - l.start.x) < l.start_half.x + 2 * l.u && std::fabs(f.y - l.start.y) < l.start_half.y + 3 * l.u)
        b |= BTN_START;
    return b;
}

}  // namespace

void touch_init(SDL_Window* window)
{
    g_window = window;
}

bool touch_shown()
{
    return g_shown;
}

void touch_event(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION: {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const float x = event.tfinger.x * size.x, y = event.tfinger.y * size.y;
        for (Finger& f : g_fingers)
            if (f.id == event.tfinger.fingerID) {
                f.x = x;
                f.y = y;
                return;
            }
        if (event.type != SDL_EVENT_FINGER_DOWN)
            return;
#ifdef __ANDROID__
        g_shown = true;
#endif
        if (!g_shown)
            return;
        const Layout l = layout();
        if (distance(l.menu, x, y) < 2 * l.menu_radius) {
            g_menu_tapped = true;
            return;
        }
        g_fingers.push_back({event.tfinger.fingerID, x, y, distance(l.dpad, x, y) < 1.3f * l.dpad_radius});
        break;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        for (size_t i = 0; i < g_fingers.size(); i++)
            if (g_fingers[i].id == event.tfinger.fingerID) {
                g_fingers.erase(g_fingers.begin() + i);
                break;
            }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        g_shown = false;
        g_fingers.clear();
        break;
    default:
        break;
    }
}

uint8_t touch_buttons()
{
    if (!g_shown)
        return 0;
    const Layout l = layout();
    uint8_t b = 0;
    for (const Finger& f : g_fingers)
        b |= finger_buttons(l, f);
    return b;
}

bool touch_menu_tapped()
{
    const bool tapped = g_menu_tapped;
    g_menu_tapped = false;
    return tapped;
}

void touch_release()
{
    g_fingers.clear();
}

void touch_draw()
{
    if (!g_shown)
        return;
    const Layout l = layout();
    const uint8_t held = touch_buttons();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 fill = IM_COL32(255, 255, 255, 40), pressed = IM_COL32(255, 255, 255, 110);
    const ImU32 line = IM_COL32(255, 255, 255, 120), text = IM_COL32(255, 255, 255, 170);
    const float thick = 0.4f * l.u;

    // D-pad: a ring, with the held direction's arrows lit.
    draw->AddCircleFilled(l.dpad, l.dpad_radius, fill, 48);
    draw->AddCircle(l.dpad, l.dpad_radius, line, 48, thick);
    struct Arrow { uint8_t button; float dx, dy; };
    for (const Arrow& a : {Arrow{BTN_UP, 0, -1}, Arrow{BTN_DOWN, 0, 1}, Arrow{BTN_LEFT, -1, 0}, Arrow{BTN_RIGHT, 1, 0}}) {
        const ImVec2 tip(l.dpad.x + a.dx * 0.8f * l.dpad_radius, l.dpad.y + a.dy * 0.8f * l.dpad_radius);
        const ImVec2 base(l.dpad.x + a.dx * 0.5f * l.dpad_radius, l.dpad.y + a.dy * 0.5f * l.dpad_radius);
        const float s = 0.22f * l.dpad_radius;
        draw->AddTriangleFilled(tip, ImVec2(base.x - a.dy * s, base.y + a.dx * s), ImVec2(base.x + a.dy * s, base.y - a.dx * s),
                                held & a.button ? pressed : line);
    }

    ImFont* font = ImGui::GetFont();
    const float letter = 7 * l.u;
    auto label = [&](ImVec2 centre, const char* s, float size) {
        const ImVec2 extent = font->CalcTextSizeA(size, 1e9f, 0, s);
        draw->AddText(font, size, ImVec2(centre.x - extent.x / 2, centre.y - extent.y / 2), text, s);
    };
    static constexpr const char* kNames[3] = {"A", "B", "C"};
    for (int i = 0; i < 3; i++) {
        draw->AddCircleFilled(l.face[i], l.face_radius, held & kFaceButtons[i] ? pressed : fill, 40);
        draw->AddCircle(l.face[i], l.face_radius, line, 40, thick);
        label(l.face[i], kNames[i], letter);
    }

    const ImVec2 s0(l.start.x - l.start_half.x, l.start.y - l.start_half.y), s1(l.start.x + l.start_half.x, l.start.y + l.start_half.y);
    draw->AddRectFilled(s0, s1, held & BTN_START ? pressed : fill, l.start_half.y);
    draw->AddRect(s0, s1, line, l.start_half.y, 0, thick);
    label(l.start, "START", 3.5f * l.u);

    // Menu: three bars in a circle.
    draw->AddCircleFilled(l.menu, l.menu_radius, fill, 24);
    draw->AddCircle(l.menu, l.menu_radius, line, 24, thick);
    for (int i = -1; i <= 1; i++) {
        const float y = l.menu.y + i * 0.35f * l.menu_radius, half = 0.5f * l.menu_radius;
        draw->AddLine(ImVec2(l.menu.x - half, y), ImVec2(l.menu.x + half, y), text, thick);
    }
}
