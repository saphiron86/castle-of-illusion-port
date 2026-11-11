#include "rom_family.h"

#include <cstdio>

#include "boss_rush.h"
#include "patches.h"
#include "smooth_swing.h"

std::vector<uint8_t> make_rom_image(const std::vector<uint8_t>& original, const RomSettings& s, bool report)
{
    std::vector<uint8_t> rom = original;
    if (s.widescreen)
        apply_widescreen_patches(rom);
    if (s.smooth_swing)
        apply_smooth_swing(rom);
    const std::string rush = apply_boss_rush(rom);
    if (report && !rush.empty())
        std::fprintf(stderr, "%s\n", rush.c_str());
    const std::string skipped = apply_cheats(rom, s.cheats);
    if (report && !skipped.empty())
        std::fprintf(stderr, "%s\n", skipped.c_str());
    fix_checksum(rom);
    return rom;
}

const RomSetting kRomSettings[] = {
    {"widescreen", [](RomSettings& s) -> bool& { return s.widescreen; }},
    {"smooth-swing", [](RomSettings& s) -> bool& { return s.smooth_swing; }},
    {"invincibility", [](RomSettings& s) -> bool& { return s.cheats.invincibility; }},
    {"infinite-power", [](RomSettings& s) -> bool& { return s.cheats.infinite_power; }},
    {"infinite-tries", [](RomSettings& s) -> bool& { return s.cheats.infinite_tries; }},
    {"infinite-items", [](RomSettings& s) -> bool& { return s.cheats.infinite_items; }},
    {"infinite-continues", [](RomSettings& s) -> bool& { return s.cheats.infinite_continues; }},
    {"full-power", [](RomSettings& s) -> bool& { return s.cheats.full_power; }},
    {"walk-faster", [](RomSettings& s) -> bool& { return s.cheats.walk_faster; }},
    {"moon-jump", [](RomSettings& s) -> bool& { return s.cheats.moon_jump; }},
    {"midair-jump", [](RomSettings& s) -> bool& { return s.cheats.midair_jump; }},
};
const size_t kRomSettingCount = sizeof(kRomSettings) / sizeof(kRomSettings[0]);

uint32_t rom_settings_mask(RomSettings s)
{
    uint32_t mask = 0;
    for (size_t k = 0; k < kRomSettingCount; k++)
        if (kRomSettings[k].field(s)) mask |= 1u << k;
    return mask;
}

bool save_rom_family(const std::vector<uint8_t>& original, const std::string& dir)
{
    bool ok = true;
    auto save = [&](const std::string& name, const RomSettings& s) {
        const std::vector<uint8_t> rom = make_rom_image(original, s, false);
        FILE* f = std::fopen((dir + "/" + name + ".bin").c_str(), "wb");
        ok = ok && f && std::fwrite(rom.data(), 1, rom.size(), f) == rom.size();
        if (f) std::fclose(f);
    };
    save("base", RomSettings{});
    std::string names;
    for (size_t k = 0; k < kRomSettingCount; k++) {
        RomSettings s{};
        kRomSettings[k].field(s) = true;
        save(kRomSettings[k].name, s);
        names += std::string(kRomSettings[k].name) + "\n";
    }
    FILE* f = std::fopen((dir + "/family.txt").c_str(), "w");
    ok = ok && f && std::fputs(names.c_str(), f) >= 0;
    if (f) std::fclose(f);
    return ok;
}
