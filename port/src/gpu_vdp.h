// The VDP's picture drawn on the GPU (--gpu-vdp, a prototype): the frame's state is captured
// line by line as the CPU renderer would read it (the registers, scroll values, VSRAM, CRAM
// and each line's sprites, with the sprite limit and masking worked out here), and one
// fragment shader then draws every pixel from VRAM. The original picture, widescreen and
// Smooth Scrolling's flat picture so far (not its layers for the upscalers, nor the other
// enhancements): main.cpp uses it only for frames the CPU renderer would draw with no more than
// those. See docs/notes/native-renderer.md.
#pragma once

#include <cstdint>
#include <vector>

namespace art { struct Pack; }
namespace storm { struct Picture; }

class Vdp;

// The shader program every GpuVdp draws with. Building it can take long (about 37 s at every
// start with the NVIDIA Shield TV's driver), so the built program is saved beside the settings
// as the driver's binary and loaded from there while it matches (this version, the shaders'
// source and the driver); otherwise it is built once more, on a thread of its own with a
// context of its own when asked, while the CPU renderer draws.
namespace gpu_shader {
enum class State { None, Building, Ready, Failed };
// With the main OpenGL context current: loads the saved program, or builds it (on a thread
// when `background`, else here, before returning). Returns the state afterwards; once Ready or
// Failed, it stays so.
State start(bool background);
// Between frames: Ready (or Failed) once the thread's build is done.
State poll();
State state();
unsigned program();
// While Building: the seconds since it began, and those it is expected to take (the last
// build's on this device, or a guess before any).
double elapsed();
double expected();
// True on an NVIDIA Shield (Android TV), whose driver takes long over the build: the game
// waits for it there, drawing nothing but its progress (main.cpp).
bool on_nvidia_shield();
// At exit: a build still running is left to the process's end.
void abandon();
}  // namespace gpu_shader

class GpuVdp {
public:
    static constexpr int kLineWords = 314;      // per line: see gpu_vdp.cpp
    static constexpr int kOverlayRows = 8;      // overlay rows a line can show (Smooth Water's spouts)
    static constexpr int kLines = 224;          // the VDP's lines
    static constexpr int kRows = 244;           // a frame's rows, at most (overscan: main.cpp's kFrameRows)
    static constexpr int kMaxWidth = 512;       // view pixels across, at most

    // Call with an OpenGL 3.3 (or ES 3.0) context current. Returns false (with a message) if the
    // shader or textures could not be set up.
    bool init();
    void shutdown();

    // Records what line `line` will show, as the CPU renderer reads it just before the 68000
    // runs that line. VRAM is taken whole on the first line with the display on, and again on any line after one whose
    // 68000 code wrote to it (the game writes the HUD, and some tiles, while the picture is
    // being drawn), as many copies as it takes (at most one a line). The line shows `width` view pixels from
    // `view_left` pixels left of the original 320 (render_line's width and left).
    // Overscan (co-op's zoom out) draws some lines twice, into other rows of a taller frame: a
    // line goes to `row` (0 to rows - 1), with the Vdp as render_line sees it for that row
    // (VSRAM and sprites moved down, or no window and no masking). begin_frame starts a frame
    // of `rows` rows.
    void begin_frame(int rows);
    void capture_line(const Vdp& vdp, int line, int row, int width, int view_left);
    int rows = kLines;

    // Output pixels per game pixel (Smooth Scrolling: 4). Above 1 (or with the VDP's
    // scroll_at_scale_1, for pictures between frames), capture_line asks the VDP's smooth_scroll
    // for each line's shifts (once: the main frame's smoother keeps a history) and records them
    // for line_shifts.
    int scale = 1;
    bool line_shifts(int line, int& sub_a, int& sub_b) const;

    // While `hd` is on for a frame, render draws the flat picture with kHdScale rows a line
    // (`scale` must be kHdScale too): for an art pack (artpack.h), whose replaced tiles' texels
    // come from the pack's pictures while `pack` is set, or for Smooth Animation without one.
    // take_pack (once a frame) finds each VRAM tile's picture.
    const art::Pack* pack = nullptr;
    bool hd = false;
    static constexpr int kHdScale = 4;
    void take_pack(const Vdp& vdp);
    // Switches to another pack (nullptr: none): its pictures go up, and every tile is looked up
    // again, on the next frame.
    void set_pack(const art::Pack* p);

