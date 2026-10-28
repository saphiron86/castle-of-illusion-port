#include "bus.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

extern "C" {
#include "m68k.h"
}

#include "sound.h"
#include "stages.h"
#include "vdp.h"

namespace {
constexpr uint32_t kMainLoopRead = 0x0003C4;        // MainLoop: move.w (GameState).w,d0
}

// $000000-$3FFFFF  cartridge ROM
// $A00000-$A0FFFF  Z80 address space (8 KB RAM, mirrored; YM2612 at $A04000)
// $A10000-$A1001F  I/O: version register, controller ports
// $A11100          Z80 bus request, $A11200 Z80 reset
// $C00000-$C0001F  VDP
// $E00000-$FFFFFF  64 KB work RAM, mirrored

uint8_t Bus::read8(uint32_t address)
{
    address &= 0xFFFFFF;
    if (address < 0x400000)
        return address < rom.size() ? rom[address] : 0xFF;
    if (address >= 0xE00000)
        return ram[address & 0xFFFF];
    if (address >= 0xA00000 && address < 0xA10000) {
        if ((address & 0xFFFF) < 0x4000)
            return z80_ram[address & 0x1FFF];
        return 0;                                   // YM2612 status: never busy
    }
    if (address >= 0xA10000 && address < 0xA10020)
        return io_read(address);
    if ((address & ~1u) == 0xA11100)
        return (address & 1) ? 0 : (z80_bus_requested ? 0x00 : 0x01);   // bit 0 clear = bus granted
    if (address >= 0xC00000 && address < 0xC00020) {
        uint16_t word = vdp->read_port(address & 0x1E);
        return (address & 1) ? word & 0xFF : word >> 8;
    }
    return 0xFF;
}

uint16_t Bus::read16(uint32_t address)
{
    address &= 0xFFFFFF;
    if (address < 0x400000) {
        if (address + 1 < rom.size())
            return rom[address] << 8 | rom[address + 1];
        return 0xFFFF;
    }
    if (address >= 0xE00000) {
        // The main loop reading GameState: the previous state routine has returned, so this
        // is the one safe point to switch states from outside.
        if ((address & 0xFFFF) == 0xF300 && jump_stage >= 0 && m68k_get_reg(nullptr, M68K_REG_PPC) == kMainLoopRead)
            start_game_at_stage();
        return ram[address & 0xFFFF] << 8 | ram[(address + 1) & 0xFFFF];
    }
    if (address >= 0xC00000 && address < 0xC00020)
        return vdp->read_port(address & 0x1E);
    return read8(address) << 8 | read8(address + 1);
}

void Bus::write8(uint32_t address, uint8_t value)
{
    address &= 0xFFFFFF;
    if (address >= 0xE00000) {
        ram[address & 0xFFFF] = value;
        log_watch(address, value, 1);
        if (on_sprite_list_write && (address & 0xFFFF) >= 0xF500 && (address & 0xFFFF) < 0xF780)
            on_sprite_list_write(address & 0xFFFF);
        return;
    }
    if (address >= 0xA00000 && address < 0xA10000) {
        if ((address & 0xFFFF) < 0x4000)
            z80_ram[address & 0x1FFF] = value;
        return;                                     // YM2612 writes from the 68000 are ignored for now
    }
    if (address >= 0xA10000 && address < 0xA10020) {
        io_write(address, value);
        return;
    }
    if (address == 0xA11100) {
        z80_bus_requested = value & 1;
        if (sound)
            sound->set_bus_requested(z80_bus_requested);
        return;
    }
    if (address == 0xA11200) {
        if (sound)
            sound->set_reset(!(value & 1));         // 0 holds the Z80 in reset
        return;
    }
    if (address >= 0xC00000 && address < 0xC00020) {
        if ((address & 0x1F) >= 0x10 && (address & 0x1F) < 0x18) {
            if ((address & 1) && sound)
                sound->psg_write(value);
            return;
        }
        vdp->write_port(address & 0x1E, value << 8 | value);   // byte writes reach the VDP doubled
        return;
    }
    // $A14000 (TMSS) and unmapped addresses: ignored
}

void Bus::write16(uint32_t address, uint16_t value)
{
    address &= 0xFFFFFF;
    if (address >= 0x03F7F0 && address < 0x03F800) {   // in ROM space: the MD+ registers
        if (on_cartridge_write)
            on_cartridge_write(address, value);
        return;
    }
    if (address >= 0xE00000) {
        ram[address & 0xFFFF] = value >> 8;
        ram[(address + 1) & 0xFFFF] = value & 0xFF;
        log_watch(address, value, 2);
        const uint16_t a = address & 0xFFFF;
        if (on_sprite_list_write && a >= 0xF500 && a < 0xF780)
            on_sprite_list_write(a);
        if (a == 0xF300) {
            if (value == 0x0C && start_stage >= 0) {
                stage_lock = start_stage;
                start_stage = -1;
                apply_stage_select();
            } else if (value == 0x0E) {
                stage_lock = -1;                    // level play: the game owns the stage again
            }
        } else if (stage_lock >= 0 && (a == 0xFF42 || a == 0xFF44 || a == 0xFF46)) {
            apply_stage_select();                   // keep the chosen stage during the level start
        }
        return;
    }
    if (address >= 0xC00000 && address < 0xC00020) {
        vdp->write_port(address & 0x1E, value);
        return;
    }
    if (address >= 0xA00000 && address < 0xA10000) {
        write8(address, value >> 8);                // word writes to Z80 space reach it as the high byte
        return;
    }
    write8(address, value >> 8);
    write8(address + 1, value & 0xFF);
}

