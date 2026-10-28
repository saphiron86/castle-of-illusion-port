// Cheats as ROM patches. Most are the classic Game Genie codes (each code replaces one ROM
// word); the code is noted where one exists, and every site was checked against the
// disassembly:
//
//   $0092EE  Mickey is hit: sets the flashing timer ($F446), takes one power ($F322); at none
//            left he dies and loses a try ($F324). $0092B2 and $0092D2 test enemy contact and
//            call it. $00BFF8 marks Mickey dead on a deadly hazard.
//   $00BD6A  falling off the stage: no power, one try less.
//   $00B8CA  throwing an apple uses an item ($F320).
//   $07A590  the continue screen uses a continue ($FF40).
//   $009726  at the start of a life, power is raised to 3 (2 on the hardest setting, 5 on the
//            easiest; $FF0D).
//   $00CA0E  the walking speed limit, $16000 (1.375 pixels a frame): compared at $00CA18 and
//            $00CA22, set at $00CA2A.
//   $00C8A8  while jump is held: up to 20 frames of rising speed ($F405 counts), then the jump
//            ends (bit 2 of $F406); bit 13 of the input word ($F310) is the jump button held.

#include "cheats.h"

namespace {

// Room for helper routines, past the 512 KB ROM, an MD+ pack's code ($080000) and the smooth-swing
// data ($0C0000).
constexpr uint32_t kMidairHelper = 0x090000;

struct Patch {
    uint32_t address;
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
};

struct Cheat {
    bool Cheats::*on;
    const char* name;
    std::vector<Patch> patches;
};

const std::vector<Cheat>& cheat_list()
{
    static const std::vector<Cheat> list = {
        // No enemy contact (R0KA-A6ZY, R0KA-A604), no damage (R0KA-A61R) and no death on
        // hazards; for the hazards the bset that marks Mickey dead is removed (the Game Genie
        // code R09T-A616 points it elsewhere instead). Falling off the stage still costs a try.
        {&Cheats::invincibility, "Invincibility", {
            {0x0092B4, {0x4A, 0x38}, {0x4E, 0x75}},
            {0x0092DA, {0x4A, 0x38}, {0x4E, 0x75}},
            {0x0092EE, {0x11, 0xFC}, {0x4E, 0x75}},
            {0x00BFF8, {0x08, 0xF8, 0x00, 0x02, 0xDC, 0x31}, {0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71}}}},
        // Hits flash and knock Mickey back but take no power (ALKA-AA9Y: skip the subtraction).
        {&Cheats::infinite_power, "Infinite Power", {
            {0x0092F4, {0x53, 0x78}, {0x60, 0x02}}}},
        // No try lost when out of power (ALKT-AA2A) or falling off the stage (SC8T-AYXR).
        {&Cheats::infinite_tries, "Infinite Tries", {
            {0x009300, {0x53, 0x78}, {0x60, 0x02}},
            {0x00BD6E, {0x53, 0x78}, {0x4A, 0x78}}}},
        // Throwing keeps the item count (SC6A-AY0L).
        {&Cheats::infinite_items, "Infinite Items", {
            {0x00B8CA, {0x53, 0x78}, {0x4A, 0x78}}}},
        // The continue screen keeps the count (subq.b becomes tst.b; the flag it sets after
        // stays on).
        {&Cheats::infinite_continues, "Infinite Continues", {
            {0x07A590, {0x53, 0x38}, {0x4A, 0x38}}}},
        // Every life starts with power 5, whatever the difficulty.
        {&Cheats::full_power, "Start with Full Power", {
            {0x00972A, {0x70, 0x03}, {0x70, 0x05}},
            {0x009746, {0x00, 0x02}, {0x00, 0x05}}}},
        // Walking speed limit $25000, 2.3 pixels a frame. The Game Genie code (AMFA-AABN,
        // ADFA-BAVR) changes only the speed the limit sets, not the two comparisons with it,
        // so the speed saw-tooths between 1.4 and 2.3; with all three it is steady.
        // Above $18000 ($00C8F0) the walking code slows Mickey down to the limit, never below
        // it; at the new limit that would hold him at full speed after letting go, so that
        // threshold moves up by the same amount, to $27000.
        {&Cheats::walk_faster, "Walk Faster", {
            {0x00C8F2, {0x00, 0x01, 0x80, 0x00}, {0x00, 0x02, 0x70, 0x00}},
            {0x00CA1A, {0x00, 0x01, 0x60, 0x00}, {0x00, 0x02, 0x50, 0x00}},
            {0x00CA24, {0x00, 0x01, 0x60, 0x00}, {0x00, 0x02, 0x50, 0x00}},
            {0x00CA2C, {0x00, 0x01, 0x60, 0x00}, {0x00, 0x02, 0x50, 0x00}}}},
        // Holding jump keeps rising: the end of the jump and the 20-frame limit are ignored
        // (RHEA-A6ZW, RHEA-A60G).
        {&Cheats::moon_jump, "Moon Jump", {
            {0x00C8B2, {0x66, 0x2C}, {0x4E, 0x71}},
            {0x00C8C6, {0x64, 0x08}, {0x4E, 0x71}}}},
        // A fresh press of jump in the air (bit 5 of the input word) starts a new jump the way
        // the game starts one ($F405 and $F406 cleared), so every jump, the first included,
        // rises while jump is held, up to the usual height. (The Game Genie version, the moon
        // jump codes plus A1EA-AAF0, rises only on the frame jump is pressed, so even the first
        // jump is short.) The start of $00C8A8 calls a helper past the end of the ROM:
        //   move.w ($F310).w,d1 / btst #5,d1 / beq.s +8 / clr.b ($F405).w / clr.b ($F406).w
        //   btst #2,($F406).w / rts            (the original test, for the bne.s after the call)
        {&Cheats::midair_jump, "Jump in Midair", {
            {0x00C8A8, {0x32, 0x38, 0xF3, 0x10, 0x08, 0x38, 0x00, 0x02, 0xF4, 0x06},
                       {0x4E, 0xB9, 0x00, 0x09, 0x00, 0x00, 0x4E, 0x71, 0x4E, 0x71}},
            {kMidairHelper, {},
                       {0x32, 0x38, 0xF3, 0x10, 0x08, 0x01, 0x00, 0x05, 0x67, 0x08,
                        0x42, 0x38, 0xF4, 0x05, 0x42, 0x38, 0xF4, 0x06,
                        0x08, 0x38, 0x00, 0x02, 0xF4, 0x06, 0x4E, 0x75}}}},
    };
    return list;
}

}  // namespace

std::string apply_cheats(std::vector<uint8_t>& rom, const Cheats& cheats)
{
    std::string skipped;
    for (const Cheat& cheat : cheat_list()) {
        if (!(cheats.*cheat.on))
            continue;
        if (cheat.on == &Cheats::moon_jump && cheats.midair_jump)
            continue;                               // the menu allows only one
        for (const Patch& p : cheat.patches)
            if (rom.size() < p.address + p.replacement.size())
                rom.resize(p.address + p.replacement.size(), 0xFF);
        bool matches = true;
        for (const Patch& p : cheat.patches)
            for (size_t i = 0; i < p.original.size(); i++)
                if (p.address + i >= rom.size() || rom[p.address + i] != p.original[i])
                    matches = false;
        if (!matches) {
            skipped += skipped.empty() ? "" : ", ";
            skipped += cheat.name;
            continue;
        }
        for (const Patch& p : cheat.patches)
            for (size_t i = 0; i < p.replacement.size(); i++)
                rom[p.address + i] = p.replacement[i];
    }
    return skipped.empty() ? "" : "Cheats not applied (ROM differs): " + skipped;
}
