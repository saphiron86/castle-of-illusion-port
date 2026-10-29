#include "patches.h"

#include <cstdio>

namespace {

struct Patch {
    uint32_t address;
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
    const char* what;
};

// Helper routines go in the $FF padding at the end of the ROM.
constexpr uint32_t kSpawnRowStart = 0x7FED6;
constexpr uint32_t kRectVisibleA = 0x7FEE4;
constexpr uint32_t kRectVisibleB = 0x7FEF2;
constexpr uint32_t kInitCameraMin = 0x7FF00;
// $7FF10-$7FF23 is free again (the first IsRectVisibleA/B routines).
constexpr uint32_t kPlayerLeftLimit = 0x7FF24;
constexpr uint32_t kClampBeforeFill = 0x7FF36;
constexpr uint32_t kCameraMaxX = 0x7FF6E;
constexpr uint32_t kDeleteRange = 0x7FF8A;
constexpr uint32_t kSpawnLeft = 0x7FFA0;

const std::vector<Patch>& patches()
{
    static const std::vector<Patch> list = {
        // ClipSpriteX: keep sprite pieces from screen X -64 to 383 instead of -32 to 351.
        {0x001C3E, {0x00, 0x60}, {0x00, 0x40}, "ClipSpriteX lower bound"},
        {0x001C44, {0x01, 0xE0}, {0x02, 0x00}, "ClipSpriteX upper bound"},

        // InitCamera: minimum X 40 instead of 0 (arenas still override it with their
        // own larger minimum straight afterwards), maximum X width - 360 instead of - 320.
        {0x00EEB6, {0x42, 0x69, 0x00, 0x20, 0x42, 0x69, 0x00, 0x22},
                   {0x4E, 0xB9, 0x00, 0x07, 0xFF, 0x00, 0x4E, 0x71}, "InitCamera minimum X (jsr)"},
        {kInitCameraMin, std::vector<uint8_t>(12, 0xFF),
                   {0x42, 0x69, 0x00, 0x20,                     // clr.w   $20(a1)
                    0x33, 0x7C, 0x00, kWideMargin, 0x00, 0x22,  // move.w  #40,$22(a1)
                    0x4E, 0x75},                                // rts
                   "InitCamera minimum X (routine)"},
        // Maximum X width - 360 instead of - 320. Where that falls below the minimum (the
        // 512-pixel boss arenas), arenas locked at 192 (stages 9 and 28) keep their original
        // lock, so they are centred as on the console; stage 5 (camera 160-192 originally)
        // keeps the clamped maximum and sits at 152, so its view never passes the arena's
        // right edge.
        {0x00EEF2, {0x04, 0x40, 0x01, 0x40, 0x33, 0x40, 0x00, 0x26},
                   {0x4E, 0xB9, 0x00, 0x07, 0xFF, 0x6E, 0x4E, 0x71}, "InitCamera maximum X (jsr)"},
        {kCameraMaxX, std::vector<uint8_t>(28, 0xFF),
                   {0x04, 0x40, 0x01, 0x68,                     // subi.w  #360,d0
                    0xB0, 0x69, 0x00, 0x22,                     // cmp.w   $22(a1),d0          minimum X
                    0x6C, 0x0C,                                 // bge.s   store
                    0x0C, 0x69, 0x00, 0xC0, 0x00, 0x22,         // cmpi.w  #192,$22(a1)
                    0x6D, 0x04,                                 // blt.s   store
                    0x06, 0x40, 0x00, kWideMargin,              // addi.w  #40,d0              original width - 320
                    0x33, 0x40, 0x00, 0x26,                     // store: move.w d0,$26(a1)
                    0x4E, 0x75},                                // rts
                   "InitCamera maximum X (routine)"},

        // CameraFollow keeps the player at least 16 pixels right of the camera minimum X.
        // Use the original minimum for that, so the raised camera minimum does not move the
        // player's left wall: 40 can only come from the InitCamera patch above (the arenas'
        // minimums are 160, 192 and 3456), so treat it as the original 0.
        {0x00F71E, {0x32, 0x38, 0xFA, 0xA2, 0x06, 0x41, 0x00, 0x10},
                   {0x4E, 0xB9, 0x00, 0x07, 0xFF, 0x24, 0x4E, 0x71}, "CameraFollow player left limit (jsr)"},
        {kPlayerLeftLimit, std::vector<uint8_t>(18, 0xFF),
                   {0x32, 0x38, 0xFA, 0xA2,                     // move.w  ($FFFFFAA2).w,d1   camera minimum X
                    0x0C, 0x41, 0x00, kWideMargin,              // cmpi.w  #40,d1
                    0x66, 0x02,                                 // bne.s   keep
                    0x72, 0x00,                                 // moveq   #0,d1
                    0x06, 0x41, 0x00, 0x10,                     // keep: addi.w #$10,d1
                    0x4E, 0x75},                                // rts
                   "CameraFollow player left limit (routine)"},

        // The level's initial plane fill ($00EDF0) loads blocks from the camera X - 128 to
        // X + 400 at the start position. The raised minimum then moves the camera (0 -> 40)
        // without streaming, so blocks 400-432 were never loaded and showed stale tiles.
        // Clamp both cameras to their limits first (minimum, then maximum, as UpdateCamera
        // does) so the fill starts where the camera will actually be.
        {0x00EDF0, {0x4B, 0xF8, 0xF2, 0x28, 0x3A, 0xB8, 0xFA, 0x84},
                   {0x4E, 0xB9, 0x00, 0x07, 0xFF, 0x36, 0x4E, 0x71}, "initial plane fill (jsr)"},
        {kClampBeforeFill, std::vector<uint8_t>(56, 0xFF),
                   {0x41, 0xF8, 0xFA, 0x80,                     // lea     (CameraA).w,a0
                    0x61, 0x10,                                 // bsr.s   clamp
                    0x41, 0xF8, 0xFA, 0xC0,                     // lea     (CameraB).w,a0
                    0x61, 0x0A,                                 // bsr.s   clamp
                    0x4B, 0xF8, 0xF2, 0x28,                     // lea     ($FFFFF228).w,a5   (replaced)
                    0x3A, 0xB8, 0xFA, 0x84,                     // move.w  (CameraA_X).w,(a5) (replaced)
                    0x4E, 0x75,                                 // rts
                    0x30, 0x28, 0x00, 0x04,                     // clamp: move.w 4(a0),d0     X
                    0xB0, 0x68, 0x00, 0x22,                     //   cmp.w   $22(a0),d0       minimum X
                    0x6C, 0x04,                                 //   bge.s   +4
                    0x30, 0x28, 0x00, 0x22,                     //   move.w  $22(a0),d0
                    0xB0, 0x68, 0x00, 0x26,                     //   cmp.w   $26(a0),d0       maximum X
                    0x6F, 0x04,                                 //   ble.s   +4
                    0x30, 0x28, 0x00, 0x26,                     //   move.w  $26(a0),d0
                    0x31, 0x40, 0x00, 0x04,                     //   move.w  d0,4(a0)         X
                    0x31, 0x40, 0x00, 0x18,                     //   move.w  d0,$18(a0)       X at the last update
                    0x4E, 0x75},                                //   rts
                   "initial plane fill (clamp routine)"},

        // IsRectVisibleA/B: start the visible block range 3 blocks earlier, 26 blocks wide.
        // The game compares the range unsigned, so near the left end (camera X under 48)
        // a start below 0 hid every animated block (stage 6's flowers): clamp it at 0.
        {0x00F332, {0x34, 0x38, 0xFA, 0x84, 0xE8, 0x42},
                   {0x4E, 0xB9, 0x00, 0x07, 0xFE, 0xE4}, "IsRectVisibleA start (jsr)"},
        {kRectVisibleA, std::vector<uint8_t>(14, 0xFF),
                   {0x34, 0x38, 0xFA, 0x84,                     // move.w  (CameraA_X).w,d2
                    0xE8, 0x42,                                 // asr.w   #4,d2
                    0x57, 0x42,                                 // subq.w  #3,d2
                    0x6A, 0x02,                                 // bpl.s   +2
                    0x74, 0x00,                                 // moveq   #0,d2
                    0x4E, 0x75},                                // rts
                   "IsRectVisibleA start (routine)"},
        {0x00F346, {0x00, 0x14}, {0x00, 0x1A}, "IsRectVisibleA width"},
        {0x00DA9C, {0x34, 0x38, 0xFA, 0xC4, 0xE8, 0x42},
                   {0x4E, 0xB9, 0x00, 0x07, 0xFE, 0xF2}, "IsRectVisibleB start (jsr)"},
        {kRectVisibleB, std::vector<uint8_t>(14, 0xFF),
                   {0x34, 0x38, 0xFA, 0xC4,                     // move.w  (CameraB_X).w,d2
                    0xE8, 0x42,                                 // asr.w   #4,d2
                    0x57, 0x42,                                 // subq.w  #3,d2
                    0x6A, 0x02,                                 // bpl.s   +2
                    0x74, 0x00,                                 // moveq   #0,d2
                    0x4E, 0x75},                                // rts
                   "IsRectVisibleB start (routine)"},
        {0x00DAAE, {0x00, 0x14}, {0x00, 0x1A}, "IsRectVisibleB width"},

        // Building tops in stage 15 (objects $BE and $BF, sprites over plane B): they appear
        // when a building's plane B X is from 24 pixels left of CameraB_X to 344 right, and
        // hide once their sprite X leaves screen X -32 to 351, so near the widened view's
        // edges the buildings stood there without tops. Widen both ranges by 40 pixels.
        {0x00FEBC, {0x00, 0x18}, {0x00, 0x40}, "building tops: appear from 64 px left"},
        {0x00FEC6, {0x01, 0x70}, {0x01, 0xC0}, "building tops: appear up to 384 px right"},
        {0x00FE90, {0x00, 0x60}, {0x00, 0x38}, "building tops: hide past screen X -72"},
        {0x00FE96, {0x01, 0xE0}, {0x02, 0x08}, "building tops: hide past screen X 392"},

        // Spawning. Objects come from a grid of 128-pixel cells: when the camera X crosses a
        // multiple of 128, UpdateSpawns spawns a column of cells, moving left the one just
        // left of the camera (0-127 pixels left), moving right the one 384-511 pixels right,
        // and the drawer deletes objects more than 128 pixels left or 512 or more right. In
        // the original that is just in time on the left and 64 pixels past the view on the
        // right; the widened view shows 40 pixels more on each side, so objects popped in at
        // both edges (stage 16's falling floor blocks on the left, stage 26's yellow gears on
        // the right). Run the same scheme 40 pixels further out on each side, keeping the
        // original timing relative to the visible edges: moving left, test the crossing on
        // X - 40 (cells spawn 40-167 pixels left), moving right on X + 40 (424-551 right),
        // and delete past 168 left or from 552 right.
        {0x06D53C, {0x30, 0x2E, 0x00, 0x04, 0x36, 0x2E},
                   {0x4E, 0xF9, 0x00, 0x07, 0xFF, 0xA0}, "UpdateSpawns column test (jmp)"},
        {kSpawnLeft, std::vector<uint8_t>(38, 0xFF),
                   {0x30, 0x2E, 0x00, 0x04,                     // move.w  4(a6),d0           X
                    0x36, 0x2E, 0x00, 0x18,                     // move.w  $18(a6),d3         X at the last update
                    0x72, kWideMargin,                          // moveq   #40,d1
                    0x4A, 0x6E, 0x00, 0x1E,                     // tst.w   $1E(a6)            X velocity
                    0x6A, 0x02,                                 // bpl.s   +2                 moving right: +40,
                    0x44, 0x41,                                 // neg.w   d1                 moving left: -40
                    0xD0, 0x41,                                 // add.w   d1,d0
                    0xD6, 0x41,                                 // add.w   d1,d3
                    0xB1, 0x43,                                 // eor.w   d0,d3
                    0x08, 0x03, 0x00, 0x07,                     // btst    #7,d3
                    0x67, 0x06,                                 // beq.s   done
                    0x4E, 0xF9, 0x00, 0x06, 0xD5, 0xBA,         // jmp     SpawnColumn ($06D5BA), from X +- 40
                    0x4E, 0x75},                                // done: rts
                   "UpdateSpawns column test (routine)"},
        // Moving up or down, a crossing spawns a row of 5 cells from the one left of the
        // camera's (camera X - 128 to at least X + 384). Start from the cell left of X - 40's
        // and take 6, so rows reach both widened edges as the columns do.
        {0x06D586, {0x30, 0x2E, 0x00, 0x04, 0x02, 0x40, 0xFF, 0x80},
                   {0x4E, 0xB9, 0x00, 0x07, 0xFE, 0xD6, 0x4E, 0x71}, "UpdateSpawns row start (jsr)"},
        {kSpawnRowStart, std::vector<uint8_t>(14, 0xFF),
                   {0x30, 0x2E, 0x00, 0x04,                     // move.w  4(a6),d0           X
                    0x04, 0x40, 0x00, kWideMargin,              // subi.w  #40,d0
                    0x02, 0x40, 0xFF, 0x80,                     // andi.w  #$FF80,d0          (replaced)
                    0x4E, 0x75},                                // rts
                   "UpdateSpawns row start (routine)"},
        {0x06D59C, {0x7E, 0x04}, {0x7E, 0x05}, "UpdateSpawns row: 6 cells"},
        {0x001B12, {0x0C, 0x44, 0x02, 0x80, 0x64, 0x00, 0x01, 0x7C, 0x4A, 0x44, 0x6B, 0x00, 0x01, 0x76},
                   {0x4E, 0xB9, 0x00, 0x07, 0xFF, 0x8A, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71},
                   "off-screen deletion, X (jsr)"},
        {kDeleteRange, std::vector<uint8_t>(22, 0xFF),
                   {0x0C, 0x44, 0x02, 0x80 + kWideMargin,       // cmpi.w  #$2A8,d4           X + 128 - camera X
                    0x6C, 0x08,                                 // bge.s   delete             552 or more right
                    0x0C, 0x44, 0xFF, 0x100 - kWideMargin,      // cmpi.w  #-40,d4
                    0x6D, 0x02,                                 // blt.s   delete             over 168 left
                    0x4E, 0x75,                                 // rts
                    0x58, 0x8F,                                 // delete: addq.l #4,a7       drop the return
                    0x4E, 0xF9, 0x00, 0x00, 0x1C, 0x94},        // jmp     DeleteObject ($001C94)
                   "off-screen deletion, X (routine)"},
    };
    return list;
}

}  // namespace

std::string apply_widescreen_patches(std::vector<uint8_t>& rom)
{
    for (const Patch& p : patches()) {
        if (p.address + p.original.size() > rom.size())
            return std::string("out of range: ") + p.what;
        for (size_t i = 0; i < p.original.size(); i++)
            if (rom[p.address + i] != p.original[i]) {
                char message[160];
                std::snprintf(message, sizeof(message), "unexpected bytes at $%06X (%s)", p.address + (unsigned)i, p.what);
                return message;
            }
    }
    for (const Patch& p : patches())
        for (size_t i = 0; i < p.replacement.size(); i++)
            rom[p.address + i] = p.replacement[i];
    return "";
}

void fix_checksum(std::vector<uint8_t>& rom)
{
    const uint32_t end = (uint32_t)rom[0x1A4] << 24 | rom[0x1A5] << 16 | rom[0x1A6] << 8 | rom[0x1A7];
    uint16_t sum = 0;
    for (size_t i = 0x200; i + 1 <= end && i + 1 < rom.size(); i += 2)
        sum += rom[i] << 8 | rom[i + 1];
    rom[0x18E] = sum >> 8;
    rom[0x18F] = sum & 0xFF;
}
