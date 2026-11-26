#include "menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "stages.h"
#include "touch.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"

namespace {

bool g_open = false;
SDL_Window* g_menu_window = nullptr;
bool g_remote_nav = false;                          // menu_remote_nav: the keyboard keeps the bar focused like a controller
bool g_focus_bar = false;                           // put the nav cursor on the first menu next frame
int g_last_menu = 0;                                // index of the top-level menu last opened
// Short messages at the top of the screen (menu_notice): each until its time is up, fading
// out over its last half second.
struct Notice {
    std::string text;
    Uint64 until;
};
std::vector<Notice> g_notices;

void draw_notices()
{
    const Uint64 now = SDL_GetTicks();
    g_notices.erase(std::remove_if(g_notices.begin(), g_notices.end(), [&](const Notice& n) { return now >= n.until; }),
                    g_notices.end());
    if (g_notices.empty())
        return;
    const ImGuiViewport* view = ImGui::GetMainViewport();
    const float top = view->Pos.y + (g_open ? ImGui::GetFrameHeight() : 0.0f) + 8.0f;
    float fade = 1.0f;
    for (const Notice& n : g_notices)
        fade = std::min(fade, std::max(0.0f, (float)(n.until - now) / 500.0f));
    ImGui::SetNextWindowPos(ImVec2(view->Pos.x + view->Size.x / 2, top), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.7f * std::min(1.0f, fade));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::min(1.0f, fade));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                 | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
                                 | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##notices", nullptr, flags))
        for (const Notice& n : g_notices)
            ImGui::TextUnformatted(n.text.c_str());
    ImGui::End();
    ImGui::PopStyleVar();
}

// The launch hint (menu_hint): top left, until its time is up, fading out over the last half
// second; its text can change while it shows.
Notice g_hint;
std::string g_status;                               // menu_status: this frame's top right line
float g_status_alpha = 0.0f;
std::string g_status_left;                          // menu_status_left: this frame's top left line
float g_status_left_alpha = 0.0f;

void draw_hint()
{
    const Uint64 now = SDL_GetTicks();
    if (g_hint.text.empty() || now >= g_hint.until || g_open)
        return;
    const float fade = std::min(1.0f, (float)(g_hint.until - now) / 500.0f);
    const ImGuiViewport* view = ImGui::GetMainViewport();
    SDL_Rect safe;                                  // clear of a phone's camera cut-out
    float left = view->Pos.x + 8.0f, top = view->Pos.y + 8.0f;
    if (SDL_GetWindowSafeArea(g_menu_window, &safe) && safe.w > 0) {
        left = std::max(left, (float)safe.x + 8.0f);
        top = std::max(top, (float)safe.y + 8.0f);
    }
    ImGui::SetNextWindowPos(ImVec2(left, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.7f * fade);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                 | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
                                 | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##hint", nullptr, flags))
        ImGui::TextUnformatted(g_hint.text.c_str());
    ImGui::End();
    ImGui::PopStyleVar();
}

bool g_crt_window = false;                          // the CRT settings window is open
bool g_focus_crt_window = false;
constexpr const char* kCrtWindow = "CRT Shader Settings";

bool crt_window_focused()
{
    const ImGuiWindow* w = ImGui::GetCurrentContext()->NavWindow;
    return g_crt_window && w && std::strcmp(w->Name, kCrtWindow) == 0;
}

// The CRT settings window. Changes apply at once; returns true when one is finished
// (a slider released, a box ticked), so the caller saves the settings.
bool crt_settings_window(CrtSettings& c)
{
    bool save = false;
    if (g_focus_crt_window) {
        ImGui::SetNextWindowFocus();
        g_focus_crt_window = false;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Size.x * 0.5f, ImGui::GetMainViewport()->Size.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin(kCrtWindow, &g_crt_window, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        auto done = [&] { save |= ImGui::IsItemDeactivatedAfterEdit(); };
        save |= ImGui::Checkbox("CRT shader on", &c.enabled);

        ImGui::SeparatorText("Beam");
        // The shader's sharpness values are negative exponents; show them as positive numbers.
        float scan = -c.scanline_sharpness, pix = -c.pixel_sharpness;
        if (ImGui::SliderFloat("Scanline sharpness", &scan, 1.0f, 24.0f, "%.1f"))
            c.scanline_sharpness = -scan;
        done();
        ImGui::SetItemTooltip("Higher: thinner scanlines with darker gaps between them.");
        if (ImGui::SliderFloat("Pixel sharpness", &pix, 0.5f, 12.0f, "%.1f"))
            c.pixel_sharpness = -pix;
        done();
        ImGui::SetItemTooltip("Higher: crisper pixels across; lower: softer, blended pixels.");
        ImGui::SliderFloat("Glow", &c.glow, 0.0f, 0.5f, "%.3f");
        done();
        ImGui::SetItemTooltip("A soft halo around bright areas.");
        ImGui::SliderFloat("Brightness", &c.brightness, 0.5f, 2.0f, "%.2f");
        done();

        ImGui::SeparatorText("Phosphor mask");
        static const char* kMasks[] = {"None", "Slot mask", "Aperture grille", "Stretched VGA", "VGA"};
        save |= ImGui::Combo("Mask type", &c.mask_type, kMasks, IM_ARRAYSIZE(kMasks));
        ImGui::SliderFloat("Mask dark", &c.mask_dark, 0.0f, 1.0f, "%.2f");
        done();
        ImGui::SetItemTooltip("Brightness of the unlit phosphors: lower is a stronger mask.");
        ImGui::SliderFloat("Mask light", &c.mask_light, 1.0f, 2.0f, "%.2f");
        done();
        ImGui::SetItemTooltip("Brightness of the lit phosphors, to make up for the dark ones.");
        ImGui::SliderInt("Mask size", &c.mask_size, 1, 4);
        done();
        ImGui::SetItemTooltip("Screen pixels per phosphor: raise it on very large or high-DPI screens.");

        ImGui::SeparatorText("Screen shape");
        ImGui::SliderFloat("Curvature across", &c.curvature_x, 0.0f, 0.125f, "%.3f");
        done();
        ImGui::SliderFloat("Curvature down", &c.curvature_y, 0.0f, 0.125f, "%.3f");
        done();

        ImGui::Spacing();
        if (ImGui::Button("Reset to defaults")) {
            const bool on = c.enabled;
            c = CrtSettings();
            c.enabled = on;
            save = true;
        }
    }
    ImGui::End();
    return save;
}

}  // namespace

void menu_init(SDL_Window* window, SDL_GLContext gl_context)
{
    g_menu_window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                       // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    const float scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
    style.FontSizeBase = 18.0f;
    style.ScaleAllSizes(scale > 0 ? scale : 1.0f);
    style.FontScaleDpi = scale > 0 ? scale : 1.0f;

    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
#ifdef __ANDROID__
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 330 core");
#endif
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);   // any connected pad
}

