// Cheats (the Cheats menu): ROM patches, most from the classic Game Genie codes. See cheats.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Cheats {
    bool invincibility = false;
    bool infinite_power = false;
    bool infinite_tries = false;
    bool infinite_items = false;
    bool infinite_continues = false;
    bool full_power = false;        // start every life with full power (5)
    bool walk_faster = false;
    bool moon_jump = false;         // moon jump and jump in midair switch each other off
    bool midair_jump = false;
};

// Applies the cheats that are on. Every patch checks the original bytes first; a cheat whose
// bytes don't match is skipped and named in the returned message (empty when all applied).
// Call fix_checksum afterwards.
std::string apply_cheats(std::vector<uint8_t>& rom, const Cheats& cheats);
