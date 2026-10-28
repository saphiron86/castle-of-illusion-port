// Smooth swinging.
//
// Vines and ropes (object routine at $000A70) swing through a 34-step animation script at
// $06A30C, one step every 6 frames, built from 9 poses of a 15-link rope (the other side of
// the swing reuses them mirrored). Mickey hangs from the pose's hitbox ($000AE4 turns it into
// his offset from the vine, $32/$36), so he and the camera following him also move only every
// 6 frames, which looks choppy next to the rest of the game.
//
// Every pose has the same 15 links in the same order, each a single 8 x 8 sprite, so a pose
// part of the way to the next one is the links moved part of the way. This builds a 204-step
// script (6 per original step, 1 frame each) with those in-between poses, plus a table of
// Mickey's offsets interpolated the same way, and patches four places:
//   $000A98  the vine loads the new script
//   $000AA4  the vine's starting step ($29) counts in the finer steps (x 6)
//   $000AE4  Mickey's offset comes from the interpolated table, by step
//   $00B73A  Mickey's pose art and facing ($00B772, state $25/$26) still use the original
//            step (step / 6), so his drawings change exactly when they did
// Nothing else reads the step. The swing takes as long as before; only the motion in
// between is new. Hitbox indexes (used to grab the vine) come from the nearest original pose.
//
// Everything new lives from $0C0000, past the end of the 512 KB ROM (and clear of an MD+
// pack's code, which its 1 MB ROM keeps at $080000); the header checksum
// (recomputed by fix_checksum) need not cover it.

#include "smooth_swing.h"

#include <cmath>
#include <cstdio>

namespace {

constexpr uint32_t kOriginalScript = 0x06A30C;
constexpr uint32_t kHitboxes = 0x0699CE;    // 4 bytes each: Y, height, X, width
constexpr uint32_t kBase = 0x0C0000;     // clear of MD+ packs' code at $080000-$080FFF
constexpr uint32_t kInitStep = kBase;       // helper routines
constexpr uint32_t kOffsets = kBase + 0x20;
constexpr uint32_t kPoseStep = kBase + 0x40;
constexpr uint32_t kScript = kBase + 0x100;
constexpr int kSteps = 34, kSub = 6, kLinks = 15, kRecordSize = 6 + 5 * kLinks;

struct Site {
    uint32_t address;
    std::vector<uint8_t> original;
    const char* what;
};

const Site kSites[] = {
    {0x000A98, {0x21, 0x7C, 0x00, 0x06, 0xA3, 0x0C, 0x00, 0x06}, "vine script pointer"},
    {0x000AA4, {0x11, 0x68, 0x00, 0x29, 0x00, 0x0B}, "vine starting step"},
    {0x000AE4, {0x4E, 0xB9, 0x00, 0x00, 0x92, 0x6A}, "Mickey's offset from the vine"},
    {0x00B73A, {0x7A, 0x00, 0x1A, 0x29, 0x00, 0x0B}, "Mickey's swing pose"},
    {kOriginalScript, {0x00, 0x02, 0x22, 0x05}, "swing animation script"},
};

uint16_t word(const std::vector<uint8_t>& rom, uint32_t a) { return rom[a] << 8 | rom[a + 1]; }

void put_word(std::vector<uint8_t>& rom, uint32_t a, uint16_t v)
{
    rom[a] = v >> 8;
    rom[a + 1] = v & 0xFF;
}

void put_long(std::vector<uint8_t>& rom, uint32_t a, uint32_t v)
{
    put_word(rom, a, v >> 16);
    put_word(rom, a + 2, v & 0xFFFF);
}

int lerp(int a, int b, int sub) { return (int)std::lround(a + (b - a) * (double)sub / kSub); }

}  // namespace