void menu_shutdown()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

bool menu_event(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (!g_open)
        return false;
    const ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // B in the CRT settings window closes it; with no menu open, there is nothing left
        // to go back from, so B closes the menu. (With a menu open, ImGui closes that menu.)
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST && crt_window_focused()) {
            g_crt_window = false;
            return true;
        }
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST
            && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            menu_toggle();
            return true;
        }
        return false;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        // The port's own hotkeys always reach it: once a menu has been opened and closed, the
        // bar keeps the keyboard, and F1 couldn't close the menu until a click elsewhere.
#ifdef __ANDROID__
        if (event.key.key == SDLK_ESCAPE)               // a TV remote's Back (tv_remote.cpp)
            return false;
#endif
        if (event.key.key == SDLK_F1 || event.key.key == SDLK_AC_BACK || event.key.key == SDLK_F2 || event.key.key == SDLK_F3
            || event.key.key == SDLK_F4 || event.key.key == SDLK_F11 || event.key.key == SDLK_F12
            || (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))
            return false;
        return io.WantCaptureKeyboard;
    case SDL_EVENT_TEXT_INPUT:
        return io.WantCaptureKeyboard;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return io.WantCaptureMouse;
    default:
        return false;
    }
}

bool menu_open() { return g_open; }

void menu_toggle(bool from_gamepad)
{
    g_open = !g_open;
    g_focus_bar = g_open && from_gamepad;
    if (!g_open) {
        ImGui::ClosePopupsExceptModals();
        ImGui::SetWindowFocus(nullptr);             // drop keyboard focus with the menu
    }
}

void menu_remote_nav(bool on) { g_remote_nav = on; }

bool menu_wants_keyboard() { return g_open && ImGui::GetIO().WantCaptureKeyboard; }

