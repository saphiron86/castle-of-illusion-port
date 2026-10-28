// Genesis memory map as seen from the 68000.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

class Vdp;
class Sound;

// Controller buttons, as bits of Bus::buttons (1 = pressed).
enum Button : uint8_t {
    BTN_UP = 1 << 0, BTN_DOWN = 1 << 1, BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
    BTN_B = 1 << 4, BTN_C = 1 << 5, BTN_A = 1 << 6, BTN_START = 1 << 7,
};

class Bus {
public:
    std::vector<uint8_t> rom;
    uint8_t ram[0x10000] = {};
    uint8_t z80_ram[0x2000] = {};   // sound driver and its mailbox at $1C00
    uint8_t buttons = 0;            // controller 1
    // Stage selects, as game stage indexes ($FFFF42; see stages.h for the stage numbers).
    int start_stage = -1;           // --stage: applied at the first level start, then -1
    int jump_stage = -1;            // debug menu: start a new game at this stage right away
    std::vector<uint16_t> watches;  // RAM words (low 16 bits of the address) whose writes are logged
    Vdp* vdp = nullptr;
    Sound* sound = nullptr;
    // Optional: called after each write to the game's sprite list ($FFF500-$FFF77F, 80
    // entries of 8 bytes, built by DrawObject and copied to the VDP by DMA), with the RAM
    // address written (frame interpolation uses it to note which object owns each sprite).
    std::function<void(uint16_t address)> on_sprite_list_write;
    // Optional: word writes to the cartridge's registers at $03F7F0-$03F7FF (MD+, mdplus.cpp).
    std::function<void(uint32_t address, uint16_t value)> on_cartridge_write;

    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);

    // Back to the power-on state (RAM cleared, Z80 bus released, no stage select pending),
    // as for a cold boot.
    void power_on();

private:
    void apply_stage_select();
    void start_game_at_stage();
    void log_watch(uint32_t address, uint16_t value, int size);
    uint8_t io_read(uint32_t address);
    void io_write(uint32_t address, uint8_t value);

    bool z80_bus_requested = false;
    int stage_lock = -1;            // stage held in place while the level start runs
    uint8_t pad_data = 0x40;        // last value written to the port 1 data register (TH in bit 6)
    uint8_t port_ctrl[3] = {};      // control registers of ports 1-3; 0 after power-on
};
