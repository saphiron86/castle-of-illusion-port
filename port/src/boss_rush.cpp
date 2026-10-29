// Boss Rush: a BOSS RUSH tick box on the game's options screen.
//
// The options screen (GameState $08 draws it, $0A runs it) is laid out from 12-byte pieces in
// the table at $00A7B8 (source, VDP command, width - 1, height - 1), each placed by its own
// VDP command, and its cursor (Mickey on the swing, object $C4) moves between three rows
// with fixed swing heights. To fit a fourth row, every gap closes to one row: the Difficulty
// sign moves up a row, Sound Test two, Control three and the exit line down one, with the
// values the cursor redraws (difficulty name, sound number, control lines) moved to match.
// BOSS RUSH goes in at row 24 with a 3 x 3 sign built from the Difficulty sign's ends, an
// X (font tile $31) in it when ticked.
//
// The new code (boss_rush.asm, assembled into boss_rush_code.h) lives at $0D0000, past the end
// of the 512 KB ROM and clear of smooth swinging's $0C0000. The cursor's state dispatch goes
// through it: row 2 can now move down, state 7 is row 3, and B/C there tick or untick (the
// presses that change the other rows' values). The swing's chain grows to 21 links for row 3,
// so the two highlight objects that sat in slots $FFD900-$FFD9BF move to $FFDA00-$FFDABF.

#include "boss_rush.h"

#include <cstdio>

#include "boss_rush_code.h"

