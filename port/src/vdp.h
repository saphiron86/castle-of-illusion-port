// Sega Genesis VDP (315-5313): ports, DMA and a scanline renderer.
#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <vector>

#include "fade.h"

class Bus;
class StateWriter;
class StateReader;

class Vdp {
public:
    static constexpr int kScreenHeight = 224;
    static constexpr int kMaxWidth = 512;
    static constexpr int kMaxScale = 4;     // the most output pixels per game pixel across

    explicit Vdp(Bus& bus) : bus(bus) {}

    void power_on();            // clears memory, registers and port state

    // Save states (savestate.h): memory, registers, the ports and the line counters.
    void save_state(StateWriter& w) const;
    bool load_state(StateReader& r);

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
    // Counts the writes to VRAM (for --gpu-vdp: a line after any takes a new copy of VRAM).
    uint32_t vram_writes = 0;
    bool trace = false;         // log register writes and commands to stdout
    bool sprite_masking = true; // X-0 sprites hide later sprites on their lines (off only for testing)
    int sprite_y_shift = 0;     // sprites drawn this many lines lower (Co-op Zoom's rows above the view)
    bool ignore_window = false; // draw the planes where the window plane would be (Co-op Zoom's lines under the HUD)
    // Co-op: Player 2's sprites (tiles recolor_tile .. + recolor_tiles - 1, palette line 0)
    // draw their pixel values in recolor_values (a bit each) as colour 64 + value: the
    // palette's colour with red and blue swapped (Mickey's red shorts in blue). Off at -1.
    int recolor_tile = -1, recolor_tiles = 0;
    uint16_t recolor_values = 0;
    // Only the biggest area of touching pixels of those values, as the sprites lay them out
    // (the shorts, at least recolor_min_area pixels), and the areas from its top row down are
    // recoloured, not those above (the tongue): one byte per pixel of each tile, built from
    // the sprite table on the frame's first line.
    int recolor_min_area = 12;
    uint16_t recolor_link_values = 0;   // values that join red areas (his buttons' yellow)
    std::vector<uint8_t> recolor_mask;
    void build_recolor_mask(int base_sprites, int max_sprites);
    // With sprite_masking off (Co-op Zoom's lines under the HUD): whether the sprite at `link`
    // only starts the game's masking there (left of the screen, the next one at X 0 on `line`).
    bool masking_lead(int base_sprites, int link, int line) const;

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
    // Smooth Animation (smooth_animation.h; the GPU renderer only, the CPU renderer ignores it):
    // objects drawn as one upright picture turned by `angle` (radians, clockwise) about
    // (x, y) (view position in game pixels, as a sprite's table X and Y less 128). The sprites
    // marked in `replaces` (by table index) are not drawn; the picture takes the place of the
    // first of them in the link order (`first`), and covers the lines within `radius` of y.
    // Its pixels (RotatedPicture) name the sprite tile pixel to show, so the game's own art
    // (or an art pack's) turns, square pixels and all.
    struct RotatedPicture {
        int w = 0, h = 0;
        float pivot_x = 0, pivot_y = 0;             // the turning point, in the picture
        // Per pixel, 0 for clear, or: attribute word | column << 16 | row << 21 (within its
        // sprite, as shown, before flips) | width - 1 << 26 | height - 1 << 28 | 1 << 31.
        std::vector<uint32_t> words;
    };
    struct Rotated {
        float x = 0, y = 0, angle = 0;
        float rate = 0;                             // radians a game frame (for pictures between frames)
        int radius = 0, first = -1;
        uint8_t replaces[80] = {};
        int picture = 0;                            // into rotated_pictures
        // Only where the game's own frames ever draw (the bell inside its arch): a mask picture
        // (rotated_pictures, nonzero = may draw; -1: none), its top left from the turning point
        // in game pixels, not turned.
        int clip = -1;
        float clip_x = 0, clip_y = 0;
        // A part carried by another (the jack-in-the-box's head on its spring): from rotated
        // [parent]'s turning point by `arm` (game pixels, turned with it), turned `keep` times
        // its angle (0: it stays upright). -1: none.
        int parent = -1;
        float arm_x = 0, arm_y = 0, keep = 0;
    };
    std::vector<Rotated> rotated;
    // Plane A pictures the game spins by rewriting their tiles (stage 6's pinwheels): the 16
    // tiles from `first` make a 32 x 32 block (`cell`: each tile's place, x | y << 2), and the
    // disc of `radius` about (cx, cy) in it is drawn turned `angle` (radians, clockwise) on
    // from the picture VRAM holds. GPU renderer only.
    struct Spinner {
        int first = 0;
        int plane = 0;                              // 0: plane A, 1: plane B
        // Colours that turn (bit c: colour c; 0: all, within `moving`); the rest show `behind`
        // (colour indices by block pixel, 0: unknown), what the game's pictures show there.
        uint16_t solid = 0;
        uint8_t behind[32 * 32] = {};
        // With solid colours: one picture of the game's (`base`, colour indices by block pixel)
        // turned all the way round, `offset` radians on from the picture VRAM holds plus
        // `angle` (the game's pictures differ a little from true turns of each other).
        bool single = false;
        float offset = 0;
        uint8_t base[32 * 32] = {};
        uint8_t cell[16] = {};
        float cx = 16, cy = 16, radius = 0, angle = 0;
        // For pictures between frames: the step's turn started `start` from the picture's angle,
        // `elapsed` frames ago, and takes `interval` frames.
        float start = 0, elapsed = 0, interval = 1;
        // The pixels that turn (a bit per pixel, row y in moving[y], bit x): those the game's
        // rewrites have ever changed. The rest of the block (its frame) stays put.
        uint32_t moving[32] = {};
        // Shading that stays put on the turning part (the pinwheel's shadow crescent): colour c
        // drawn in shadow is shade[c]; unshade undoes it (identity where none).
        uint8_t shade[16] = {}, unshade[16] = {};
    };
    std::vector<Spinner> spinners;
    // Smooth Animation's gliders: plane A pictures the game moves a few pixels at a time by
    // rewriting name-table cells (stage 6-8's bobbing flowers). A 2 x 3 cell area of the plane
    // (top left x, y in plane pixels) is drawn as `cell` (its entries, across then down: the
    // picture at its farthest from the ground) moved `offset` game pixels down (up: negative);
    // what moves past its edges is cut off and what comes in is see-through. `rate`: pixels a
    // frame, for pictures between frames.
    struct Glider {
        int x = 0, y = 0;
        float offset = 0, rate = 0;
        uint16_t cell[6] = {};
    };
    std::vector<Glider> gliders;
    // Smooth Animation's colour-cycled wheels (stage 26's big wheels): plane A pixels around
    // (x, y) (plane pixels) from `inner` to `outer` pixels out, in palette `palette`'s colours
    // `colours` (a bit each), are the spokes' stripes: drawn from their angle, a stripe each
    // `step` radians from `spoke_start` in the three colours in turn (`order`, clockwise),
    // turned `angle` on: the game cycles the colours to turn the stripes a step at a time. (From
    // the angle rather than the game's pixels, so whatever lies over the spokes can't break
    // them up.)
    // `start`, `elapsed`, `interval`: as Spinner's, for pictures between frames.
    // A rim (rim_outer > 0) whose dots use only two of the colours has no way round of its
    // own: from rim_inner to rim_outer about (rim_x, rim_y) it is drawn as rim_segments
    // segments in the three colours in turn (`order`), from rim_start radians, turned with the
    // stripes (`angle` / `step` segments).
    struct Cycler {
        int x = 0, y = 0;
        float inner = 0, outer = 0, angle = 0, start = 0, elapsed = 0, interval = 1, step = 1;
        int palette = 0;
        uint16_t colours = 0;
        uint8_t order[3] = {};
        float rim_inner = 0, rim_outer = 0, rim_segments = 0, rim_start = 0;
        float spoke_start = 0;
        int rim_x = 0, rim_y = 0;
    };
    std::vector<Cycler> cyclers;
    // Smooth Animation's drums (stage 26's big horizontal gears, seen edge on, turning about an
    // upright axis): a w x h area of plane A from (x, y) (plane pixels), in tiles first to
    // first + tiles - 1, is drawn with its teeth moved `angle` radians on round the drum
    // (radius w / 2), its lighting kept: the gaps between the teeth (with their edges) move,
    // over the faces' own colour in each place. `look`: that colour by area pixel (h rows of
    // w; the lightest the game's pictures show there; 0: none seen). `start`, `elapsed`,
    // `interval`: as Spinner's.
    struct Drum {
        int x = 0, y = 0, w = 0, h = 0, first = 0, tiles = 0;
        float angle = 0, start = 0, elapsed = 0, interval = 1;
    };
    std::vector<Drum> drums;
    std::vector<uint8_t> drum_look;                 // one area's (all drums share their tiles)
    int drum_look_w = 0, drum_look_h = 0;
    // Smooth Animation's window shutters (stages 6 and 8): plane A cells in tiles first to first
    // + 15 (each tile's place in the 32 x 32 block: cell) are drawn as two doors swung `angle`
    // radians toward the viewer on hinges at block columns hinge_l and hinge_r (the left door
    // columns hinge_l to split - 1, the right split to hinge_r - 1, rows above `sill`), from the
    // shut picture (`door`), in front of the open picture's window (`behind`; colour indices),
    // with a little perspective (viewer `distance` pixels away, about row `middle`) and a
    // shadow on the wall colour (`shadow_k` pixels across per pixel out). Past a right angle
    // the doors show their backs: colour `back`, `edge` at the free edge. `start`, `elapsed`,
    // `interval`: the swing (start 1 opening, -1 closing, 0 none; at rest open when
    // `open_angle` is the angle).
    struct Shutter {
        int first = 0;
        uint8_t cell[16] = {};
        float angle = 0, open_angle = 0;
        float start = 0, elapsed = 0, interval = 1;
        int hinge_l = 0, split = 0, hinge_r = 0, sill = 0;
        float distance = 0, middle = 0, shadow_k = 0;
        int wall = 0, shadow = 0, back = 0, edge = 0;
        uint8_t door[32 * 32] = {}, behind[32 * 32] = {};
    };
    std::vector<Shutter> shutters;
    // Smooth Animation's flower buds (stages 6 and 8): plane A cells in tiles first to first + 3
    // (2 x 2, row by row) show `look`, the bud morphed `openness` of the way from closed (0)
    // through half open (1) to open (2): kBudScale texels a pixel, colour indices (0: see
    // through). `pictures`: the game's closed, half open and open pictures (16 x 16), `rate`
    // the openness a frame (for the pictures between frames).
    static constexpr int kBudScale = 8;
    struct Bud {
        int first = 0;
        float openness = 0, rate = 0;
        uint8_t pictures[3][16 * 16] = {};
        uint8_t look[16 * kBudScale * 16 * kBudScale] = {};
    };
    std::vector<Bud> buds;
    // Smooth Animation's swinging chains (stage 24): a chain hanging from plane A pixel (x, y)
    // (its top middle, plane pixels), `length` pixels down (its anchor from row `anchor`, 32
    // wide; the links above it 16 wide), drawn bent: row r's middle `offset[r]` pixels across
    // and turned `angle[r]` radians. `history`: the angle at the top, now (0) and the frames
    // before (for the rows' lag, and the pictures between frames).
    static constexpr int kChainHistory = 32;
    struct Chain {
        int x = 0, y = 0, length = 0, anchor = 0;
        float history[kChainHistory] = {};
        float speed = 0;
        std::vector<float> offset, angle;
    };
    std::vector<Chain> chains;
    std::vector<RotatedPicture> rotated_pictures;
    // Optional colours for plane B pixels (Smooth Water: background water that flows by colour
    // cycling, morphed between the cycle's steps), by plane pixel (plane_b_rgb_w x plane_b_rgb_h,
    // the plane's size), 0xFFRRGGBB, 0 = the pixel's own colour.
    const uint32_t* plane_b_rgb = nullptr;
    int plane_b_rgb_w = 0, plane_b_rgb_h = 0;
    // Added to plane B's horizontal scroll on every line (Smooth Water: stage 12's river, its
    // snaps back taken out). Display only: line_scroll is where every renderer reads it.
    int plane_b_hs_add = 0;
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
    friend class GpuVdp;        // (it resolves the widened strips' remapped pixels as render_line does)
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
