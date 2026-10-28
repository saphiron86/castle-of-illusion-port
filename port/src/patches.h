// Patches applied to the in-memory ROM for widescreen (see docs/widescreen.md).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Extra pixels shown on each side of the original 320-pixel view.
constexpr int kWideMargin = 40;

// Applies the widescreen patches. Every patch checks the original bytes first; on
// any mismatch nothing is changed and the reason is returned. Returns an empty
// string on success. Call fix_checksum once all patches are in.
std::string apply_widescreen_patches(std::vector<uint8_t>& rom);

// Rewrites the header checksum, which the boot code verifies: the 16-bit sum of
// the words from $200 to the ROM end address in the header ($07FFFF).
void fix_checksum(std::vector<uint8_t>& rom);