void Bus::log_watch(uint32_t address, uint16_t value, int size)
{
    for (uint16_t w : watches)
        if ((address & 0xFFFF) == w || (size == 1 && (address & 0xFFFF) == w + 1))
            std::printf("[watch] PC=%06X write%s $%06X = $%0*X\n", m68k_get_reg(nullptr, M68K_REG_PPC),
                        size == 1 ? ".b" : ".w", address, size * 2, value);
}

void Bus::power_on()
{
    start_stage = jump_stage = -1;
    std::fill(std::begin(ram), std::end(ram), 0);
    std::fill(std::begin(z80_ram), std::end(z80_ram), 0);
    z80_bus_requested = false;
    stage_lock = -1;
    pad_data = 0x40;
    std::fill(std::begin(port_ctrl), std::end(port_ctrl), 0);
}

// Stage select. The game tracks progress as a stage index ($FFFF42, 0-27), which it maps
// to a level ($FFFF44) and an act ($FFFF46) through the byte tables at $00A008 and $00A024
// around the time it sets GameState to $0C (level start). From that write until level play
// ($0E), the chosen stage replaces any write to the three, so GameState_0C loads it however
// the game got there (a new game, or the attract demo, which picks its own stage); after
// that the game carries on from it as usual.
void Bus::apply_stage_select()
{
    uint8_t level = rom[0xA008 + stage_lock];
    uint8_t act = rom[0xA024 + stage_lock];
    bool changed = ram[0xFF43] != stage_lock || ram[0xFF45] != level || ram[0xFF47] != act;
    ram[0xFF42] = 0; ram[0xFF43] = (uint8_t)stage_lock;
    ram[0xFF44] = 0; ram[0xFF45] = level;
    ram[0xFF46] = 0; ram[0xFF47] = act;
    if (changed)
        std::printf("Stage select: stage %d (internal index %d, level %d, act %d)\n", stage_number(stage_lock),
                    stage_lock, level, act);
}

// Debug stage select: a new game at the chosen stage, from whatever the game is doing.
// What the title screen does when Start is pressed (GameState_0A at $009FCC, then $00A094)
// is redone here, and GameState goes straight to $0C (level start), which loads the stage.
void Bus::start_game_at_stage()
{
    auto set_word = [this](uint16_t a, uint16_t v) { ram[a] = v >> 8; ram[a + 1] = v & 0xFF; };
    ram[0xF40A] = 0;                                // not the attract demo (it would pick its own stage)
    ram[0xFF0D] = ram[0xFF0F];                      // difficulty from the options (the demo clears $FF0D)
    ram[0xFF40] = ram[0xFF0D] < 1 ? 3 : ram[0xFF0D] == 1 ? 1 : 5;   // continues, as at $00A094
    ram[0xFF41] |= 1;
    ram[0xF415] &= ~1;                              // first stage of a game: $0C sets 3 lives
    ram[0xF22C] = ram[0xF22D] = ram[0xF22E] = ram[0xF22F] = 0;      // score
    set_word(0xF320, 0);
    ram[0xF41D] = 0;                                // not paused
    set_word(0xF302, 0);                            // VIntMode, as the title screen leaves it
    stage_lock = jump_stage;
    jump_stage = -1;
    apply_stage_select();
    set_word(0xF300, 0x0C);
}

// Controller 1 (3-button pad). The game selects one of two button groups by
// writing TH (bit 6) to the data register; buttons read as 0 when pressed.
uint8_t Bus::io_read(uint32_t address)
{
    switch (address & 0x1F) {
    case 0x01:
        return 0xA0;                                // overseas, NTSC, no expansion unit, no TMSS
    case 0x03: {
        uint8_t pressed;
        if (pad_data & 0x40)                        // TH high: C B R L D U
            pressed = (buttons & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT))
                    | ((buttons & BTN_B) ? 0x10 : 0) | ((buttons & BTN_C) ? 0x20 : 0);
        else                                        // TH low: Start A 0 0 D U
            pressed = (buttons & (BTN_UP | BTN_DOWN)) | 0x0C
                    | ((buttons & BTN_A) ? 0x10 : 0) | ((buttons & BTN_START) ? 0x20 : 0);
        return (pad_data & 0x40) | (~pressed & 0x3F);
    }
    case 0x05:
    case 0x07:
        return 0x7F;                                // port 2 and expansion: nothing connected
    case 0x09:
    case 0x0B:
    case 0x0D:
        // Control registers read back as written and are 0 after power-on. The
        // boot code tests them to tell a cold boot from a reset (GameInit skips
        // its RAM setup when bit 6 of $A1000D is set).
        return port_ctrl[((address & 0x1F) - 0x09) / 2];
    default:
        return 0;
    }
}

void Bus::io_write(uint32_t address, uint8_t value)
{
    switch (address & 0x1F) {
    case 0x03: pad_data = value; break;
    case 0x09:
    case 0x0B:
    case 0x0D: port_ctrl[((address & 0x1F) - 0x09) / 2] = value; break;
    default: break;
    }
}