std::string apply_smooth_swing(std::vector<uint8_t>& rom)
{
    for (const Site& s : kSites)
        for (size_t i = 0; i < s.original.size(); i++)
            if (s.address + i >= rom.size() || rom[s.address + i] != s.original[i]) {
                char message[160];
                std::snprintf(message, sizeof(message), "unexpected bytes at $%06X (%s)", s.address + (unsigned)i, s.what);
                return message;
            }

    // The original script: count, frame time, then one word per step: the pose record's
    // offset from just after the time byte, plus 2, with bit 15 set for a mirrored pose.
    const uint32_t list = kOriginalScript + 4;
    struct Step { uint32_t record; bool mirrored; };
    Step steps[kSteps];
    for (int k = 0; k < kSteps; k++) {
        uint16_t e = word(rom, list + 2 * k);
        steps[k] = {list + ((e - 2) & 0x7FFF), (e & 0x8000) != 0};
    }
    // Mickey's offset for a step: the pose's hitbox (byte 1, or byte 3 when mirrored) as
    // $000AE4 turns it into $36 (Y + $30) and $32 (X + 4).
    auto offset = [&](const Step& s, int& y, int& x) {
        uint8_t box = rom[s.record + (s.mirrored ? 3 : 1)];
        y = (int8_t)rom[kHitboxes + 4 * box] + 0x30;
        x = (int8_t)rom[kHitboxes + 4 * box + 2] + 4;
    };

    const size_t needed = kScript + 4 + 2 * kSteps * kSub + kRecordSize * kSteps * kSub + 4 * kSteps * kSub + 16;
    if (rom.size() < needed)
        rom.resize(needed, 0xFF);
    put_word(rom, kScript, 0x0002);                 // animation 0 starts right after this word
    rom[kScript + 2] = kSteps * kSub;               // 204 steps
    rom[kScript + 3] = 0;                           // 1 frame each
    const uint32_t new_list = kScript + 4;
    const uint32_t records = new_list + 2 * kSteps * kSub;
    const uint32_t offsets = records + kRecordSize * kSteps * kSub;
    for (int k = 0; k < kSteps; k++) {
        const Step& a = steps[k];
        const Step& b = steps[(k + 1) % kSteps];
        // Poses on opposite sides only meet at the hanging-straight pose, which is the same
        // either way round, so draw the in-betweens on the side of the other one.
        const bool mirrored = a.mirrored == b.mirrored ? a.mirrored : (a.record == steps[0].record ? b.mirrored : a.mirrored);
        int ay, ax, by, bx;
        offset(a, ay, ax);
        offset(b, by, bx);
        for (int sub = 0; sub < kSub; sub++) {
            const int n = k * kSub + sub;
            const uint32_t r = records + kRecordSize * n;
            const Step& near = sub * 2 < kSub ? a : b;
            for (int i = 0; i < 6; i++)             // link count - 1, hitboxes, flags
                rom[r + i] = rom[near.record + i];
            for (int link = 0; link < kLinks; link++) {
                const uint32_t pa = a.record + 6 + 5 * link, pb = b.record + 6 + 5 * link;
                const uint32_t pn = near.record + 6 + 5 * link, p = r + 6 + 5 * link;
                rom[p] = (uint8_t)lerp((int8_t)rom[pa], (int8_t)rom[pb], sub);         // Y
                rom[p + 1] = rom[pn + 1];                                               // size
                rom[p + 2] = rom[pn + 2];                                               // tile
                rom[p + 3] = rom[pn + 3];
                rom[p + 4] = (uint8_t)lerp((int8_t)rom[pa + 4], (int8_t)rom[pb + 4], sub);  // X
            }
            put_word(rom, new_list + 2 * n, (uint16_t)((r - new_list + 2) | (mirrored ? 0x8000 : 0)));
            put_word(rom, offsets + 4 * n, (uint16_t)lerp(ay, by, sub));
            put_word(rom, offsets + 4 * n + 2, (uint16_t)lerp(ax, bx, sub));
        }
    }

    // $000AA4: move.b $29(a0),$B(a0) -> jsr InitStep
    const uint8_t init_step[] = {
        0x70, 0x00,                                 // moveq   #0,d0
        0x10, 0x28, 0x00, 0x29,                     // move.b  $29(a0),d0
        0xC0, 0xFC, 0x00, kSub,                     // mulu.w  #6,d0
        0x11, 0x40, 0x00, 0x0B,                     // move.b  d0,$B(a0)
        0x4E, 0x75};                                // rts
    // $000AE4: jsr sub_00926A (then the hitbox -> $32/$36 code) -> jmp Offsets
    const uint8_t offsets_code[] = {
        0x70, 0x00,                                 // moveq   #0,d0
        0x10, 0x28, 0x00, 0x0B,                     // move.b  $B(a0),d0
        0xE5, 0x48,                                 // lsl.w   #2,d0
        0x4B, 0xF9, 0, 0, 0, 0,                     // lea     (offsets).l,a5
        0x31, 0x75, 0x00, 0x00, 0x00, 0x36,         // move.w  (a5,d0.w),$36(a0)
        0x31, 0x75, 0x00, 0x02, 0x00, 0x32,         // move.w  2(a5,d0.w),$32(a0)
        0x4E, 0x75};                                // rts
    // $00B73A: moveq #0,d5 / move.b $B(a1),d5 -> jsr PoseStep
    const uint8_t pose_step[] = {
        0x7A, 0x00,                                 // moveq   #0,d5
        0x1A, 0x29, 0x00, 0x0B,                     // move.b  $B(a1),d5
        0x8A, 0xFC, 0x00, kSub,                     // divu.w  #6,d5  (step in the low word)
        0x4E, 0x75};                                // rts
    for (size_t i = 0; i < sizeof(init_step); i++) rom[kInitStep + i] = init_step[i];
    for (size_t i = 0; i < sizeof(offsets_code); i++) rom[kOffsets + i] = offsets_code[i];
    put_long(rom, kOffsets + 10, offsets);
    for (size_t i = 0; i < sizeof(pose_step); i++) rom[kPoseStep + i] = pose_step[i];

    put_word(rom, 0x000AA4, 0x4EB9); put_long(rom, 0x000AA6, kInitStep);   // jsr
    put_word(rom, 0x000AE4, 0x4EF9); put_long(rom, 0x000AE6, kOffsets);    // jmp
    put_word(rom, 0x00B73A, 0x4EB9); put_long(rom, 0x00B73C, kPoseStep);   // jsr
    put_long(rom, 0x000A9A, kScript);                                      // move.l #script,$6(a0)
    return "";
}

