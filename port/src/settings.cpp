#include "settings.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>

namespace {

// One "key = value" line per setting: true/false for switches, numbers for the rest;
// lines starting with '#' are comments.
struct Entry {
    const char* key;
    std::function<std::string(const Settings&)> get;
    std::function<void(Settings&, const std::string&)> set;   // ignores values it can't read
};

template <typename Field>
Entry entry(const char* key, Field field)
{
    return {key,
            [field](const Settings& s) {
                const auto& v = field(const_cast<Settings&>(s));
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, bool>)
                    return std::string(v ? "true" : "false");
                else if constexpr (std::is_same_v<T, std::string>)
                    return v;
                else
                    return std::to_string(v);
            },
            [field](Settings& s, const std::string& text) {
                auto& v = field(s);
                using T = std::decay_t<decltype(v)>;
                char* end = nullptr;
                if constexpr (std::is_same_v<T, bool>) {
                    if (text == "true" || text == "false")
                        v = text == "true";
                } else if constexpr (std::is_same_v<T, std::string>) {
                    v = text;
                } else if constexpr (std::is_same_v<T, int>) {
                    long n = std::strtol(text.c_str(), &end, 10);
                    if (end != text.c_str() && *end == '\0')
                        v = (int)n;
                } else {
                    float f = std::strtof(text.c_str(), &end);
                    if (end != text.c_str() && *end == '\0')
                        v = f;
                }
            }};
}

#define FIELD(member) [](Settings& s) -> auto& { return s.member; }

const Entry kEntries[] = {
    entry("widescreen", FIELD(widescreen)),
    entry("fullscreen", FIELD(fullscreen)),
    entry("easy_bounce_attack", FIELD(easy_bounce)),
    entry("smooth_swinging", FIELD(smooth_swing)),
    entry("smooth_scrolling", FIELD(smooth_scroll)),
    entry("scanlines", FIELD(scanlines)),
    entry("hqx", FIELD(hqx)),
    entry("clean_edge_line_width", FIELD(clean_edge_line_width)),
    entry("clean_rock_edges", FIELD(clean_rock_edges)),
    entry("smooth_fades", FIELD(smooth_fades)),
    entry("smooth_water", FIELD(smooth_water)),
    entry("smooth_glass", FIELD(smooth_glass)),
    entry("remove_sprite_limit", FIELD(remove_sprite_limit)),
    entry("smooth_animation", FIELD(smooth_animation)),
    entry("stage4_storm", FIELD(stage4_storm)),
    entry("vsync", FIELD(vsync)),
    entry("show_fps", FIELD(show_fps)),
    entry("show_coordinates", FIELD(show_coordinates)),
    entry("check_for_updates", FIELD(check_for_updates)),
    entry("high_refresh_interpolation", FIELD(interpolate)),
    entry("frame_rate", FIELD(frame_rate)),
    entry("coop_camera", FIELD(coop_camera)),
    entry("gpu_renderer", FIELD(gpu_renderer)),
    entry("mdplus_soundtrack", FIELD(mdplus)),
    entry("clear_fm", FIELD(clear_fm)),
    entry("clean_psg", FIELD(clean_psg)),
    entry("model1_low_pass", FIELD(low_pass)),
    entry("texture_packs", FIELD(texture_packs)),
    entry("texture_pack", FIELD(texture_pack)),
    entry("save_slot", FIELD(save_slot)),
    entry("tv_remote_help_shown", FIELD(tv_remote_help_shown)),
    entry("cheat_invincibility", FIELD(cheats.invincibility)),
    entry("cheat_infinite_power", FIELD(cheats.infinite_power)),
    entry("cheat_infinite_tries", FIELD(cheats.infinite_tries)),
    entry("cheat_infinite_items", FIELD(cheats.infinite_items)),
    entry("cheat_infinite_continues", FIELD(cheats.infinite_continues)),
    entry("cheat_full_power", FIELD(cheats.full_power)),
    entry("cheat_walk_faster", FIELD(cheats.walk_faster)),
    entry("cheat_moon_jump", FIELD(cheats.moon_jump)),
    entry("cheat_midair_jump", FIELD(cheats.midair_jump)),
    entry("crt_shader", FIELD(crt.enabled)),
    entry("crt_scanline_sharpness", FIELD(crt.scanline_sharpness)),
    entry("crt_pixel_sharpness", FIELD(crt.pixel_sharpness)),
    entry("crt_mask_type", FIELD(crt.mask_type)),
    entry("crt_mask_dark", FIELD(crt.mask_dark)),
    entry("crt_mask_light", FIELD(crt.mask_light)),
    entry("crt_mask_size", FIELD(crt.mask_size)),
    entry("crt_curvature_x", FIELD(crt.curvature_x)),
    entry("crt_curvature_y", FIELD(crt.curvature_y)),
    entry("crt_glow", FIELD(crt.glow)),
    entry("crt_brightness", FIELD(crt.brightness)),
    entry("quick_settings", FIELD(quick)),
    entry("custom_widescreen", FIELD(custom.widescreen)),
    entry("custom_smooth_swinging", FIELD(custom.smooth_swing)),
    entry("custom_smooth_scrolling", FIELD(custom.smooth_scroll)),
    entry("custom_clean_rock_edges", FIELD(custom.clean_rock_edges)),
    entry("custom_smooth_fades", FIELD(custom.smooth_fades)),
    entry("custom_smooth_water", FIELD(custom.smooth_water)),
    entry("custom_smooth_glass", FIELD(custom.smooth_glass)),
    entry("custom_remove_sprite_limit", FIELD(custom.remove_sprite_limit)),
    entry("custom_smooth_animation", FIELD(custom.smooth_animation)),
    entry("custom_hqx", FIELD(custom.hqx)),
    entry("custom_scanlines", FIELD(custom.scanlines)),
    entry("custom_crt_shader", FIELD(custom.crt)),
    entry("custom_clear_fm", FIELD(custom.clear_fm)),
    entry("custom_clean_psg", FIELD(custom.clean_psg)),
    entry("custom_model1_low_pass", FIELD(custom.low_pass)),
    entry("custom_stage4_storm", FIELD(custom.stage4_storm)),
};