    // Layers (Smooth Scrolling with HQx or cleanEdge: main.cpp's render_layer_line): while set,
    // capture_line places the planes and sprites at whole pixels (scale 1) and records the
    // layers' shifts at kLayerSub per pixel instead, and render_layers draws the seven layers.
    bool layered = false;
    static constexpr int kLayerSub = 4;
    static constexpr int kLayerCount = 7;
    // A line's layer shifts in 1/kLayerSub pixels (plane A's, the sprites', plane B's, the
    // background camera's sprites'), whether it is a window line, and its backdrop colour.
    void layer_line(int line, int shifts[4], bool& window_line, uint32_t& backdrop) const;
    // Draws the layers into layer_texture(0-6): `width` x 224 texels, the view in `count` of
    // them from `start` (render_line's `left`: `view_left`); outside it every layer is clear,
    // but layer 0 is black when `black_border` (main.cpp's render_layer_line).
    void render_layers(int width, int start, int count, int view_left, bool black_border);
    unsigned layer_texture(int i) const { return layer_tex[i]; }
    // Smooth Water in layers: plane A's water, low and high priority (premultiplied RGBA, as
    // render_line's LineLayers::water), drawn by render_layers when the frame has any water or
    // tile pictures; has_water() is false otherwise (no water layers).
    bool has_water() const { return water_drawn; }
    unsigned water_texture(int i) const { return water_layer_tex[i]; }
    void read_water(int i, std::vector<uint32_t>& argb);
    // The layers (bit i: layer i) the captured frame certainly leaves clear, so the upscalers can
    // skip them: worked out from the lines' sprites, the name tables' priority bits and the
    // hidden-pixel fill (a layer also takes pixels from a higher one moving alike). Layers 0 and
    // 1 never count as clear, and no layer does in a frame with remapped strips.
    unsigned empty_layers() const { return strip_used ? 0u : ~layer_maybe & 0x7Fu & (storm_drawn ? ~0x40u : ~0u); }
    // Reads layer `i` of the last render_layers back as 0xAARRGGBB rows, top first.
    void read_layer(int i, std::vector<uint32_t>& argb);

    // While set, the widened strips may be remapped (Vdp::extend_plane_a/b: main.cpp's strip
    // rules), so capture_line resolves those pixels on the CPU, as render_line would.
    bool strips = false;

    // Draws the captured frame into texture(): `width` x 224 texels, of which `count` from
    // `start` show the view, `scale` texels per game pixel (black either side); the view's first
    // game pixel is `view_left` pixels left of the original picture (render_line's `left`).
    void render(int width, int start, int count, int view_left);
    unsigned texture() const { return target; }
    int width() const { return target_width; }

    // Weather - Stage 4 (storm.h): draws the rain and its splashes over the last render's picture,
    // or into the front layer (sprites, high priority) of the last render_layers, with a small
    // program of its own (so the main shader, and its build time, stay as they are).
    void draw_storm(const storm::Picture& p);

    // Reads the last render back as 0xAARRGGBB rows, top first (for --gpu-vdp-check).
    void read(std::vector<uint32_t>& argb);
    int picture_width() const { return target_width; }   // the last render's size
    int picture_rows() const { return target_rows; }

    long long copies = 0;                       // VRAM copies taken, over all frames
    int most_copies = 0;                        // the most in one frame
    bool timed = false;                         // time the GPU work (gpu_ms; waits for the GPU each picture)
    long long strip_frames = 0;                 // renders with remapped strip pixels
    long long overlay_lines = 0;                // lines with overlay rows (Smooth Water's spouts)
    long long overlay_overflow = 0;             // lines with more than kOverlayRows overlay rows (drawn wrong)

