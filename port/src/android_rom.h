// Android: asks the player for their ROM with the system's file picker. See android_rom.cpp.
#pragma once

#include <cstdint>
#include <vector>

// Asks for the ROM until a 512 KB file is chosen, keeps a copy in the app's private storage
// (rom/coi.bin, where the port looks first) and returns it. Empty if the player quits.
std::vector<uint8_t> android_pick_rom();
