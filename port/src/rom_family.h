// The ROM images the port's settings make (widescreen, smooth swinging, Boss Rush, cheats), and
// the image family the readable C is generated from (docs/decompile.md): the image with no
// setting on and one image per setting, written by coi --save-rom-family and by the coi_family
// build tool.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cheats.h"

// The settings that patch the ROM.
struct RomSettings {
    bool widescreen = false, smooth_swing = false;
    Cheats cheats;
};

// The image these settings make from the ROM as dumped. `report`: patches skipped go to stderr.
std::vector<uint8_t> make_rom_image(const std::vector<uint8_t>& original, const RomSettings& s, bool report);

// The settings in a fixed order: the readable C picks each patched function's variant by the bit
// of its setting (coi_native_settings, bit k = setting k).
struct RomSetting {
    const char* name;
    bool& (*field)(RomSettings&);
};
extern const RomSetting kRomSettings[];
extern const size_t kRomSettingCount;

uint32_t rom_settings_mask(RomSettings s);

// DIR/base.bin (no setting on), DIR/<setting>.bin for each one alone, and DIR/family.txt (the
// settings in bit order). False if a file couldn't be written.
bool save_rom_family(const std::vector<uint8_t>& original, const std::string& dir);
