// Sega Genesis VDP (315-5313): ports, DMA and a scanline renderer.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "fade.h"

class Bus;

class Vdp {
public:
    static constexpr int kScreenHeight = 224;
    static constexpr int kMaxWidth = 512;
    static constexpr int kMaxScale = 4;     // the most output pixels per game pixel across

    explicit Vdp(Bus& bus) : bus(bus) {}

    void power_on();            // clears memory, registers and port state

    uint16_t read_port(uint32_t port);
    void write_port(uint32_t port, uint16_t value);

    // Frame timing, driven by the main loop.
    void start_frame();
    bool vint_enabled() const { return reg[1] & 0x20; }
    bool hint_enabled() const { return reg[0] & 0x10; }
    bool hint_line();           // call once per line; true when the HInt counter expires
    void set_vblank(bool on) { vblank = on; }
    void set_line(int line) { vcounter = line; }

    // Renders one active line into `out` (0xAARRGGBB). The visible area spans
    // `width` pixels starting `left` pixels left of the original 320-pixel view.
    // With scale > 1, each game pixel becomes `scale` output pixels across (`out` holds
    // width * scale), so smooth_scroll can move the planes by fractions of a pixel.
    // With `layers`, the line is not combined into `out` (unused) but split into its layers
    // at whole-pixel positions (scale must be 1), for hqx with smooth scrolling.
    struct LineLayers;
    void render_line(int line, uint32_t* out, int width, int left, int scale = 1, LineLayers* layers = nullptr);
    struct LineLayers {
        // Back to front; SpritesB holds the sprites that move with plane B.
        enum { BLow, ALow, SpritesLow, BHigh, AHigh, SpritesB, SpritesHigh, kCount };
        uint32_t* layer[kCount];    // in: `width` pixels each; out: 0xAARRGGBB, 0 = transparent
        int sub_scale = 4;          // in: the scale smooth_scroll is asked for
        int sub_a = 0, sub_b = 0;   // out: smooth_scroll's shifts, in 1/sub_scale pixels
        int sub_sprites = 0;        // out: the sprite layers' shift (sub_a, or sprite_layer_shift's)
        bool window_line = false;   // out: plane A's layers hold the window (not shifted)
        uint32_t backdrop = 0;      // out: the colour behind every layer
    };

    uint8_t vram[0x10000] = {};
    uint16_t cram[64] = {};
    uint16_t vsram[40] = {};
    uint8_t reg[32] = {};
    bool trace = false;         // log register writes and commands to stdout
    bool sprite_masking = true; // X-0 sprites hide later sprites on their lines (off only for testing)

    // Optional per-scene fill for the widened strips: called for plane A pixels outside the
    // original columns (view_x < 0 or >= 320) with the plane coordinates about to be sampled;
    // it may change them to sample a different part of the plane, and returns true if it did.
    std::function<bool(int view_x, int line, int& plane_x, int& plane_y)> extend_plane_a;
    // When set, extend_plane_a is asked about pixels inside the original columns too (for a
    // rule that moves part of the scene, such as the castle exterior's widened tower).
    bool extend_plane_a_inside = false;
    // When set, opaque pixels remapped by extend_plane_a are drawn in front of everything,
    // sprites included, so sprites pass behind the fill (used for the ending's walls).
    bool strip_fill_in_front = false;
    bool strip_fill_in_front_left = false;      // the same, in the left strip only
    // Name table entries for pixels remapped by extend_plane_a, used in place of plane A's
    // own for the cells from (strip_cells_x, strip_cells_y), strip_cells_w cells wide (the
    // final boss arena's wall cache in main.cpp). Empty: plane A's own.
    std::vector<uint16_t> strip_cells;
    int strip_cells_x = 0, strip_cells_y = 0, strip_cells_w = 0;
    // When set, the black gaps between stones that reach the outside of a rock wall are
    // drawn as what lies beyond them (see clean_black_edge): in the original they show as
    // black notches along the forest's rock edges. edge_tiles lists the rock edge tiles
    // (outer side on the left as stored; flipped for right-hand edges).
    bool clean_black_edges = false;
    // The palette the game's fades move towards ($FFF800). A colour counts as black for
    // clean_black_edges only if it is black there too, so the dark colours that pass
    // through black during a fade are left alone.
    uint16_t edge_palette[64] = {};
    // The VDP shows at most 20 sprites on a line (16 in the 256-pixel mode); later ones are
    // dropped. When false, every sprite on the line is drawn (Remove Sprite Limit).
    bool sprite_limit = true;
    // Optional colours to show in place of CRAM's (Smooth Fades): used for each colour while
    // CRAM still holds the value they were made for.
    const PaletteOverride* palette = nullptr;
    std::vector<uint16_t> edge_tiles;
    // Optional sub-pixel scrolling, used when rendering with scale > 1: given a line's
    // horizontal scroll values for planes A and B, returns how far right to draw each, in
    // 1/scale pixels (sprites move with plane A).
    std::function<void(int line, int hs_a, int hs_b, int scale, int& sub_a, int& sub_b)> smooth_scroll;
    // Optional: true for sprites (by attribute word) that the game moves with the background
    // camera (CameraB_X) rather than the foreground one. With scale > 1 they are drawn
    // sub_sprites_b (in 1/scale pixels) right of their position instead of plane A's shift.
    std::function<bool(uint16_t attr)> sprite_moves_with_b;
    int sub_sprites_b = 0;
    // Optional, for frames drawn between two game frames: an extra offset for the sprite at
    // `index` in the sprite table (attribute word `attr`) on `line`, dx in 1/scale pixels
    // (right) and dy in lines (down).
    std::function<void(int index, int line, uint16_t attr, int scale, int& dx, int& dy)> sprite_offset;
    // Optional: called when a DMA copies 68000 memory to the VDP, with the source address.
    std::function<void(uint32_t source)> on_dma;
    // Optional, for layers: the sprite layers' shift on `line` in 1/scale pixels, instead of
    // plane A's (frames drawn between two game frames keep the sprites off the planes' motion).
    std::function<int(int line, int scale)> sprite_layer_shift;
    // When set, smooth_scroll is also asked at scale 1 (for whole-pixel shifts).
    bool scroll_at_scale_1 = false;

    // The line's horizontal scroll values for planes A and B, as the renderer reads them.
    void line_scroll(int line, int& hs_a, int& hs_b) const;

private:
    void do_dma();
    void write_data(uint16_t value);
    uint32_t color(int index) const;
    int plane_pixel(int plane_base, int x, int y, int width_cells, int height_cells, bool& priority,
                    uint16_t* entry = nullptr) const;
    int entry_pixel(uint16_t entry, int x, int y, bool& priority) const;
    int strip_pixel(int base, int x, int y, int width_cells, int height_cells, bool& priority) const;
    int clean_black_edge(int a, int base, int x, int y, int width_cells, int height_cells) const;

    Bus& bus;
    uint32_t address = 0;
    uint8_t code = 0;
    bool pending = false;       // first half of a two-word command received
    bool fill_pending = false;  // DMA fill armed, waiting for the data port write
    bool vblank = false;
    int hint_counter = 0;
    int vcounter = 0;
};