MenuChoice menu_frame(const MenuState& state)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    MenuChoice choice;
    if (g_open && ImGui::BeginMainMenuBar()) {
        ImGuiWindow* bar = ImGui::GetCurrentWindow();
        // Choosing an item closes its menu and ImGui hands focus back to the window focused
        // before the bar, which here is none, so a controller would be left with nothing to
        // move. Put the controller back on the menu it used instead. (Not for the mouse and
        // keyboard: an unfocused bar lets the game have the arrow keys.)
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        const bool refocus = g_focus_bar
            || ((g.NavInputSource == ImGuiInputSource_Gamepad || g_remote_nav) && g.NavWindow != bar && !g_crt_window
                && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
        if (g_focus_bar)
            g_last_menu = 0;
        g_focus_bar = false;
        int index = 0;
        auto begin_menu = [&](const char* label) {
            const bool open = ImGui::BeginMenu(label);
            if (refocus && index == g_last_menu) {
                // Like tapping ImGui's menu-layer button: focus the bar with this menu selected.
                ImGui::FocusWindow(bar);
                ImGui::SetFocusID(ImGui::GetItemID(), bar);
                ImGui::SetNavCursorVisible(true);
            }
            if (open)
                g_last_menu = index;
            index++;
            return open;
        };
        if (begin_menu("Game")) {
            if (ImGui::MenuItem("Reset", "Ctrl+R"))
                choice.action = MenuAction::Reset;
            ImGui::Separator();
            const bool states = !state.states_blocked;
            if (ImGui::MenuItem("Save", "F5", false, states))
                choice.action = MenuAction::SaveState;
            if (states)
                ImGui::SetItemTooltip("Saves the game as it is now to slot %d.", state.save_slot);
            else
                ImGui::SetItemTooltip("%s.", state.states_blocked);
            if (ImGui::MenuItem("Load", "F7", false, states))
                choice.action = MenuAction::LoadState;
            if (states)
                ImGui::SetItemTooltip("Goes back to the game saved in slot %d.", state.save_slot);
            else
                ImGui::SetItemTooltip("%s.", state.states_blocked);
            if (ImGui::BeginMenu("Save Slot")) {
                for (int slot = 1; slot <= 5; slot++)
                {
                    if (ImGui::MenuItem(state.slot_labels[slot - 1].c_str(), nullptr, state.save_slot == slot)) {
                        choice.action = MenuAction::ChooseSaveSlot;
                        choice.save_slot = slot;
                    }
                    // The game as it was saved, a quarter of the window across in a small window
                    // down to a sixth in a large one, so it stays big enough to make out.
                    const SlotPicture& picture = state.slot_pictures[slot - 1];
                    if (picture.texture && picture.width > 0 && ImGui::BeginItemTooltip()) {
                        const ImVec2 screen = ImGui::GetIO().DisplaySize;
                        const float t = std::clamp((screen.x - 1280.0f) / (2560.0f - 1280.0f), 0.0f, 1.0f);
                        float w = screen.x * (0.25f + t * (1.0f / 6.0f - 0.25f));
                        float h = w * picture.height / picture.width;
                        if (h > screen.y * 0.5f)
                            h = screen.y * 0.5f, w = h * picture.width / picture.height;
                        ImGui::Image((ImTextureID)picture.texture, ImVec2(w, h));
                        ImGui::EndTooltip();
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (state.update_tag) {
                char label[96];
                std::snprintf(label, sizeof(label), "Get the Update (%s)...", state.update_tag);
                if (ImGui::MenuItem(label))
                    choice.action = MenuAction::OpenReleases;
                ImGui::SetItemTooltip("Opens the releases page on GitHub in your browser.");
            }
            if (ImGui::MenuItem("Check for Updates", nullptr, state.check_for_updates))
                choice.action = MenuAction::ToggleUpdateCheck;
            ImGui::SetItemTooltip("At start-up, asks GitHub whether a newer release is out (nothing else is sent).");
            ImGui::Separator();
            if (ImGui::MenuItem("Exit to desktop", "Alt+F4"))
                choice.action = MenuAction::Exit;
            ImGui::EndMenu();
        }
        if (begin_menu("Quick Settings")) {
            static const char* const kNames[] = {"Enhanced", "Vanilla", "Custom"};
            static const char* const kKeys[] = {"F2", "F3", "F4"};
            static const char* const kTips[] = {
                "Widescreen and every graphics enhancement at its default.",
                "The original game: 4:3, with every graphics enhancement off.",
                "Your own choices, as they were before you last picked Enhanced or Vanilla.\n"
                "Changing any of these settings yourself switches to Custom."};
            for (int i = 0; i < 3; i++) {
                if (ImGui::MenuItem(kNames[i], kKeys[i], state.quick == i)) {
                    choice.action = MenuAction::QuickSettings;
                    choice.quick = i;
                }
                ImGui::SetItemTooltip("%s", kTips[i]);
            }
            ImGui::EndMenu();
        }
        if (begin_menu("Video")) {
            if (ImGui::MenuItem("Widescreen", nullptr, state.widescreen, state.widescreen_available))
                choice.action = MenuAction::ToggleWidescreen;
            if (!state.widescreen_available)
                ImGui::SetItemTooltip("The widescreen patches do not match this ROM.");
#ifndef __ANDROID__                                 // always fullscreen on Android
            if (ImGui::MenuItem("Fullscreen", "F11", state.fullscreen))
                choice.action = MenuAction::ToggleFullscreen;
#endif
            if (ImGui::MenuItem("V-Sync", nullptr, *state.vsync)) {
                *state.vsync = !*state.vsync;
                choice.save_settings = true;
            }
            ImGui::SetItemTooltip("Shows each picture in step with the display's refresh, so it never tears.\n"
                                  "On a 60 Hz display the game runs one frame per refresh.");
            if (ImGui::BeginMenu("Frame Rate")) {
                for (int rate : {0, 60, 120, 240}) {
                    char label[48];
                    if (rate)
                        std::snprintf(label, sizeof(label), "%d", rate);
                    else
                        std::snprintf(label, sizeof(label), "Display (%.0f Hz)", state.refresh);
                    if (ImGui::MenuItem(label, nullptr, *state.frame_rate == rate)) {
                        *state.frame_rate = rate;
                        choice.save_settings = true;
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::SetItemTooltip("Display: pictures follow the display's refresh.\n"
                                  "60, 120 or 240: pictures are timed at that rate whatever the display reports,\n"
                                  "for variable-refresh (G-Sync, FreeSync) displays. Above the display's maximum\n"
                                  "refresh rate not every picture can be shown.");
            if (ImGui::MenuItem("High Refresh Interpolation", nullptr, *state.interpolate)) {
                *state.interpolate = !*state.interpolate;
                choice.save_settings = true;
            }
            ImGui::SetItemTooltip("At 120 or 240 pictures a second (Frame Rate, or a display at 120, 180 or 240 Hz\n"
                                  "with V-Sync), each game frame is shown as evenly spaced moments of its movement\n"
                                  "instead of repeats. The picture is up to one game frame (17 ms) behind.");
            if (ImGui::MenuItem("Show FPS", nullptr, *state.show_fps)) {
                *state.show_fps = !*state.show_fps;
                choice.save_settings = true;
            }
            ImGui::EndMenu();
        }
        if (begin_menu("Enhancements")) {
            if (ImGui::BeginMenu("Gameplay")) {
                if (ImGui::MenuItem("Easy Bounce Attack", nullptr, state.easy_bounce))
                    choice.action = MenuAction::ToggleEasyBounce;
                ImGui::SetItemTooltip("Every jump turns into the bounce attack, without pressing Down.");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Audio")) {
                if (ImGui::MenuItem("MD+ Soundtrack", nullptr, state.mdplus, state.mdplus_pack != nullptr))
                    choice.action = MenuAction::ToggleMdPlus;
                if (state.mdplus_pack)
                    ImGui::SetItemTooltip("Plays the music from the MD+ pack in mdplus/:\n%s\n"
                                          "Switching restarts the game.", state.mdplus_pack);
#ifdef __ANDROID__
                if (ImGui::MenuItem("Import MD+ Pack..."))
                    choice.action = MenuAction::ImportMdPlus;
                ImGui::SetItemTooltip("Choose the folder that holds an MD+ pack (its .cue, .md and WAV files):\n"
                                      "the app keeps a copy and plays its soundtrack instead of the game's music.");
#else
                else
                    ImGui::SetItemTooltip("Put an MD+ pack (its .cue, .md and WAV files) in a folder named mdplus\n"
                                          "beside rom/ to play its soundtrack instead of the game's music.");
#endif
                ImGui::Separator();
                if (ImGui::MenuItem("Clear FM", nullptr, *state.clear_fm)) {
                    *state.clear_fm = !*state.clear_fm;
                    choice.save_settings = true;
                }
                ImGui::SetItemTooltip("The FM sound chip without the \"ladder effect\", the distortion of the first\n"
                                      "consoles' YM2612 that adds a buzzy hiss to quiet notes and fades. It sounds\n"
                                      "as the later consoles' chip does. Off: as the first consoles.");
                if (ImGui::MenuItem("Clean PSG", nullptr, *state.clean_psg)) {
                    *state.clean_psg = !*state.clean_psg;
                    choice.save_settings = true;
                }
                ImGui::SetItemTooltip("The square-wave chip (high notes, drums and noise) without the faint,\n"
                                      "out-of-tune overtones the port's simpler mixing let through.");
                if (ImGui::MenuItem("Model 1 Low-Pass", nullptr, *state.low_pass)) {
                    *state.low_pass = !*state.low_pass;
                    choice.save_settings = true;
                }
                ImGui::SetItemTooltip("The authentic Genesis sound: the output filter of the first Mega Drive /\n"
                                      "Genesis (Model 1), which softens the treble for a warmer, a bit more muddy\n"
                                      "sound, as through the console's own audio out. Off: the full, clear sound.");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Graphics")) {
                if (ImGui::BeginMenu("Enhancements")) {
                    if (ImGui::MenuItem("All On")) {
                        choice.action = MenuAction::AllEnhancements;
                        choice.all_on = true;
                    }
                    ImGui::SetItemTooltip("Switches on every enhancement in this menu.");
                    if (ImGui::MenuItem("All Off")) {
                        choice.action = MenuAction::AllEnhancements;
                        choice.all_on = false;
                    }
                    ImGui::SetItemTooltip("Switches off every enhancement in this menu: the picture as in the original.");
                    ImGui::Separator();
                    if (ImGui::MenuItem("Smooth Swinging", nullptr, state.smooth_swing, state.smooth_swing_available))
                        choice.action = MenuAction::ToggleSmoothSwing;
                    ImGui::SetItemTooltip(state.smooth_swing_available
                        ? "Vines and ropes swing at 60 frames a second instead of 10."
                        : "The swing patches do not match this ROM.");
#ifdef __ANDROID__
                    // Off with cleanEdge on a phone (main.cpp): shown unticked and greyed out.
                    const bool scroll_blocked = *state.hqx == kCleanEdge && !state.crt->enabled;
#else
                    const bool scroll_blocked = false;
#endif
                    if (ImGui::MenuItem("Smooth Scrolling", nullptr, state.smooth_scroll && !scroll_blocked, !scroll_blocked))
                        choice.action = MenuAction::ToggleSmoothScroll;
                    ImGui::SetItemTooltip(scroll_blocked
                        ? "Not available with cleanEdge on a phone: it would be too much for the GPU."
                        : "Scrolling moves in even steps, using fractions of a pixel, instead of the\n"
                          "game's uneven whole-pixel steps.");
                    if (ImGui::MenuItem("Clean Rock Edges", nullptr, *state.clean_rock_edges)) {
                        *state.clean_rock_edges = !*state.clean_rock_edges;
                        choice.save_settings = true;
                    }
                    ImGui::SetItemTooltip("In the forest, the black gaps between stones no longer show as black\n"
                                          "notches where the rock walls meet the background. Off: as in the original.");
                    if (ImGui::MenuItem("Smooth Fades", nullptr, *state.smooth_fades)) {
                        *state.smooth_fades = !*state.smooth_fades;
                        choice.save_settings = true;
                    }
                    ImGui::SetItemTooltip("Fades in and out between screens change colour smoothly instead of in\n"
                                          "the game's few steps. Off: as in the original.");
                    if (ImGui::MenuItem("Smooth Water", nullptr, *state.smooth_water)) {
                        *state.smooth_water = !*state.smooth_water;
                        choice.save_settings = true;
                    }
                    ImGui::SetItemTooltip("Water the game draws with dithering (the tea, the flooded castle, the\n"
                                          "waterfalls and their foam, Toyland's jello) is see-through instead, and\n"
                                          "flowing water moves smoothly. Off: as in the original.");
                    if (ImGui::MenuItem("Smooth Glass", nullptr, *state.smooth_glass)) {
                        *state.smooth_glass = !*state.smooth_glass;
                        choice.save_settings = true;
                    }
                    ImGui::SetItemTooltip("The library's glass jars, which the game draws with dithering, are\n"
                                          "see-through instead. Off: as in the original.");
                    if (ImGui::MenuItem("Remove Sprite Limit", nullptr, *state.remove_sprite_limit)) {
                        *state.remove_sprite_limit = !*state.remove_sprite_limit;
                        choice.save_settings = true;
                    }
                    ImGui::SetItemTooltip("The console shows at most 20 sprites on a line and drops the rest, so busy\n"
                                          "lines lose parts of sprites. With this on, every sprite is drawn.");
#ifndef __ANDROID__
                    if (ImGui::MenuItem("Co-op Dynamic Camera", nullptr, *state.coop_camera)) {   // (no co-op on Android yet)
                        *state.coop_camera = !*state.coop_camera;
                        choice.save_settings = true;
                    }
                    ImGui::SetItemTooltip("Two-player co-op: the picture zooms in a little while the players are close\n"
                                          "together (more once both have stood still a moment; not in boss stages),\n"
                                          "and out by a tenth while they are far apart. Only the picture changes: the\n"
                                          "game, its camera and the HUD are as they are.");
#endif
                    const bool animation_ok = state.gpu_renderer && state.gpu_available;
                    if (ImGui::MenuItem("Smooth Animation", nullptr, *state.smooth_animation && animation_ok, animation_ok)) {
                        *state.smooth_animation = !*state.smooth_animation;
                        choice.save_settings = true;
                    }
                    ImGui::SetItemTooltip(animation_ok
                        ? "Many animations throughout the game are smoother.\n"
                          "See the project's documentation (Settings) for the full list."
                        : "Drawn by the GPU renderer: switch it on in the Debug menu.");
                    if (ImGui::MenuItem("Weather - Stage 4", nullptr, *state.stage4_storm && animation_ok, animation_ok)) {
                        *state.stage4_storm = !*state.stage4_storm;
                        choice.save_settings = true;
                    }
                    ImGui::SetItemTooltip(animation_ok
                        ? "In stage 4, about ten seconds after night falls, it starts to rain, with\n"
                          "lightning now and then and thunder after it. No rain under the rock.\n"
                          "Only the picture and the sound change: the game plays as it does."
                        : "Drawn by the GPU renderer: switch it on in the Debug menu.");
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("Upscaler", !state.crt->enabled)) {
#ifdef __ANDROID__
                    for (int scale : {1, 2}) {              // a phone's GPU can't keep up with hq3x or hq4x
#else
                    for (int scale : {1, 2, 3, 4}) {
#endif
                        char label[8];
                        std::snprintf(label, sizeof(label), scale > 1 ? "HQ%dx" : "Off", scale);
                        if (ImGui::MenuItem(label, nullptr, *state.hqx == scale)) {
                            *state.hqx = scale;
                            choice.save_settings = true;
                        }
                        if (scale == 2)
                            ImGui::SetItemTooltip("Recommended to activate 25%% scanlines when using this mode.");
                    }
                    if (ImGui::MenuItem("cleanEdge", nullptr, *state.hqx == kCleanEdge)) {
                        *state.hqx = kCleanEdge;
                        choice.save_settings = true;
                    }
                    ImGui::SetItemTooltip("torcado's cleanEdge: keeps the pixel art's hard edges and redraws\n"
                                          "its diagonals and slopes as clean lines, at the window's size.");
                    ImGui::BeginDisabled(*state.hqx != kCleanEdge);
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
                    ImGui::SliderFloat("Line Width", state.clean_edge_line_width, 0.45f, 1.14f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                    if (ImGui::IsItemDeactivatedAfterEdit())
                        choice.save_settings = true;
                    ImGui::SetItemTooltip("cleanEdge only. Lower: thinner diagonal lines, closer to the original\n"
                                          "pixels. Higher: bolder lines and rounder corners. Default 1.00.\n"
                                          "Ctrl+click to type a value.");
                    ImGui::EndDisabled();
                    ImGui::EndMenu();
                }
#ifdef __ANDROID__
                const char* upscaler_tip = "hqx: smooths the pixel art's edges and diagonals at twice the size.";
#else
                const char* upscaler_tip = "hqx: smooths the pixel art's edges and diagonals at 2, 3 or 4 times the size.";
#endif
                ImGui::SetItemTooltip("%s", state.crt->enabled ? "Turn the CRT shader off to use hqx." : upscaler_tip);
                if (ImGui::BeginMenu("Scanlines", !state.crt->enabled)) {
                    for (int percent : {0, 25, 50, 75, 100}) {
                        char label[8];
                        std::snprintf(label, sizeof(label), percent ? "%d%%" : "Off", percent);
                        if (ImGui::MenuItem(label, nullptr, *state.scanlines == percent)) {
                            *state.scanlines = percent;
                            choice.save_settings = true;
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::SetItemTooltip(state.crt->enabled ? "Turn the CRT shader off to use simple scanlines."
                                                         : "Dark lines between the game's lines, without the CRT shader's other effects.");
                if (ImGui::MenuItem("CRT Shader", "F12", state.crt->enabled)) {
                    state.crt->enabled = !state.crt->enabled;
                    choice.save_settings = true;
                }
                ImGui::SetItemTooltip("Scanlines, a phosphor mask and glow, like a CRT television.");
                if (ImGui::MenuItem("CRT Shader Settings...")) {
                    g_crt_window = true;
                    g_focus_crt_window = true;
                }
                // Texture Packs: on or off, and which of the packs in texture_packs/ to draw with.
                ImGui::SeparatorText("Texture Packs (Experimental)");
                const std::vector<std::string>& packs = *state.texture_pack_names;
                if (ImGui::MenuItem("Use a Texture Pack", nullptr, state.texture_packs))
                    choice.action = MenuAction::ToggleTexturePacks;
                if (state.texture_pack_blocked)
                    ImGui::SetItemTooltip("%s", state.texture_pack_blocked);
                else
                    ImGui::SetItemTooltip("Draws the game with redrawn art from the pack chosen below, at four times the\n"
                                          "size. Each pack is a folder in a folder named texture_packs beside rom/.\n"
                                          "Experimental: still being worked on, so you may run into bugs.");
                if (packs.empty()) {
                    ImGui::TextDisabled("No packs in texture_packs/");
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip("Put each pack in its own folder in a folder named texture_packs beside rom/\n"
                                          "(texture_packs/<pack name>/, laid out as coi-art-dump's art-dump folder).\n"
                                          "Packs added while the game runs show after switching Use a Texture Pack off and on.");
                }
                ImGui::BeginDisabled(!state.texture_packs);
                for (int i = 0; i < (int)packs.size(); i++) {
                    const bool loading = state.texture_pack_loading && packs[i] == state.texture_pack_loading;
                    if (ImGui::MenuItem(packs[i].c_str(), loading ? "loading..." : nullptr, i == state.texture_pack)
                        && i != state.texture_pack) {
                        choice.action = MenuAction::ChooseTexturePack;
                        choice.texture_pack = i;
                    }
                }
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (begin_menu("Cheats")) {
            Cheats& c = *state.cheats;
            auto item = [&](const char* label, bool& on, const char* tip) {
                if (ImGui::MenuItem(label, nullptr, on)) {
                    on = !on;
                    choice.cheats_changed = true;
                }
                ImGui::SetItemTooltip("%s", tip);
            };
            item("Invincibility", c.invincibility, "Enemies and hazards don't hurt Mickey. Falling off the stage still costs a try.");
            item("Infinite Power", c.infinite_power, "Hits knock Mickey back but take no power.");
            item("Infinite Tries", c.infinite_tries, "Losing a life, falls included, costs no try.");
            item("Infinite Items", c.infinite_items, "Throwing doesn't use up items.");
            item("Infinite Continues", c.infinite_continues, "Continuing doesn't use up continues.");
            item("Start with Full Power", c.full_power, "Every life starts with all five power stars.");
            ImGui::Separator();
            item("Walk Faster", c.walk_faster, "Mickey walks at a steady 2.3 pixels a frame instead of 1.4.");
            const bool moon = c.moon_jump, midair = c.midair_jump;
            item("Moon Jump", c.moon_jump, "Holding jump keeps Mickey rising. Switches Jump in Midair off.");
            item("Jump in Midair", c.midair_jump, "Each press of jump in the air starts a new jump, held for height like a\nnormal one. Switches Moon Jump off.");
            if (c.moon_jump && !moon)
                c.midair_jump = false;              // the two patch the same code
            if (c.midair_jump && !midair)
                c.moon_jump = false;
            ImGui::EndMenu();
        }
        // Debug stays the last menu.
        if (begin_menu("Debug")) {
            if (ImGui::BeginMenu("Go to stage")) {
                // Stages in play order, by level; each level ends with its boss. Choosing one
                // starts a new game at that stage.
                for (const Level& level : kLevels) {
                    char title[64];
                    if (level.first == level.last)
                        std::snprintf(title, sizeof(title), "%s (stage %d)", level.name, level.first);
                    else
                        std::snprintf(title, sizeof(title), "%s (stages %d-%d)", level.name, level.first, level.last);
                    if (!ImGui::BeginMenu(title))
                        continue;
                    for (int stage = level.first; stage <= level.last; stage++) {
                        char label[32];
                        std::snprintf(label, sizeof(label), stage == level.last ? "Stage %d (boss)" : "Stage %d", stage);
                        if (ImGui::MenuItem(label, nullptr, stage == state.stage)) {
                            choice.action = MenuAction::GoToStage;
                            choice.stage = stage;
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
#ifndef __ANDROID__                                 // the app's storage can't be opened on Android
            ImGui::Separator();
            if (ImGui::MenuItem("Open Settings Folder"))
                choice.action = MenuAction::OpenSettingsFolder;
            ImGui::SetItemTooltip("Opens the folder that holds settings.ini.");
#endif
            ImGui::Separator();
            if (ImGui::MenuItem("Show Coordinates", nullptr, *state.show_coordinates)) {
                *state.show_coordinates = !*state.show_coordinates;
                choice.save_settings = true;
            }
            ImGui::SetItemTooltip("Shows the stage, Mickey's position and the camera's in the top right\n"
                                  "(below the FPS counter when that is on), for reporting where something happened.");
            // On unless it gives trouble: here rather than in Graphics, so players switch it off
            // only when the picture looks wrong or the game runs badly with it.
            ImGui::Separator();
            if (ImGui::MenuItem("GPU Renderer", nullptr, state.gpu_renderer && state.gpu_available, state.gpu_available))
                choice.action = MenuAction::ToggleGpuRenderer;
            ImGui::SetItemTooltip(state.gpu_available
                ? "The picture is drawn by the graphics card, which takes most of the drawing\n"
                  "off the processor; it looks exactly the same. Leave it on: switch it off only if\n"
                  "the picture looks wrong or the game runs badly, and the processor draws it."
                : "Not available: the graphics card could not start it, so the processor draws\n"
                  "the picture.");
            ImGui::EndMenu();
        }
        // At the right, inside the safe area (clear of a phone's camera cutout): with the touch
        // controls showing, an X to tap (Back is an edge swipe away); otherwise how to close.
        float right = ImGui::GetWindowWidth();
        SDL_Rect safe;
        if (SDL_GetWindowSafeArea(g_menu_window, &safe) && safe.w > 0)
            right = std::min(right, (float)(safe.x + safe.w));
        const float spacing = ImGui::GetStyle().ItemSpacing.x * 2;
        if (touch_shown()) {
            const ImVec2 size(ImGui::GetFrameHeight() * 2.5f, ImGui::GetFrameHeight());
            ImGui::SameLine(right - size.x - spacing);
            if (ImGui::Button("X##close", size))
                menu_toggle();
        } else {
#ifdef __ANDROID__
            const char* hint = "Back to close";
#else
            const char* hint = "F1 to close";
#endif
            ImGui::SameLine(right - ImGui::CalcTextSize(hint).x - spacing);
            ImGui::TextDisabled("%s", hint);
        }
        ImGui::EndMainMenuBar();
    }
    if (g_open && g_crt_window && crt_settings_window(*state.crt))
        choice.save_settings = true;
    // Top right, below the menu bar when it is open: the status line (menu_status), then the
    // frame rate and the coordinates below it.
    float status_bottom = 0.0f;
    if (!g_status.empty() && g_status_alpha > 0.0f) {
        const ImGuiViewport* view = ImGui::GetMainViewport();
        float right = view->Pos.x + view->Size.x - 8.0f, top = view->Pos.y + (g_open ? ImGui::GetFrameHeight() : 0.0f) + 8.0f;
        SDL_Rect safe;                              // clear of a phone's camera cut-out
        if (SDL_GetWindowSafeArea(g_menu_window, &safe) && safe.w > 0) {
            right = std::min(right, (float)(safe.x + safe.w) - 8.0f);
            top = std::max(top, (float)safe.y + 8.0f);
        }
        const float alpha = std::min(1.0f, g_status_alpha);
        ImGui::SetNextWindowPos(ImVec2(right, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.7f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                     | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
                                     | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("##status", nullptr, flags)) {
            ImGui::TextUnformatted(g_status.c_str());
            status_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
    g_status.clear();                               // set again every frame while it shows
    // Top left, below the menu bar when it is open: menu_status_left's line.
    if (!g_status_left.empty() && g_status_left_alpha > 0.0f) {
        const ImGuiViewport* view = ImGui::GetMainViewport();
        float left = view->Pos.x + 8.0f, top = view->Pos.y + (g_open ? ImGui::GetFrameHeight() : 0.0f) + 8.0f;
        SDL_Rect safe;                              // clear of a phone's camera cut-out
        if (SDL_GetWindowSafeArea(g_menu_window, &safe) && safe.w > 0) {
            left = std::max(left, (float)safe.x + 8.0f);
            top = std::max(top, (float)safe.y + 8.0f);
        }
        const float alpha = std::min(1.0f, g_status_left_alpha);
        ImGui::SetNextWindowPos(ImVec2(left, top), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.7f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                     | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
                                     | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("##status_left", nullptr, flags))
            ImGui::TextUnformatted(g_status_left.c_str());
        ImGui::End();
        ImGui::PopStyleVar();
    }
    g_status_left.clear();
    if (*state.show_fps || *state.show_coordinates) {
        const ImGuiViewport* view = ImGui::GetMainViewport();
        const float top = std::max(view->Pos.y + (g_open ? ImGui::GetFrameHeight() : 0.0f) + 6.0f, status_bottom + 4.0f);
        ImGui::SetNextWindowPos(ImVec2(view->Pos.x + view->Size.x - 6.0f, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.5f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                     | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
                                     | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("##fps", nullptr, flags)) {
            if (*state.show_fps) {
                ImGui::Text("%.0f fps", state.fps);
                if (state.game_fps > 0.0f && std::fabs(state.game_fps - state.fps) > 0.5f)
                    ImGui::Text("game %.1f", state.game_fps);
            }
            if (*state.show_coordinates) {
                if (state.stage)
                    ImGui::Text("stage %d (index %d)", state.stage, state.stage_index);
                else
                    ImGui::Text("state $%02X", state.game_state);
                ImGui::Text("Mickey %d, %d", state.mickey_x, state.mickey_y);
                ImGui::Text("camera %d, %d", state.camera_x, state.camera_y);
            }
        }
        ImGui::End();
    }
    if (state.texture_pack_loading) {
        // A texture pack being read: its name and a progress bar at the top centre.
        const ImGuiViewport* view = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(view->Pos.x + view->Size.x / 2, view->Pos.y + ImGui::GetFontSize() * 3),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.85f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                     | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
                                     | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("##texture_pack", nullptr, flags)) {
            ImGui::Text("Loading texture pack: %s", state.texture_pack_loading);
            ImGui::ProgressBar(state.texture_pack_progress, ImVec2(ImGui::GetFontSize() * 20, 0));
        }
        ImGui::End();
    }
    if (state.busy) {
        // Centred over the game (or in the bottom left corner), with a progress bar when there
        // is progress to show.
        const ImGuiViewport* view = ImGui::GetMainViewport();
        const float margin = ImGui::GetFontSize();
        if (state.busy_corner)
            ImGui::SetNextWindowPos(ImVec2(view->WorkPos.x + margin, view->WorkPos.y + view->WorkSize.y - margin),
                                    ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        else
            ImGui::SetNextWindowPos(ImVec2(view->Pos.x + view->Size.x / 2, view->Pos.y + view->Size.y / 2),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                     | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
                                     | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("##busy", nullptr, flags)) {
            ImGui::TextUnformatted(state.busy);
            if (state.busy_progress >= 0)       // (in the corner: as wide as the text, no percentage)
                ImGui::ProgressBar(state.busy_progress, ImVec2(state.busy_corner ? -FLT_MIN : ImGui::GetFontSize() * 16, 0),
                                   state.busy_corner ? "" : nullptr);
        }
        ImGui::End();
    }
    draw_hint();
    draw_notices();
    if (!g_open)
        touch_draw();
    ImGui::Render();
    return choice;
}

void menu_hint(const char* text, double seconds)
{
    g_hint = {text, SDL_GetTicks() + (Uint64)(seconds * 1000)};
}

void menu_hint_text(const char* text)
{
    g_hint.text = text;
}

void menu_status(const char* text, float alpha)
{
    g_status = text;
    g_status_alpha = alpha;
}

void menu_status_left(const char* text, float alpha)
{
    g_status_left = text;
    g_status_left_alpha = alpha;
}

void menu_notice(const char* text, double seconds)
{
    g_notices.push_back({text, SDL_GetTicks() + (Uint64)(seconds * 1000)});
}

void menu_render()
{
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
