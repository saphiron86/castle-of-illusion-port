// Sega Genesis VDP (315-5313): ports, DMA and a scanline renderer.
#pragma once

#include <cstdint>
#include <array>
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
        // Optional (Smooth Water): plane A's translucent water, low and high priority, each
        // `width` pixels, premultiplied 0xAARRGGBB (0 = none). Without them the water is
        // drawn into plane A's layers as it is in the original.
        uint32_t* water[2] = {nullptr, nullptr};
        uint32_t backdrop = 0;      // out: the colour behind every layer
    };

    uint8_t vram[0x10000] = {};
    uint16_t cram[64] = {};
    uint16_t vsram[40] = {};
    uint8_t reg[32] = {};

    // The ports' command state, for saving and restoring it (the recompiler's test harness).
    struct PortState {
        uint32_t address;
        uint8_t code;
        bool pending, fill_pending;
    };
    PortState port_state() const { return {address, code, pending, fill_pending}; }
    void set_port_state(const PortState& s)
    {
        address = s.address;
        code = s.code;
        pending = s.pending;
        fill_pending = s.fill_pending;
    }
    bool trace = false;         // log register writes and commands to stdout
    bool sprite_masking = true; // X-0 sprites hide later sprites on their lines (off only for testing)

    // Optional per-scene fill for the widened strips: called for plane A pixels outside the
    // original columns (view_x < 0 or >= 320) with the plane coordinates about to be sampled;
    // it may change them to sample a different part of the plane, and returns true if it did.
    std::function<bool(int view_x, int line, int& plane_x, int& plane_y)> extend_plane_a;
    // When set, extend_plane_a is asked about pixels inside the original columns too (for a
    // rule that moves part of the scene, such as the castle exterior's widened tower).
    bool extend_plane_a_inside = false;
    // The same for plane B (view_x is plane B's position in the view), and its "inside" flag.
    std::function<bool(int view_x, int line, int& plane_x, int& plane_y)> extend_plane_b;
    bool extend_plane_b_inside = false;
    // When set, plane B pixels remapped by extend_plane_b in the right strip take the high
    // priority, so low-priority sprites pass behind them.
    bool strip_b_priority_right = false;
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
    // Smooth Water: the water the game draws as a dithered pattern on plane A (a checkerboard
    // of one colour, or streaks with gaps) is drawn translucent instead. For each tile
    // (2048), bit p is set when pixel value p (1-15) of that tile is water, and bit 16 + p
    // when value p paints what lies behind the water (where streaks cross a ledge, the gaps
    // show the ledge's own colours); transparent pixels of a water tile are water too, other
    // pixels are drawn as usual. Each water pixel takes the colour and coverage of the water
    // pixels around it (3x3, weighted 1-2-1), and that colour is laid over what lies beneath
    // plane A (or the average of the "behind" pixels around it) with the coverage as its
    // opacity, so a checkerboard becomes an even 50% tint. Empty: off.
    std::vector<uint32_t> water_tiles;
    // Pictures drawn in place of some sprites (Smooth Water: stage 10's waterspouts, morphed
    // between their two poses). While there are any, the sprites marked in hidden_sprites (by
    // sprite table index) are not drawn, and each overlay is drawn instead, behind the other
    // sprites and (without `priority`) behind
    // high-priority plane pixels, as a low-priority sprite would be: w x h pixels from view
    // position (x, y) in game pixels, 0xAARRGGBB with straight alpha (0: clear). It moves
    // like the sprite at table index `sprite` (sprite_offset) and with plane A.
    struct Overlay {
        int x = 0, y = 0, w = 0, h = 0;
        bool priority = false;
        int sprite = 0;
        std::vector<uint32_t> argb;
    };
    std::vector<Overlay> overlays;
    std::vector<uint8_t> hidden_sprites;            // 80 flags; empty: none
    // Optional colours for plane B pixels (Smooth Water: background water that flows by colour
    // cycling, morphed between the cycle's steps), by plane pixel (plane_b_rgb_w x plane_b_rgb_h,
    // the plane's size), 0xFFRRGGBB, 0 = the pixel's own colour.
    const uint32_t* plane_b_rgb = nullptr;
    int plane_b_rgb_w = 0, plane_b_rgb_h = 0;
    // Optional pictures for plane A tiles (Smooth Water: stage 25's water surface, morphed
    // between its two animation frames): tile_rgba_index[tile] (2048) is -1 or an entry of
    // tile_rgba, 8 x 8 pixels of straight 0xAARRGGBB, drawn in place of the tile's own pixels
    // and blended over what lies beneath plane A as the water is. Empty: none.
    std::vector<int16_t> tile_rgba_index;
    std::vector<std::array<uint32_t, 64>> tile_rgba;
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
    int strip_pixel(int base, int x, int y, int width_cells, int height_cells, bool& priority,
                    uint16_t* entry = nullptr) const;
    int clean_black_edge(int a, int base, int x, int y, int width_cells, int height_cells) const;
    // render_line's planes and compositing, without per-column vertical scroll.
    // The overlays on the current line: each one's row and the output pixel its column 0 is at.
    struct OverlayRow { const Overlay* overlay; int row, start; };
    std::vector<OverlayRow> overlay_rows;
    // The overlay colour (straight 0xAARRGGBB) at output pixel ox of the current line, 0 if
    // none, and that overlay's priority.
    uint32_t overlay_at(int ox, int scale, bool& priority) const;
    void render_pixels(int line, uint32_t* out, int width, int left, int scale, LineLayers* layers, uint32_t backdrop,
                       const uint8_t* sprite_pixel, const bool* sprite_priority, const bool* sprite_on_b, int sub_a,
                       int sub_b, int hs_a, int hs_b, bool window_line, int window_hp, bool window_right, int window_w,
                       int native_width, int window_period, int base_a, int base_b, int base_window, int plane_w,
                       int plane_h);
    // The same without Smooth Water's additions (faster), while they are not in use.
    void render_pixels_plain(int line, uint32_t* out, int width, int left, int scale, LineLayers* layers, uint32_t backdrop,
                       const uint8_t* sprite_pixel, const bool* sprite_priority, const bool* sprite_on_b, int sub_a,
                       int sub_b, int hs_a, int hs_b, bool window_line, int window_hp, bool window_right, int window_w,
                       int native_width, int window_period, int base_a, int base_b, int base_window, int plane_w,
                       int plane_h);

    Bus& bus;
    uint32_t address = 0;
    uint8_t code = 0;
    bool pending = false;       // first half of a two-word command received
    bool fill_pending = false;  // DMA fill armed, waiting for the data port write
    bool vblank = false;
    int hint_counter = 0;
    int vcounter = 0;
};