    // GPU time of the renders so far (desktop OpenGL only; 0 elsewhere), in ms.
    double gpu_ms = 0;
    int gpu_renders = 0;

private:
    std::vector<uint8_t> vram;                  // the copies, 64 KB each (grown as needed)
    int versions = 0;                           // this frame's copies so far
    uint32_t vram_writes = 0;                   // Vdp::vram_writes at the last copy
    int vram_layers = 0;                        // the texture array's layers
    uint32_t lines[kRows][kLineWords] = {};
    // Per view pixel of each line, from kStripMargin left of the view: the strip rules' remapped
    // plane pixels (gpu_vdp.cpp). Shifted planes show a pixel more on either side.
    static constexpr int kStripMargin = 2;
    uint32_t strip[kRows][kMaxWidth] = {};
    // With Smooth Water or Clean Rock Edges, for each remapped plane A pixel (gpu_vdp.cpp):
    // four words, its tile entry and pixel on the lines above, at and below (each with its own
    // scroll), and its plane coordinates.
    uint32_t strip_a[kRows][kMaxWidth * 6] = {};   // per pixel: plane A above, here, below, its coordinates; plane B here, its coordinates
    bool strip_a_used = false;
    unsigned strip_a_tex = 0;
    bool strip_used = false;                    // this frame has any
    int strip_width = 0, strip_left = 0;
    unsigned program = 0, vao = 0, vram_tex = 0, lines_tex = 0, strip_tex = 0, target = 0, fbo = 0, query = 0;
    int target_width = 0, target_rows = 0;
    unsigned layer_tex[kLayerCount] = {}, layer_fbo[2] = {};
    int layer_width = 0, layer_rows = 0;
    uint32_t backdrops[kRows] = {};
    unsigned layer_maybe = 0;                   // this frame's layers that may have pixels
    // Whether each name-table row has a high-priority entry (-1: not looked at yet), for one
    // plane's table at one VRAM state.
    struct PriorityRows {
        int base = -1, width = 0;
        uint32_t vram_writes = 0;
        int8_t row[128];
        bool any(const Vdp& vdp, int base, int width, int row);
    };
    PriorityRows priority_a, priority_b, priority_window;
    void note_layers(const Vdp& vdp, int line, int width, bool window_line);
    // Smooth Water and its kin as the frame's Vdp has them, taken on line 0 (gpu_vdp.cpp):
    // water_tiles, tile_rgba (as an index + 1 per tile, and the pictures), plane_b_rgb and the
    // overlays' pictures, one after another (overlay_start: where each begins).
    void take_water(const Vdp& vdp);
    // Clean Rock Edges (Vdp::clean_black_edges): row 0 of edge_table flags the rock edge tiles
    // (Vdp::edge_tiles), row 1 holds Vdp::edge_palette (2048 x 2).
    bool edges_on = false, edges_changed = false;
    std::vector<uint32_t> edge_table;
    unsigned edge_tex = 0;
    // Co-op: Player 2's recolour (Vdp::recolor_tile ..., its mask as render_line builds it on
    // line 0), a row of 64 per tile; recolor_count 0 = off.
    int recolor_first = 0, recolor_count = 0;
    bool recolor_changed = false;
    std::vector<uint32_t> recolor_table;
    unsigned recolor_tex = 0;
    bool water_on = false, pictures_on = false, b_rgb_on = false, water_drawn = false;
    std::vector<uint32_t> water_mask, pic_index, pics, b_rgb, overlay_px;
    std::vector<uint32_t> overlay_start;
    int b_rgb_w = 0, b_rgb_h = 0;
    bool b_rgb_changed = false, water_mask_changed = false, pics_changed = false, overlays_changed = false;
    // The water's first pass (layered mode 4): per line and plane A pixel (from kWaterLeft
    // left of the view), the water's opacity and colour, and what lies behind it.
    static constexpr int kWaterLeft = 32, kWaterWidth = kMaxWidth + 64;
    unsigned water_pre_tex[2] = {}, water_pre_fbo = 0;
    void water_prepass(int view_left);
    unsigned water_tex = 0, pic_index_tex = 0, pics_tex = 0, b_rgb_tex = 0, overlay_tex = 0;
    unsigned water_layer_tex[2] = {}, water_fbo = 0;
    void upload();
    void draw(int start, int count, int view_left, int pass, bool black_border);
    // The art pack's tables: per VRAM tile its picture's slot + 1 (2048 x 1), the pictures
    // (one atlas); the table's rows from 1 hold each picture's 16 dumped colours.
    uint32_t hd_slots[2048] = {};
    uint64_t hd_hash[2048] = {};                // each VRAM tile's pattern
    // Per VRAM tile: the look (take_pack call) it was last rewritten at; tiles the game keeps
    // rewriting are animated (waterfalls), and squares beside them skip neighbour-matched pictures.
    uint32_t hd_rewritten[2048] = {};
    uint32_t hd_look = 0;
    uint32_t hd_cells[3][4096] = {};            // planes A and B's and the window's cells: their block picture's slot + 1, or 0
    // Sprites by first tile: their piece's first slot + 1 (bits 0-27), width - 1 (28-29) and
    // height - 1 (30-31) in tiles; 0 for none.
    uint32_t hd_pieces[2048] = {};
    int hd_ref_row = 7;                         // hd_table's first row of colours
    uint16_t hd_cram[64] = {};                  // the colours as last seen (blocks are looked up in theirs)
    uint8_t hd_regs[6] = {};                    // the tables (planes, window, sprites), the mode, the plane size, as last seen
    std::vector<uint8_t> hd_vram;               // VRAM as take_pack last saw it (empty: not yet)
    bool hd_slots_changed = false;
    const art::Pack* hd_uploaded = nullptr;
    unsigned hd_table_tex = 0, hd_atlas_tex = 0;
    // Smooth Animation (Vdp::rotated): row 0 of rot_tex holds each rotated object's cosine,
    // sine, turning point (float bits), and its picture's column and size; its picture's
    // pixels (Vdp::RotatedPicture::words) are from row 1, side by side.
    static constexpr int kRotWidth = 1024, kRotRows = 129, kRotMax = 8;
    static constexpr int kRotPictures = 864;    // rows 1-: the turned pictures left of this column, the drums' look from it
    std::vector<uint32_t> rot_words = std::vector<uint32_t>(kRotWidth * kRotRows);
    bool rot_changed = false;
    unsigned rot_tex = 0;
    void take_rotated(const Vdp& vdp);
    bool query_pending = false;
    void collect_query();
    // The last render's (or render_layers') view: render's start, count and view_left.
    int last_start = 0, last_count = 0, last_view_left = 0;
    // Weather - Stage 4: its textures (the roofs, the splash), and whether the last render_layers'
    // front layer has rain (so it isn't counted clear).
    unsigned storm_heights_tex = 0, storm_splash_tex = 0, storm_fbo = 0;
    uint32_t storm_heights_version = 0, storm_splash_version = 0;
    int storm_columns = 0;
    bool storm_drawn = false;
};
