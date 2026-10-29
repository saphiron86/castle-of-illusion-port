// Boss Rush: the game's options screen gets a BOSS RUSH tick box. See boss_rush.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Adds the Boss Rush code past the end of the 512 KB ROM and patches the game to use it.
// The caller fixes the header checksum afterwards. Every patch checks the original bytes
// first; on any mismatch nothing is changed and the reason is returned. Returns an empty
// string on success.
std::string apply_boss_rush(std::vector<uint8_t>& rom);
