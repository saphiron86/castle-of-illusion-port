// Frame interpolation for displays faster than 60 Hz (Video > High Refresh Interpolation).
// See interp.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "vdp.h"

class FrameInterpolator {
public:
    static constexpr int kSprites = 80;

    // The object that wrote a sprite-list entry: its X and Y words ($10, $14), in level
    // coordinates or, for objects positioned that way, screen coordinates; and the camera
    // (CameraA) DrawObject placed it with.
    struct Owner {
        bool valid = false;
        uint32_t key = 0;       // object slot address and id word
        int x = 0, y = 0;
        int camera_x = 0, camera_y = 0;
        bool screen = false;
    };
    // The VDP state each active line was drawn with.
    struct Line {
        uint8_t reg[32];
        uint16_t cram[64];
        uint16_t vsram[40];
        int hs_a, hs_b;
    };
    struct Snapshot {
        bool valid = false;
        uint32_t frame = 0;
        bool wide = false;
        std::vector<uint8_t> vram = std::vector<uint8_t>(0x10000);
        std::array<Line, Vdp::kScreenHeight> lines;
        std::array<Owner, kSprites> owners;     // by sprite table index
    };

    void reset();

    // While the game runs: a write to the sprite list (with the writing instruction's address
    // and a0), and a DMA from 68000 memory to the VDP.
    void sprite_written(uint16_t address, uint32_t pc, uint32_t a0, const uint8_t* ram);
    void dma(uint32_t source);

    // Each game frame: begin, then capture every active line before it is drawn.
    void begin_frame(bool wide);
    void capture_line(const Vdp& vdp, int line);

    // True once two consecutive frames are captured.
    bool ready() const;
    const Snapshot& previous() const { return snaps[cur ^ 1]; }
    const Snapshot& current() const { return snaps[cur]; }

    // How far the object owning sprite `index` moved from the previous frame to the current
    // one (dx, dy, in its own coordinates) and how far the camera moved (camera_dx,
    // camera_dy; 0 for screen coordinates). False when it is not an object's sprite, is new,
    // or jumped on screen.
    bool sprite_motion(int index, int& dx, int& dy, int& camera_dx, int& camera_dy, bool& screen) const;

private:
    std::array<Owner, kSprites> building;   // the list the game is writing
    std::array<Owner, kSprites> latched;    // the list last copied to the VDP
    Snapshot snaps[2];
    int cur = 0;
    uint32_t frames = 0;
};

// Shortest difference between two 10-bit scroll values.
int scroll_difference(int from, int to);