void convert_swing_objects(uint8_t* ram, bool to_smooth)
{
    // Object slots are $40 bytes; a vine has its script pointer at +6, step at +$B and the
    // frames left on the step at +$C (5 down to 0 in the original script).
    const uint32_t from = to_smooth ? kOriginalScript : kScript, to = to_smooth ? kScript : kOriginalScript;
    for (uint32_t o = 0xC000; o < 0xF000; o += 0x40) {
        uint8_t* obj = ram + o;
        const uint32_t script = (uint32_t)obj[6] << 24 | obj[7] << 16 | obj[8] << 8 | obj[9];
        if (script != from || obj[0xB] >= (to_smooth ? kSteps : kSteps * kSub))
            continue;
        if (to_smooth) {
            const int elapsed = 5 - (obj[0xC] > 5 ? 5 : obj[0xC]);
            obj[0xB] = (uint8_t)(obj[0xB] * kSub + elapsed);
            obj[0xC] = 0;
        } else {
            obj[0xC] = (uint8_t)(5 - obj[0xB] % kSub);
            obj[0xB] = (uint8_t)(obj[0xB] / kSub);
        }
        obj[6] = to >> 24; obj[7] = to >> 16; obj[8] = to >> 8; obj[9] = to & 0xFF;
    }
}
