// Smooth swinging (Enhancements > Graphics): the vine and rope swing at 60 frames a second
// instead of the original 10. See smooth_swing.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Adds the in-between swing animation past the end of the 512 KB ROM and patches the game to
// use it. The caller fixes the header checksum afterwards. Every patch checks the original
// bytes first; on any mismatch nothing is changed and the reason is returned. Returns an
// empty string on success.
std::string apply_smooth_swing(std::vector<uint8_t>& rom);

// Converts vine objects already in RAM between the two animations, so switching the setting
// mid-swing keeps each vine where it is in its swing.
void convert_swing_objects(uint8_t* ram, bool to_smooth);
