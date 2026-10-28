// Frame interpolation.
//
// The game runs at 60 frames a second. On a faster display (90, 120, 144 Hz...) each extra
// refresh would otherwise repeat a frame, so movement looks uneven. With interpolation, every
// refresh is drawn for its own moment between the last two game frames: the planes' scroll
// and each sprite are placed part of the way from the previous frame's position to the
// current one. That shows the game up to one frame (17 ms) later than it runs.
//
// To redraw a frame at any moment, each game frame keeps a snapshot of what its active lines
// were drawn with: VRAM (taken at line 0) and each line's registers, palette, vertical and
// horizontal scroll. The two most recent snapshots are kept.
//
// Sprites can't be matched between frames by their sprite table entries, which change as
// animations do and objects come and go. Instead, every write to the game's sprite list
// ($FFF500, built by DrawObject at $001AB6 from the object in a0, copied to the sprite table
// by DMA during VInt) records the object that wrote the entry, its position and the camera
// DrawObject placed it with. A sprite then moves back along its object's movement plus the
// camera's (not the planes': cutscenes scroll the planes without moving the camera, so a
// sprite fixed on screen must not follow them). With smooth scrolling the camera's part uses
// the smoother's fitted camera movement, as the planes do, so sprites stay on the scenery.
// Sprites not drawn by an object, new objects and jumps of more than 64 pixels stay where the
// current frame has them.

#include "interp.h"

#include <cstring>

namespace {

constexpr uint16_t kSpriteList = 0xF500;
constexpr uint32_t kDrawObject = 0x001AB6, kDrawObjectEnd = 0x001C94;   // up to DeleteObject

int word(const uint8_t* ram, uint32_t address)
{
    return (int16_t)(ram[address & 0xFFFF] << 8 | ram[(address + 1) & 0xFFFF]);
}

}  // namespace

int scroll_difference(int from, int to)
{
    int d = ((to - from) % 1024 + 1024) % 1024;
    return d >= 512 ? d - 1024 : d;
}

void FrameInterpolator::reset()
{
    building = {};
    latched = {};
    snaps[0].valid = snaps[1].valid = false;
    frames = 0;
}

void FrameInterpolator::sprite_written(uint16_t address, uint32_t pc, uint32_t a0, const uint8_t* ram)
{
    Owner& o = building[(address - kSpriteList) / 8 % kSprites];
    if (pc < kDrawObject || pc >= kDrawObjectEnd) {
        o.valid = false;                            // written by other code (the masking sprites)
        return;
    }
    const uint32_t object = a0 & 0xFFFF;
    o.valid = true;
    o.key = object << 16 | (uint16_t)word(ram, object);
    // DrawObject puts it on screen at X and Y less the camera, unless the object is
    // positioned in screen coordinates (bit 3 of byte 2).
    o.screen = ram[(object + 2) & 0xFFFF] & 0x08;
    o.x = word(ram, object + 0x10);
    o.y = word(ram, object + 0x14);
    o.camera_x = o.screen ? 0 : word(ram, 0xFA84);
    o.camera_y = o.screen ? 0 : word(ram, 0xFA80);
}

void FrameInterpolator::dma(uint32_t source)
{
    if ((source & 0xFFFFFF) == (0xFF0000u | kSpriteList))
        latched = building;
}

void FrameInterpolator::begin_frame(bool wide)
{
    cur ^= 1;
    Snapshot& s = snaps[cur];
    s.valid = false;
    s.frame = ++frames;
    s.wide = wide;
}

void FrameInterpolator::capture_line(const Vdp& vdp, int line)
{
    if (line < 0 || line >= Vdp::kScreenHeight)
        return;
    Snapshot& s = snaps[cur];
    if (line == 0) {
        std::memcpy(s.vram.data(), vdp.vram, sizeof(vdp.vram));
        s.owners = latched;
    }
    Line& l = s.lines[line];
    std::memcpy(l.reg, vdp.reg, sizeof(l.reg));
    std::memcpy(l.cram, vdp.cram, sizeof(l.cram));
    std::memcpy(l.vsram, vdp.vsram, sizeof(l.vsram));
    vdp.line_scroll(line, l.hs_a, l.hs_b);
    if (line == Vdp::kScreenHeight - 1)
        s.valid = true;
}

bool FrameInterpolator::ready() const
{
    return current().valid && previous().valid && current().frame == previous().frame + 1;
}

bool FrameInterpolator::sprite_motion(int index, int& dx, int& dy, int& camera_dx, int& camera_dy,
                                      bool& screen) const
{
    if (index < 0 || index >= kSprites)
        return false;
    const Owner& o = current().owners[index];
    if (!o.valid)
        return false;
    for (const Owner& p : previous().owners) {
        if (!p.valid || p.key != o.key || p.screen != o.screen)
            continue;
        dx = o.x - p.x;
        dy = o.y - p.y;
        camera_dx = (int16_t)(o.camera_x - p.camera_x);
        camera_dy = (int16_t)(o.camera_y - p.camera_y);
        screen = o.screen;
        const int sx = dx - camera_dx, sy = dy - camera_dy;   // on screen
        return sx >= -64 && sx <= 64 && sy >= -64 && sy <= 64 && camera_dx >= -64 && camera_dx <= 64
               && camera_dy >= -64 && camera_dy <= 64;
    }
    return false;
}