std::string settings_path()
{
    char* dir = SDL_GetPrefPath("CastleOfIllusion", "Port");   // created if missing
    if (!dir)
        return "";
    std::string path = std::string(dir) + "settings.ini";
    SDL_free(dir);
    return path;
}

std::string trim(const std::string& s)
{
    size_t first = s.find_first_not_of(" \t\r"), last = s.find_last_not_of(" \t\r");
    return first == std::string::npos ? "" : s.substr(first, last - first + 1);
}

}  // namespace

bool Look::operator==(const Look& o) const
{
    return widescreen == o.widescreen && smooth_swing == o.smooth_swing && smooth_scroll == o.smooth_scroll
        && clean_rock_edges == o.clean_rock_edges && smooth_fades == o.smooth_fades && smooth_water == o.smooth_water
        && smooth_glass == o.smooth_glass
        && remove_sprite_limit == o.remove_sprite_limit && smooth_animation == o.smooth_animation && hqx == o.hqx && scanlines == o.scanlines && crt == o.crt
        && clear_fm == o.clear_fm && clean_psg == o.clean_psg && low_pass == o.low_pass && stage4_storm == o.stage4_storm;
}

Look quick_look(Quick quick)
{
    if (quick == Quick::Vanilla)
        return {false, false, false, false, false, false, false, false, false, 1, 0, false, false, false, false, false};
    return look_of(Settings{});                     // Enhanced: the defaults
}

Look look_of(const Settings& s)
{
    return {s.widescreen, s.smooth_swing, s.smooth_scroll, s.clean_rock_edges, s.smooth_fades, s.smooth_water, s.smooth_glass,
            s.remove_sprite_limit, s.smooth_animation, s.hqx, s.scanlines, s.crt.enabled, s.clear_fm, s.clean_psg, s.low_pass,
            s.stage4_storm};
}

void set_look(Settings& s, const Look& look)
{
    s.widescreen = look.widescreen;
    s.smooth_swing = look.smooth_swing;
    s.smooth_scroll = look.smooth_scroll;
    s.clean_rock_edges = look.clean_rock_edges;
    s.smooth_fades = look.smooth_fades;
    s.smooth_water = look.smooth_water;
    s.smooth_glass = look.smooth_glass;
    s.remove_sprite_limit = look.remove_sprite_limit;
    s.smooth_animation = look.smooth_animation;
    s.hqx = look.hqx;
    s.scanlines = look.scanlines;
    s.crt.enabled = look.crt;
    s.clear_fm = look.clear_fm;
    s.clean_psg = look.clean_psg;
    s.low_pass = look.low_pass;
    s.stage4_storm = look.stage4_storm;
}

std::string settings_folder()
{
    char* dir = SDL_GetPrefPath("CastleOfIllusion", "Port");
    if (!dir)
        return "";
    std::string path = dir;
    SDL_free(dir);
    return path;
}

Settings load_settings()
{
    Settings settings;
    std::ifstream file(settings_path());
    std::string line;
    while (std::getline(file, line)) {
        size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
        for (const Entry& e : kEntries)
            if (key == e.key)
                e.set(settings, value);
    }
    return settings;
}

void save_settings(const Settings& settings)
{
    const std::string path = settings_path();
    std::ofstream file(path);
    if (!file) {
        std::fprintf(stderr, "Could not save the settings to %s\n", path.c_str());
        return;
    }
    file << "# Castle of Illusion port settings, written when they change in the F1 menu\n";
    for (const Entry& e : kEntries)
        file << e.key << " = " << e.get(settings) << "\n";
}