namespace {

struct Patch {
    uint32_t address;
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
    const char* what;
};

constexpr uint32_t kDrawOptions = kBossRushOrigin, kCursorMain = kBossRushOrigin + 6,
                   kSetRowState = kBossRushOrigin + 12, kRedraw = kBossRushOrigin + 18,
                   kStartGame = kBossRushOrigin + 24, kLevelStart = kBossRushOrigin + 30,
                   kRushFrame = kBossRushOrigin + 36, kTryLimit = kBossRushOrigin + 42,
                   kMizrabelHit = kBossRushOrigin + 48, kResultsStart = kBossRushOrigin + 54,
                   kResultsFrame = kBossRushOrigin + 60, kMizrabelFade = kBossRushOrigin + 66,
                   kHudFrame = kBossRushOrigin + 72, kHudLevelStart = kBossRushOrigin + 78;

std::vector<uint8_t> jsr(uint32_t a) { return {0x4E, 0xB9, uint8_t(a >> 24), uint8_t(a >> 16), uint8_t(a >> 8), uint8_t(a)}; }
std::vector<uint8_t> jmp(uint32_t a) { return {0x4E, 0xF9, uint8_t(a >> 24), uint8_t(a >> 16), uint8_t(a >> 8), uint8_t(a)}; }

std::vector<uint8_t> operator+(std::vector<uint8_t> a, const std::vector<uint8_t>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

const std::vector<uint8_t> kNop = {0x4E, 0x71};

// The VDP command that writes plane A ($C000, 64 cells across) at a cell.
std::vector<uint8_t> cell(int row, int col)
{
    const uint32_t a = 0xC000 + (row * 64 + col) * 2;
    const uint32_t cmd = 0x40000000 | (a & 0x3FFF) << 16 | a >> 14;
    return {uint8_t(cmd >> 24), uint8_t(cmd >> 16), uint8_t(cmd >> 8), uint8_t(cmd)};
}

// A layout piece's VDP command (index 1-$21 in the table at $00A7B8).
uint32_t piece(int index) { return 0xA7B8 + (index - 1) * 12 + 4; }

const std::vector<Patch>& patches()
{
    static const std::vector<Patch> list = {
        // Layout pieces: signs, labels, the exit line and the difficulty names.
        {piece(0x01), cell(2, 21), cell(1, 21), "Difficulty sign"},
        {piece(0x02), cell(9, 21), cell(7, 21), "Sound Test sign"},
        {piece(0x19), cell(16, 21), cell(13, 21), "Control sign"},
        {piece(0x1A), cell(3, 5), cell(2, 5), "DIFFICULTY"},
        {piece(0x1B), cell(10, 5), cell(8, 5), "SOUND TEST"},
        {piece(0x1C), cell(19, 5), cell(16, 5), "CONTROL"},
        {piece(0x1D), cell(26, 7), cell(27, 7), "PRESS START BUTTON - EXIT"},
        {piece(0x15), cell(4, 22), cell(3, 22), "NORMAL"},
        {piece(0x16), cell(4, 22), cell(3, 22), "HARD"},
        {piece(0x17), cell(4, 22), cell(3, 22), "PRACTICE"},
        // Values the cursor writes straight to the VDP.
        {0x00441E, cell(11, 22), cell(9, 22), "sound number"},
        {0x004456, cell(18, 22), cell(15, 22), "control line 1"},
        {0x004476, cell(20, 22), cell(17, 22), "control line 2"},
        {0x00448A, cell(22, 22), cell(19, 22), "control line 3"},

        // GameState_08: draw BOSS RUSH and its sign after the labels.
        {0x009EEE, {0x20, 0x3C, 0x1D, 0x1C, 0x1B, 0x1A, 0x4E, 0xBA, 0x08, 0x6C}, jsr(kDrawOptions) + kNop + kNop,
         "options labels"},
        // The cursor: state dispatch, where it settles on a row, and the value redraw.
        {0x003FC2, {0x43, 0xFA, 0x00, 0x06, 0x4E, 0xFA, 0xDC, 0xAC}, jmp(kCursorMain) + kNop, "cursor states"},
        {0x004158, {0x11, 0x78, 0xF4, 0x1E, 0x00, 0x03, 0x52, 0x28, 0x00, 0x03}, jsr(kSetRowState) + kNop + kNop,
         "cursor settles (up)"},
        {0x0041A8, {0x11, 0x78, 0xF4, 0x1E, 0x00, 0x03, 0x52, 0x28, 0x00, 0x03}, jsr(kSetRowState) + kNop + kNop,
         "cursor settles (down)"},
        {0x0041D2, {0x11, 0x78, 0xF4, 0x1E, 0x00, 0x03, 0x52, 0x28, 0x00, 0x03}, jsr(kSetRowState) + kNop + kNop,
         "cursor settles (after a change)"},
        {0x0043EC, {0x10, 0x38, 0xFF, 0x0D, 0x0C, 0x00, 0x00, 0x01}, jmp(kRedraw) + kNop, "value redraw"},
        // The rush: start at the first boss, one try at each level start, the clock and the
        // jump to the next boss every frame of play, and no extra-try items.
        {0x009FCC, {0x61, 0x22, 0x11, 0xF8, 0xFF, 0x0D, 0xFF, 0x0F}, jsr(kStartGame) + kNop, "game start"},
        {0x009618, {0x4E, 0xBA, 0x00, 0xB2, 0x4E, 0xBA, 0x02, 0xD4}, jsr(kLevelStart) + kNop, "level start"},
        {0x009808, {0x4E, 0xBA, 0x00, 0xD0, 0x61, 0x26}, jsr(kRushFrame), "frame of play"},
        {0x000C1C, {0x0C, 0x78, 0x00, 0x0A, 0xF3, 0x24}, jsr(kTryLimit), "extra-try item"},
        {0x009812, {0x4E, 0xBA, 0x04, 0x2E, 0x4E, 0xFA, 0x03, 0x32}, jmp(kHudFrame) + kNop, "score and HUD"},
        {0x009680, {0x4E, 0xF9, 0x00, 0x07, 0xC3, 0xD8}, jmp(kHudLevelStart), "level start's end"},
        // Mizrabel's last hit stops her clock; the ending's two GameStates show the results
        // instead in a Boss Rush.
        {0x010890, {0x11, 0xFC, 0x00, 0x9B, 0xF4, 0x0E}, jsr(kMizrabelHit), "Mizrabel's last hit"},
        {0x010E48, {0x70, 0x01, 0x4E, 0xB9, 0x00, 0x00, 0xA5, 0xC0}, jsr(kMizrabelFade) + kNop, "fade to white"},
        {0x00A110, {0x33, 0xF8, 0xFF, 0x08, 0x00, 0xC0, 0x00, 0x04}, jmp(kResultsStart) + kNop, "ending start"},
        {0x00A158, {0x31, 0xFC, 0x00, 0x04, 0xF3, 0x02}, jmp(kResultsFrame), "ending frame"},
        // Swing heights: start, row 0 down, row 1 up and down (row 2 is in the new code).
        {0x003FF4, {0x00, 0x38}, {0x00, 0x30}, "cursor start height"},
        {0x0040A0, {0x00, 0x70}, {0x00, 0x60}, "row 0 -> 1 height"},
        {0x0040C2, {0x00, 0x38}, {0x00, 0x30}, "row 1 -> 0 height"},
        {0x0040D8, {0x00, 0xB8}, {0x00, 0xA0}, "row 1 -> 2 height"},
        // The highlight objects: out of the longer chain's way, and moved with their rows.
        {0x004034, {0xD9, 0x00}, {0xDA, 0x00}, "highlight 1 slot"},
        {0x00403C, {0xD9, 0x10}, {0xDA, 0x10}, "highlight 1 X"},
        {0x004040, {0x00, 0x2C, 0x00, 0xFF, 0xD9, 0x14}, {0x00, 0x24, 0x00, 0xFF, 0xDA, 0x14}, "highlight 1 Y"},
        {0x00404C, {0xD9, 0x40}, {0xDA, 0x40}, "highlight 2 slot"},
        {0x004054, {0xD9, 0x50}, {0xDA, 0x50}, "highlight 2 X"},
        {0x004058, {0x00, 0x64, 0x00, 0xFF, 0xD9, 0x54}, {0x00, 0x54, 0x00, 0xFF, 0xDA, 0x54}, "highlight 2 Y"},
        {0x004064, {0xD9, 0x69}, {0xDA, 0x69}, "highlight 2 row"},
        {0x00406C, {0xD9, 0x80}, {0xDA, 0x80}, "highlight 3 slot"},
        {0x004074, {0xD9, 0x90}, {0xDA, 0x90}, "highlight 3 X"},
        {0x004078, {0x00, 0xBC, 0x00, 0xFF, 0xD9, 0x94}, {0x00, 0xA4, 0x00, 0xFF, 0xDA, 0x94}, "highlight 3 Y"},
    };
    return list;
}

}  // namespace

std::string apply_boss_rush(std::vector<uint8_t>& rom)
{
    for (const Patch& p : patches())
        for (size_t i = 0; i < p.original.size(); i++)
            if (p.address + i >= rom.size() || rom[p.address + i] != p.original[i]) {
                char message[160];
                std::snprintf(message, sizeof(message), "Boss Rush: unexpected bytes at $%06X (%s)",
                              p.address + (unsigned)i, p.what);
                return message;
            }
    const size_t needed = kBossRushOrigin + sizeof(kBossRushCode);
    if (rom.size() < needed)
        rom.resize(needed, 0xFF);
    std::copy(std::begin(kBossRushCode), std::end(kBossRushCode), rom.begin() + kBossRushOrigin);
    for (const Patch& p : patches())
        std::copy(p.replacement.begin(), p.replacement.end(), rom.begin() + p.address);
    return "";
}
