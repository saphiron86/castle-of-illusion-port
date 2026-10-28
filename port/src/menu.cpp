#include "menu.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "stages.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"

namespace {

bool g_open = false;
bool g_focus_bar = false;                           // put the nav cursor on the first menu next frame
int g_last_menu = 0;                                // index of the top-level menu last opened
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
    ImGui_ImplOpenGL3_Init("#version 330 core");
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
        if (event.key.key == SDLK_F1 || event.key.key == SDLK_F2 || event.key.key == SDLK_F3
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
            || (g.NavInputSource == ImGuiInputSource_Gamepad && g.NavWindow != bar && !g_crt_window
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
            if (ImGui::MenuItem("Fullscreen", "F11", state.fullscreen))
                choice.action = MenuAction::ToggleFullscreen;
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
                else
                    ImGui::SetItemTooltip("Put an MD+ pack (its .cue, .md and WAV files) in a folder named mdplus\n"
                                          "beside rom/ to play its soundtrack instead of the game's music.");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Graphics")) {
                if (ImGui::MenuItem("Smooth Swinging", nullptr, state.smooth_swing, state.smooth_swing_available))
                    choice.action = MenuAction::ToggleSmoothSwing;
                ImGui::SetItemTooltip(state.smooth_swing_available
                    ? "Vines and ropes swing at 60 frames a second instead of 10."
                    : "The swing patches do not match this ROM.");
                if (ImGui::MenuItem("Smooth Scrolling", nullptr, state.smooth_scroll))
                    choice.action = MenuAction::ToggleSmoothScroll;
                ImGui::SetItemTooltip("Scrolling moves in even steps, using fractions of a pixel, instead of the\n"
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
                if (ImGui::MenuItem("Remove Sprite Limit", nullptr, *state.remove_sprite_limit)) {
                    *state.remove_sprite_limit = !*state.remove_sprite_limit;
                    choice.save_settings = true;
                }
                ImGui::SetItemTooltip("The console shows at most 20 sprites on a line and drops the rest, so busy\n"
                                      "lines lose parts of sprites. With this on, every sprite is drawn.");
                ImGui::Separator();
                if (ImGui::BeginMenu("Upscaler", !state.crt->enabled)) {
                    for (int scale : {1, 2, 3, 4}) {
                        char label[8];
                        std::snprintf(label, sizeof(label), scale > 1 ? "HQ%dx" : "Off", scale);
                        if (ImGui::MenuItem(label, nullptr, *state.hqx == scale)) {
                            *state.hqx = scale;
                            choice.save_settings = true;
                        }
                        if (scale == 2)
                            ImGui::SetItemTooltip("Recommended to activate 25%% scanlines when using this mode.");
                    }
                    ImGui::EndMenu();
                }
                ImGui::SetItemTooltip(state.crt->enabled ? "Turn the CRT shader off to use hqx."
                                                         : "hqx: smooths the pixel art's edges and diagonals at 2, 3 or 4 times the size.");
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
            ImGui::Separator();
            if (ImGui::MenuItem("Open Settings Folder"))
                choice.action = MenuAction::OpenSettingsFolder;
            ImGui::SetItemTooltip("Opens the folder that holds settings.ini.");
            ImGui::EndMenu();
        }
        const char* hint = "F1 to close";
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(hint).x - ImGui::GetStyle().ItemSpacing.x * 2);
        ImGui::TextDisabled("%s", hint);
        ImGui::EndMainMenuBar();
    }
    if (g_open && g_crt_window && crt_settings_window(*state.crt))
        choice.save_settings = true;
    if (*state.show_fps) {
        // Top right, below the menu bar when it is open.
        const ImGuiViewport* view = ImGui::GetMainViewport();
        const float top = view->Pos.y + (g_open ? ImGui::GetFrameHeight() : 0.0f) + 6.0f;
        ImGui::SetNextWindowPos(ImVec2(view->Pos.x + view->Size.x - 6.0f, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.5f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                     | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
                                     | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("##fps", nullptr, flags)) {
            ImGui::Text("%.0f fps", state.fps);
            if (state.game_fps > 0.0f && std::fabs(state.game_fps - state.fps) > 0.5f)
                ImGui::Text("game %.1f", state.game_fps);
        }
        ImGui::End();
    }
    ImGui::Render();
    return choice;
}

void menu_render()
{
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
