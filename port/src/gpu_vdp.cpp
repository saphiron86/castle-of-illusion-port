// The VDP's picture drawn on the GPU (--gpu-vdp): see gpu_vdp.h.
//
// Each line's record (GpuVdp::lines, a row of a 32-bit integer texture) holds:
//   [0]       registers 1, 2, 3 and 4 (a byte each, low first)
//   [1]       registers 7, 11, 12 and 16
//   [2]       registers 17 and 18, then the HUD row's repeat period (Vdp::render_line)
//   [3]       planes A and B's horizontal scroll (line_scroll)
//   [4-23]    VSRAM, two words each
//   [24-55]   CRAM, two words each
//   [56]      the sprites drawn on the line, front first (after the limit and masking); the
//             copy of VRAM the line shows << 8
//   [57-216]  each sprite: attribute word | row within it (flipped) << 16 | width - 1 << 24 |
//             height - 1 << 26 (in cells); then the output texel its first column starts at,
//             its shift included (plane A's, the background camera's for the sprites that move
//             with it, and Vdp::sprite_offset's for pictures between frames)
//   [219]     the VDP line the row shows (rows are lines, but for overscan's extra rows)
//   [217-218] the planes' shifts, in output texels: A's and B's (Smooth Scrolling, and the
//             pictures between frames: Vdp::smooth_scroll)
//   [220-283] the 64 colours as Vdp::color gives them at that line, 0xAARRGGBB: CRAM's, or
//             the palette override's (Smooth Fades, the waterfalls' colour cycles)
//   [284-287] layers only: the layers' shifts in 1/4 pixels, plane A's (as smooth_scroll gives
//             it), the sprites', plane B's and the background camera's sprites'
//   [288]     plane A's horizontal scroll on the lines above and below (<< 16), for Smooth
//             Water's neighbours
//   [289]     the overlay rows on the line (Vdp::overlays, at most kOverlayRows), then each in
//   [290-313] three words: the output texel of its column 0, where its row starts among the
//             overlays' pictures, and its width | priority << 16
// In layers, a sprite's first word has 1 << 28 when it moves with the background camera.
//
// Layers (render_line's LineLayers, for HQx and cleanEdge with Smooth Scrolling): the same
// shader sorts each pixel into the seven layers as render_line does, then gives the pixels a
// layer hides the colour of the layer in front where both move alike (main.cpp's
// render_layer_line), and writes them in two passes of four colour targets.
// VRAM goes up whole as layers of 256 x 256 bytes, one for each copy of the frame.
//
// Widescreen: the view runs past the original 320 pixels, and main.cpp's strip rules
// (Vdp::extend_plane_a/b) may remap plane pixels there (and, for some scenes, inside too). Those
// rules are C++, so capture_line asks them for each pixel they may move, as render_line does at
// that line, and stores the remapped pixels in `strip`, a word per view pixel:
//   bit 31    plane A remapped: bits 0-6 its pixel, bit 7 its priority, bit 8 drawn in front
//             of everything (Vdp::strip_fill_in_front)
//   bit 30    plane B remapped: bits 16-22 its pixel, bit 23 its priority The shader follows Vdp::render_line's per-pixel
// path without its enhancements, so the picture is the same byte for byte.

#include "gpu_vdp.h"

#include <SDL3/SDL.h>

#ifdef __ANDROID__
#include <EGL/egl.h>
#include <sys/system_properties.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "artpack.h"
#include "storm.h"
#include "vdp.h"

namespace {

using GLenum = unsigned;
using GLuint = unsigned;
using GLint = int;
using GLsizei = int;
using GLchar = char;
using GLuint64 = unsigned long long;

constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA8 = 0x8058, GL_RGBA = 0x1908, GL_BGRA = 0x80E1;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_TEXTURE_2D_ARRAY = 0x8C1A;
constexpr GLenum GL_R8UI = 0x8232, GL_R32UI = 0x8236, GL_RED_INTEGER = 0x8D94;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_NEAREST = 0x2600;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_TRIANGLES = 0x0004, GL_TEXTURE0 = 0x84C0, GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ALIGNMENT = 0x0D05;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_TIME_ELAPSED = 0x88BF, GL_QUERY_RESULT = 0x8866;
constexpr GLenum GL_NONE = 0;
constexpr GLenum GL_ZERO = 0, GL_ONE = 1, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;

#ifndef APIENTRY
#ifdef _WIN32
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif

#define GPU_VDP_GL(X) \
    X(void, glGenTextures, (GLsizei, GLuint*)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint*)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glTexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(void, glTexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glDisable, (GLenum)) \
    X(void, glEnable, (GLenum)) \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, glUniform1f, (GLint, float)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, glDeleteShader, (GLuint)) \
    X(GLuint, glCreateProgram, ()) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint*)) \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint*)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint*)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint*)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glDrawBuffers, (GLsizei, const GLenum*)) \
    X(void, glReadBuffer, (GLenum)) \
    X(const unsigned char*, glGetString, (GLenum)) \
    X(void, glGetIntegerv, (GLenum, GLint*)) \
    X(void, glFinish, ())

// Program binaries (gpu_shader's saved program): OpenGL ES 3.0 and OpenGL 4.1, or
// ARB_get_program_binary; without them the program is built at every start.
#define GPU_VDP_GL_BINARY(X) \
    X(void, glGetProgramBinary, (GLuint, GLsizei, GLsizei*, GLenum*, void*)) \
    X(void, glProgramBinary, (GLuint, GLenum, const void*, GLsizei)) \
    X(void, glProgramParameteri, (GLuint, GLenum, GLint))

// Timer queries: desktop OpenGL only (OpenGL ES 3.0 has them only as an extension).
#define GPU_VDP_GL_TIMER(X) \
    X(void, glGenQueries, (GLsizei, GLuint*)) \
    X(void, glDeleteQueries, (GLsizei, const GLuint*)) \
    X(void, glBeginQuery, (GLenum, GLuint)) \
    X(void, glEndQuery, (GLenum)) \
    X(void, glGetQueryObjectui64v, (GLuint, GLenum, GLuint64*))

#define DECLARE(ret, name, args) using name##_t = ret (APIENTRY*) args; name##_t name = nullptr;
GPU_VDP_GL(DECLARE)
GPU_VDP_GL_TIMER(DECLARE)
GPU_VDP_GL_BINARY(DECLARE)
#undef DECLARE

bool load()
{
    static int loaded = 0;                          // 1 all there, -1 some missing
    if (loaded)
        return loaded > 0;
    bool ok = true;
#define LOAD(ret, name, args) \
    name = reinterpret_cast<name##_t>(SDL_GL_GetProcAddress(#name)); \
    if (!name) { std::fprintf(stderr, "gpu-vdp: OpenGL function missing: %s\n", #name); ok = false; }
    GPU_VDP_GL(LOAD)
#undef LOAD
#ifndef __ANDROID__
#define LOAD_TIMER(ret, name, args) name = reinterpret_cast<name##_t>(SDL_GL_GetProcAddress(#name));
    GPU_VDP_GL_TIMER(LOAD_TIMER)
#undef LOAD_TIMER
#endif
#define LOAD_OPTIONAL(ret, name, args) name = reinterpret_cast<name##_t>(SDL_GL_GetProcAddress(#name));
    GPU_VDP_GL_BINARY(LOAD_OPTIONAL)
#undef LOAD_OPTIONAL
    loaded = ok ? 1 : -1;
    return ok;
}

const char* kVertex = R"(
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);  // (0,0), (2,0), (0,2)
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Vdp::render_line for one pixel, as its per-column path draws it at scale 1 with none of the
// port's additions. Row 0 of the output is line 0.
const char* kFragment = R"(
uniform highp usampler2DArray vram;
uniform highp usampler2D lines;
uniform highp usampler2D strips;
uniform int start, count;     // the output texels that show the view
uniform int scale;            // output texels per game pixel
uniform int view_left;        // the view's first game pixel, left of the original picture's
uniform int strip_on;         // 1: `strips` holds this frame's remapped pixels
uniform int strip_left;       // the view pixel of strips' first column, left of the original
uniform int layered;          // 0: the picture; 1: layers 0-3; 2: layers 4-6; 3: the water layers
uniform int black_border;     // layers: layer 0 is black outside the view
// Smooth Water and its kin (Vdp::render_pixels; lines without per-column scroll only):
uniform highp usampler2D water_mask;  // per tile (2048 x 1): Vdp::water_tiles
uniform highp usampler2D pic_index;   // per tile (2048 x 1): the tile's picture + 1, 0 = none
uniform highp usampler2D pics;        // the tile pictures, a row of 8 x 8 each (Vdp::tile_rgba)
uniform highp usampler2D b_rgb;       // Vdp::plane_b_rgb
uniform highp usampler2D overlay_px;  // the overlays' pictures, one after another, 1024 across
uniform int water_on, pictures_on, b_rgb_on;
// The water's first pass (layered 4) writes, per line and plane A pixel xa (at texel
// xa + kWaterLeft + view_left), the water's colour and opacity, and what lies behind it with
// flags (1: water, 2: behind shown); the other passes read them.
uniform highp sampler2D water_pre0, water_pre1;
// Clean Rock Edges (Vdp::clean_black_edge): row 0 flags the rock edge tiles, row 1 holds the
// palette the fades move towards (Vdp::edge_palette).
uniform highp usampler2D edge_table;
uniform int edges_on;
// Co-op: Player 2's recoloured sprite pixels (Vdp::recolor_mask, 64 per tile from
// recolor_tile; recolor_tiles 0 = none) show as colour 64 + value.
uniform highp usampler2D recolor;
uniform int recolor_tile, recolor_tiles;
// Remapped plane A pixels for Smooth Water and Clean Rock Edges (strip_a_on): per view pixel
// four words, the pixel on the line above, this line and the line below (bit 31 remapped,
// pixel << 16, tile entry), and this line's plane coordinates (x | y << 16, 16 bits each).
uniform highp usampler2D strip_a;
uniform int strip_a_on;
uniform int water_pre_on;
// Art packs (artpack.h): hd_table's row 0 holds each VRAM tile's picture slot + 1, rows 1-2 plane
// A's cells' block picture slots + 1 (0: none, take the tile's), rows 3-4 plane B's, row 5 the
// sprites' pieces by first tile (slot + 1 in bits 0-27, width - 1 and height - 1 above), row 6
// the window's cells (as planes A and B), from row 7 each slot's place in the atlas, and from
// hd_ref_row each slot's 16 dumped colours; hd_atlas the pictures, 64 across, 8 * hd_scale square.
// The picture is drawn vscale rows a line.
uniform highp usampler2D hd_table;
// Layer 1: each texel's blend (red: the second dumped colour * 16 + the share of it, 0-15).
uniform highp sampler2DArray hd_atlas;
uniform int hd_on, hd_scale, vscale, hd_ref_row, hd_debug;
// Smooth Animation: row 0 from column k * 16 holds rotated object k's cosine, sine and turning
// point (float bits), then its picture's first column, width and height; the pictures' pixels
// from row 1 (gpu_vdp.h); words 7-11: its clip mask's column + 1 (0: none), width, height, and
// its top left from the turning point (Vdp::Rotated::clip). Column 128: the spinners
// (Vdp::Spinner) in use; spinner k from column 129 + k * 304: its first tile, its tiles' cells (2 words, 4 bits each: x | y << 2), the tile at
// each cell (2 words, the same way), then the disc's middle x, y, radius, cosine and sine; words
// 10-11 and 12-13 its unshade and shade colour maps (4 bits each); word 14 its plane (0 A,
// 1 B); from word 16, the pixels that
// turn (32 rows of 32 bits: Vdp::Spinner::moving); word 15 its solid colours (a bit each, 0:
// none; bit 16: one picture, Vdp::Spinner::single) and from word 48 what lies behind and that
// picture (32 rows of 8 words, a byte a pixel: behind | picture << 4). Spinner k from column
// 129 + k * 304 (at most 2). Column 840: the
// gliders (Vdp::Glider) in use (at most 12; then the colour-cycled wheels, Vdp::Cycler: column
// 940 how many, wheel k from 941 + k * 14: its middle x | y << 16, its stripes' step and start
// (radians), inner and outer radius (float bits), its colours | palette << 16, their order (4
// bits each), the rim's inner and outer radius, segments, start and the turn in stripes or
// segments (float bits), the rim's middle x | y << 16); glider k from column 841 + k * 8: its top left (x | y << 16, plane pixels), its offset
// (float bits), then its entries (3 words, a row each: left | right << 16). The window shutters
// (Vdp::Shutter), row 33 from column 864: 1 if drawn, the first tile, its tiles' cells (2
// words, 4 bits each), the doors' cosine and sine, hinge_l | split << 8 | hinge_r << 16 | sill
// << 24, distance, middle, shadow_k (float bits), wall | shadow << 4 | back << 8 | edge << 12;
// rows 34 and 35 from column 864 the door and behind pictures (32 rows of 4 words, 4 bits a
// pixel). The flower buds (Vdp::Bud): row 33 column 900 1 if drawn, 901 the first tile; their
// look (128 x 128, 4 bits a texel) from row 36 column 864: texel (x, y) in row 36 + y / 8,
// column 864 + (y % 8) * 16 + x / 8. The chains (Vdp::Chain): row 33 column 910 how many (at
// most 8); chain k in rows 52 + k * 4 from column 864 (640 words): its top x | y << 16, length,
// anchor row, then from word 8 each row's middle and angle (float bits).
)" R"(
uniform highp usampler2D rot_tex;
const int kWaterLeft = 32;
layout(location = 0) out vec4 frag;
layout(location = 1) out vec4 frag1;
layout(location = 2) out vec4 frag2;
layout(location = 3) out vec4 frag3;

vec4 unpack(uint c)            // 0xAARRGGBB
{
    return vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) / 255.0;
}

// The layers' output for this pass (0-6, back to front, then plane A's water low and high),
// each 0 (clear) or 0xAARRGGBB.
void put_layers(uint l[7], uint w0, uint w1)
{
    if (layered == 1) {
        frag = unpack(l[0]); frag1 = unpack(l[1]); frag2 = unpack(l[2]); frag3 = unpack(l[3]);
    } else if (layered == 2) {
        frag = unpack(l[4]); frag1 = unpack(l[5]); frag2 = unpack(l[6]); frag3 = vec4(0.0);
    } else {
        frag = unpack(w0); frag1 = unpack(w1); frag2 = vec4(0.0); frag3 = vec4(0.0);
    }
}
int row, line, copy;                // the output row, the VDP line it shows (its record's [219])
int sub_y;                          // art packs: the texel row within the line (0 to vscale - 1)

int vb(int a) { a &= 0xFFFF; return int(texelFetch(vram, ivec3(a & 255, a >> 8, copy), 0).r); }
uint L(int i) { return texelFetch(lines, ivec2(i, row), 0).r; }
uint strip_a_word(int xa, int k)
{
    int i = xa + strip_left;
    return strip_a_on != 0 && i >= 0 && i < 512 ? texelFetch(strip_a, ivec2(i * 6 + k, row), 0).r : 0u;
}
int half_word(int first, int i) { uint w = L(first + (i >> 1)); return int((i & 1) != 0 ? w >> 16 : w & 0xFFFFu); }

vec4 color(int index)
{
    uint c = L(220 + (index & 63));
    return vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) / 255.0;
}

uint color_u(int index) { return L(220 + (index & 63)); }

// An art pack's texel of `tile`'s picture at (tx, ty) (0 to 8 * hd_scale - 1): false when the
// tile has none; `clear` where the picture is see-through. Its colour is scaled per channel by
// the colour the tile's own pixel (`index` of palette line `pal`) has now against the one it
// had in the dump (where that was black, the difference is added instead).
// `block_slot`: plane A's block picture for this cell (slot + 1), taken over the tile's own.
int plane_entry(int base, int x, int y, int width_cells, int height_cells);
int entry_pixel(int entry, int x, int y, out bool priority);

// Picture slot s's dumped colour k (alpha 0: not known).
vec4 hd_ref(int s, int k)
{
    return unpack(texelFetch(hd_table, ivec2((s * 16 + k) % 2048, hd_ref_row + (s * 16 + k) / 2048), 0).r);
}

// `pw` nonzero: the pixel lies in a plane (the table at `base`, pw x ph cells) at (x, y), and
// its neighbours are read from there, across tile edges; otherwise from the tile alone.
bool hd_texel(int tile, uint block_slot, int tx, int ty, int pal, int index, int base, int x, int y, int pw, int ph,
              out uint c, out bool clear)
{
    c = 0u;
    clear = false;
    if (hd_on == 0)
        return false;
    uint slot = block_slot != 0u ? block_slot : texelFetch(hd_table, ivec2(tile & 0x7FF, 0), 0).r;
    if (slot == 0u)
        return false;
    int s = int(slot) - 1, n = 8 * hd_scale;
    int a = int(texelFetch(hd_table, ivec2(s % 2048, 7 + s / 2048), 0).r);
    vec4 t = texelFetch(hd_atlas, ivec3(a % 64 * n + tx, a / 64 * n + ty, 0), 0);
    if (t.a < 0.5) {
        clear = true;
        return true;
    }
    // Recoloured as the dumped colour the texel is closest to (alpha 128 + that colour, the
    // loader's tag) has changed since; the pixel's own colour where that was dumped the same.
    // Colours dumped the same (a fade or a colour cycle under way) can't be told apart by
    // colour: of those, the pixel's own colour, then its neighbours' in the tile, are taken.
    vec3 rgb = t.rgb;
    int near = int(t.a * 255.0 + 0.5) - 128;
    if (near > 0 && near < 16) {
        uint was_u = texelFetch(hd_table, ivec2((s * 16 + near) % 2048, hd_ref_row + (s * 16 + near) / 2048), 0).r;
        // The colours of the pixel and its 8 neighbours in the tile (the pixel's own first). A
        // tag names one of the picture's 16 colours by likeness alone, which can be a colour
        // used elsewhere (one that cycles, as stage 11's waterfalls do, or that changes apart, as
        // in stage 4's night): it is taken as the local colour most like it, so it follows what
        // the art here is made of.
        // In a plane the neighbours come from the plane, across tile edges (an upscaler's
        // texel near a tile's edge takes colours from the next tile: kept to its own tile, the
        // tag went to an unlike colour there, and the texel kept its day colour at stage 4's
        // night); only those in the same palette line count.
        int px = tx / hd_scale, py = ty / hd_scale;
        int local[9];
        for (int n = 0; n < 9; n++) {
            int dx = n == 0 ? 0 : (n - 1) % 3 - 1, dy = n == 0 ? 0 : (n - 1) / 3 - 1;
            if (pw != 0) {
                int e = plane_entry(base, x + dx, y + dy, pw, ph);
                bool pri;
                local[n] = ((e >> 13) & 3) == pal ? entry_pixel(e, x + dx, y + dy, pri) & 15 : 0;
                if (n == 0)
                    local[n] = index;
            } else {
                int lx = clamp(px + dx, 0, 7), ly = clamp(py + dy, 0, 7);
                int pair = vb((tile & 0x7FF) * 32 + ly * 4 + (lx >> 1));
                local[n] = (lx & 1) != 0 ? pair & 15 : pair >> 4;
            }
        }
        int blend = int(texelFetch(hd_atlas, ivec3(a % 64 * n + tx, a / 64 * n + ty, 1), 0).r * 255.0 + 0.5);
        int other = blend >> 4;
        float share = float(blend & 15) / 15.0;
        bool near_here = false, other_here = other == 0;
        for (int n = 0; n < 9; n++) {
            near_here = near_here || local[n] == near;
            other_here = other_here || local[n] == other;
        }
        if (!near_here || !other_here) {
            // A tag that isn't among the colours here: the texel is taken as the blend of two
            // of them (or one) that comes closest. (An upscaler's even blend of two colours can
            // match a third dumped colour exactly, and was tagged as that: in stage 4 at night
            // that one stays put while the two darken, and the texel stood out as a line.)
            float best_e = 1e9;
            int first = near, second = 0;
            float part = 0.0;
            for (int n = 0; n < 9; n++) {
                int k = local[n];
                vec4 ka = hd_ref(s, k);
                if (k == 0 || ka.a == 0.0)
                    continue;
                vec3 r = rgb - ka.rgb;
                float e = dot(r, r) - (n == 0 ? 1e-4 : 0.0);
                if (e < best_e)
                    best_e = e, first = k, second = 0, part = 0.0;
                for (int m = n + 1; m < 9; m++) {
                    int k2 = local[m];
                    vec4 kb = hd_ref(s, k2);
                    if (k2 == 0 || k2 == k || kb.a == 0.0)
                        continue;
                    vec3 d = kb.rgb - ka.rgb;
                    float len = dot(d, d);
                    if (len < 1e-6)
                        continue;
                    float sh = clamp(dot(r, d) / len, 0.0, 1.0);
                    vec3 rest = r - sh * d;
                    e = dot(rest, rest);
                    if (e < best_e)
                        best_e = e, first = sh > 0.5 ? k2 : k, second = sh > 0.5 ? k : k2, part = sh > 0.5 ? 1.0 - sh : sh;
                }
            }
            near = first;
            other = second;
            share = part;
        } else for (int which = 0; which < 2; which++) {
            int tag = which == 0 ? near : other;
            if (tag <= 0)
                continue;
            vec3 tag_c = unpack(texelFetch(hd_table, ivec2((s * 16 + tag) % 2048, hd_ref_row + (s * 16 + tag) / 2048), 0).r).rgb;
            int best = -1;
            float best_d = 1e9;
            for (int n = 0; n < 9; n++) {
                int k = local[n];
                if (k == 0)
                    continue;
                vec3 d = unpack(texelFetch(hd_table, ivec2((s * 16 + k) % 2048, hd_ref_row + (s * 16 + k) / 2048), 0).r).rgb - tag_c;
                // (Ties: the pixel's own colour first, then the tag itself: colours dumped the
                // same may differ now, as stage 4's rocks do at night.)
                float dd = dot(d, d) - (n == 0 ? 2e-4 : k == tag ? 1e-4 : 0.0);
                if (dd < best_d)
                    best_d = dd, best = k;
            }
            if (best > 0) {
                if (which == 0)
                    near = best;
                else
                    other = best;
            }
        }
        if (other == near)
            other = 0;
        was_u = texelFetch(hd_table, ivec2((s * 16 + near) % 2048, hd_ref_row + (s * 16 + near) / 2048), 0).r;
        // A texel an upscaler blended from two dumped colours (layer 1: the other one and its
        // share): the same blend of the two as they are now, plus whatever the blend didn't
        // explain. (Recoloured by the nearer colour alone, such texels went wrong whenever the
        // two changed differently: hard light lines in stage 4 at night.)
        if ((was_u >> 24) != 0u && other > 0 && share > 0.0) {
            uint other_u = texelFetch(hd_table, ivec2((s * 16 + other) % 2048, hd_ref_row + (s * 16 + other) / 2048), 0).r;
            vec3 was = mix(unpack(was_u).rgb, unpack(other_u).rgb, share);
            vec3 now = mix(unpack(color_u(pal * 16 + near)).rgb, unpack(color_u(pal * 16 + other)).rgb, share);
            // (What the blend didn't explain scaled as the two have darkened: added whole, an
            // upscaler's slight tint by day stood out against stage 4's dark night colours.)
            float was_sum = was.r + was.g + was.b;
            float scale_rest = was_sum > 0.5 / 255.0 ? min((now.r + now.g + now.b) / was_sum, 1.0) : 1.0;
            rgb = now + (rgb - was) * scale_rest;
        } else if ((was_u >> 24) != 0u) {
            vec3 was = unpack(was_u).rgb, now = unpack(color_u(pal * 16 + near)).rgb;
            // Per channel by its own change. A channel the colour hadn't (0): what the texel has
            // there scaled by the colour's change as a whole (a blend's stray red fades with the
            // blue), plus what the colour gained there. A colour dumped black: the difference.
            float was_sum = was.r + was.g + was.b;
            if (was_sum < 0.5 / 255.0) {
                rgb = rgb + now - was;
            } else {
                float whole = (now.r + now.g + now.b) / was_sum;
                rgb = mix(rgb * whole + now, rgb * now / max(was, vec3(1e-3)), step(vec3(0.5 / 255.0), was));
            }
        }
    }
    if (hd_debug != 0)                  // (COI_ART_DEBUG: block or piece pictures green, single tiles red)
        rgb = mix(rgb, block_slot != 0u ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), 0.6);
    uvec3 u = uvec3(clamp(rgb, 0.0, 1.0) * 255.0 + 0.5);
    c = 0xFF000000u | u.r << 16 | u.g << 8 | u.b;
    return true;
}

)" R"(
// Vdp::render_pixels' blend: `over` laid on `under` with opacity alpha (0-255), opaque.
uint blend(uint under, uint over, int alpha)
{
    uint a = uint(alpha), r = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8)
        r |= ((((under >> shift) & 255u) * (255u - a) + ((over >> shift) & 255u) * a + 127u) / 255u) << shift;
    return r;
}

int entry_pixel(int entry, int x, int y, out bool priority);

int plane_entry(int base, int x, int y, int width_cells, int height_cells)
{
    x &= width_cells * 8 - 1;
    y &= height_cells * 8 - 1;
    int at = (base + ((y >> 3) * width_cells + (x >> 3)) * 2) & 0xFFFF;
    return vb(at) << 8 | vb(at | 1);
}

// A plane A pixel's kind for Smooth Water (render_pixels' fetch_a): 0 dry, 1 water, 2 what
// lies behind the water, + 4 in a tile with such pixels; 8 a tile picture's (its colour in
// `pic`). `a` is the pixel, (px, py) its place in the plane.
int water_kind(int entry, int a, int px, int py, out uint pic)
{
    int kind = 0;
    pic = 0u;
    uint mask = water_on != 0 ? texelFetch(water_mask, ivec2(entry & 0x7FF, 0), 0).r : 0u;
    int p = a & 15;
    if (mask != 0u) {
        kind = p == 0 || ((mask >> p) & 1u) != 0u ? 1 : ((mask >> (16 + p)) & 1u) != 0u ? 2 : 0;
        if (kind != 0 && (mask >> 16) != 0u)
            kind |= 4;
    }
    if (pictures_on != 0) {
        uint index = texelFetch(pic_index, ivec2(entry & 0x7FF, 0), 0).r;
        if (index != 0u) {
            int fx = (entry & 0x0800) != 0 ? 7 - (px & 7) : px & 7, fy = (entry & 0x1000) != 0 ? 7 - (py & 7) : py & 7;
            pic = texelFetch(pics, ivec2(fy * 8 + fx, int(index) - 1), 0).r;
            kind = 8;
        }
    }
    return kind;
}

uint average(int r, int g, int b, int n)
{
    return n != 0 ? uint(r / n) << 16 | uint(g / n) << 8 | uint(b / n) : 0u;
}

// Smooth Water at plane A's pixel xa of this line (of kind `kind`): its opacity and colour
// from the 3x3 pixels around it (weighted 1-2-1, the lines above and below with their own
// scroll), and the colour of what lies behind where the tile paints it.
void water_at(int xa, int kind, uint pic, int hs, int vs, int base, int plane_w, int plane_h, out int alpha,
              out uint tint, out bool has_behind, out uint behind)
{
    if ((kind & 8) != 0) {                      // a tile picture
        alpha = int(pic >> 24);
        tint = pic & 0xFFFFFFu;
        has_behind = false;
        behind = 0u;
        return;
    }
    int hs_up = int(L(288) & 0xFFFFu), hs_down = int(L(288) >> 16);
    int total = 0, cover = 0, r = 0, g = 0, b = 0, behind_w = 0, br = 0, bg = 0, bb = 0;
    for (int dy = -1; dy <= 1; dy++) {
        int ln = line + dy, h = dy < 0 ? hs_up : dy > 0 ? hs_down : hs;
        for (int dx = -1; dx <= 1; dx++) {
            int px = xa + dx - h, py = ln + vs, e, a;
            uint sw = strip_a_word(xa + dx, dy + 1);
            if ((sw & 0x80000000u) != 0u) {     // remapped by a strip rule
                e = int(sw & 0xFFFFu);
                a = int((sw >> 16) & 127u);
            } else {
                e = plane_entry(base, px, py, plane_w, plane_h);
                bool pri;
                a = entry_pixel(e, px, py, pri);
            }
            uint unused;
            int k = water_kind(e, a, px, py, unused);
            if (k == 0 || (k & 8) != 0)
                continue;
            int w = (dy == 0 ? 2 : 1) * (dx == 0 ? 2 : 1);
            uint c = color_u(a);
            total += w;
            if ((k & 3) == 2) {
                behind_w += w;
                br += w * int((c >> 16) & 255u); bg += w * int((c >> 8) & 255u); bb += w * int(c & 255u);
            } else if (a != 0) {
                cover += w;
                r += w * int((c >> 16) & 255u); g += w * int((c >> 8) & 255u); b += w * int(c & 255u);
            }
        }
    }
    alpha = cover * 255 / total;
    tint = average(r, g, b, cover);
    has_behind = behind_w > 0 && (kind & 4) != 0;
    behind = 0xFF000000u | average(br, bg, bb, behind_w);
}

// Clean Rock Edges: black in CRAM and in the fades' palette.
bool edge_black(int p)
{
    return p != 0 && (half_word(24, p) & 0xEEE) == 0 && (int(texelFetch(edge_table, ivec2(p, 1), 0).r) & 0xEEE) == 0;
}

int plane_pixel(int base, int x, int y, int width_cells, int height_cells, out bool priority);

// Vdp::clean_black_edge for plane A's black pixel `a` at plane coordinates (x, y); `src_dx`: how
// far across the pixel it returns lies (0: `a` itself or clear).
int clean_edge(int a, int base, int x, int y, int width_cells, int height_cells, out int src_dx)
{
    bool pri;
    src_dx = 0;
    int e = plane_entry(base, x, y, width_cells, height_cells);
    if (texelFetch(edge_table, ivec2(e & 0x7FF, 0), 0).r != 0u) {
        bool flipped = (e & 0x0800) != 0;
        int column = flipped ? 7 - (x & 7) : (x & 7);   // 0 = the outer side
        int dir = flipped ? 1 : -1;
        int k = 1;
        for (; k <= column; k++)
            if (!edge_black(plane_pixel(base, x + dir * k, y, width_cells, height_cells, pri)))
                break;
        if (k > column) {
            src_dx = dir * k;
            return plane_pixel(base, x + dir * k, y, width_cells, height_cells, pri);
        }
    }
    for (int dir = -1; dir <= 1; dir += 2)
        for (int k = 1; k <= 3; k++) {
            int n = plane_pixel(base, x + dir * k, y, width_cells, height_cells, pri);
            if (n == 0)
                return 0;
            if (!edge_black(n))
                break;
        }
    return a;
}

)" R"(
// The overlay colour (straight 0xAARRGGBB) at output texel ox, 0 if none (Vdp::overlay_at).
uint overlay_at(int ox, int rows, out bool priority)
{
    priority = false;
    for (int k = 0; k < rows; k++) {
        int rel = ox - int(L(290 + 3 * k));
        if (rel < 0)
            continue;
        int x = rel / scale;
        uint wp = L(292 + 3 * k);
        if (x < int(wp & 0xFFFFu)) {
            uint at = L(291 + 3 * k) + uint(x);
            uint c = texelFetch(overlay_px, ivec2(int(at & 1023u), int(at >> 10)), 0).r;
            if (c != 0u) {
                priority = (wp >> 16) != 0u;
                return c;
            }
        }
    }
    return 0u;
}

int entry_pixel(int entry, int x, int y, out bool priority)
{
    priority = (entry & 0x8000) != 0;
    int fx = x & 7, fy = y & 7;
    if ((entry & 0x0800) != 0) fx = 7 - fx;
    if ((entry & 0x1000) != 0) fy = 7 - fy;
    int pair = vb((entry & 0x7FF) * 32 + fy * 4 + (fx >> 1));
    int pixel = (fx & 1) != 0 ? pair & 15 : pair >> 4;
    return pixel != 0 ? ((entry >> 9) & 0x30) | pixel : 0;
}

int plane_pixel(int base, int x, int y, int width_cells, int height_cells, out bool priority)
{
    x &= width_cells * 8 - 1;
    y &= height_cells * 8 - 1;
    int at = (base + ((y >> 3) * width_cells + (x >> 3)) * 2) & 0xFFFF;
    return entry_pixel(vb(at) << 8 | vb(at | 1), x, y, priority);
}

int plane_size(int s) { return s == 1 ? 64 : s == 3 ? 128 : 32; }

uint rot_word(int column) { return texelFetch(rot_tex, ivec2(column, 0), 0).r; }

// Smooth Animation's spinners: plane pixel `p` at plane (x, y) (the table at `base`), with `sx`
// the texel across the pixel; inside a spinner's disc, the pixel of the picture in VRAM turned
// back by the spinner's angle. Sets `spun` when it did, and `from` to where that pixel lies:
// its plane x and y, and the texel within it across and down (0-3, for an art pack's picture).
int spin_pixel(int plane, int base, int x, int y, int w, int h, int sx, int p, out bool spun, out ivec4 from)
{
    spun = false;
    from = ivec4(0);
    int n = int(rot_word(128));
    if (n == 0)
        return p;
    int e = plane_entry(base, x, y, w, h), t = e & 0x7FF;
    for (int k = 0; k < n; k++) {
        int at = 129 + k * 304, first = int(rot_word(at));
        if (t < first || t >= first + 16 || int(rot_word(at + 14)) != plane)
            continue;
        int i = t - first;
        int cell = int((rot_word(at + 1 + (i >> 3)) >> uint((i & 7) * 4)) & 15u);
        // In the picture's own coordinates (its tiles' pixels as stored), so flipped entries
        // (stage 8 upside down) turn mirrored, as the game's picture is.
        bool hf = (e & 0x0800) != 0, vf = (e & 0x1000) != 0;
        float ax = (float(sx) + 0.5) / float(scale), ay = (float(sub_y) + 0.5) / float(vscale);
        float fx = float((cell & 3) * 8 + (hf ? 7 - (x & 7) : (x & 7))) + (hf ? 1.0 - ax : ax);
        float fy = float((cell >> 2) * 8 + (vf ? 7 - (y & 7) : (y & 7))) + (vf ? 1.0 - ay : ay);
        float cx = uintBitsToFloat(rot_word(at + 5)), cy = uintBitsToFloat(rot_word(at + 6));
        float r = uintBitsToFloat(rot_word(at + 7));
        float dx = fx - cx, dy = fy - cy;
        int ix = int(fx), iy = int(fy);
        uint solid = rot_word(at + 15);
        if (dx * dx + dy * dy > r * r || (solid == 0u && ((rot_word(at + 16 + iy) >> uint(ix)) & 1u) == 0u))
            return p;                               // (the block's frame: it stays put)
        float co = uintBitsToFloat(rot_word(at + 8)), si = uintBitsToFloat(rot_word(at + 9));
        float uf = co * dx + si * dy + cx, vf2 = co * dy - si * dx + cy;
        int u = int(floor(uf)), v = int(floor(vf2));
        bool inside = u >= 0 && v >= 0 && u < 32 && v < 32;
        if (solid == 0u && (!inside || ((rot_word(at + 16 + v) >> uint(u)) & 1u) == 0u))
            return p;
        int pixel = 0, tile = 0, c = 0;
        bool single = (solid & 0x10000u) != 0u;
        solid &= 0xFFFFu;
        if (inside && single)                       // (one picture turned all the way round)
            pixel = int((rot_word(at + 48 + v * 8 + (u >> 2)) >> uint((u & 3) * 8 + 4)) & 15u);
        else if (inside) {
            c = (u >> 3) | (v >> 3) << 2;
            tile = first + int((rot_word(at + 3 + (c >> 3)) >> uint((c & 7) * 4)) & 15u);
            int pair = vb(tile * 32 + (v & 7) * 4 + ((u & 7) >> 1));
            pixel = (u & 1) != 0 ? pair & 15 : pair >> 4;
        }
        // Only the solid colours turn (a gear, not the wall between its teeth): elsewhere, what
        // the game's pictures show behind it there (none known: as it is).
        if (solid != 0u && (!inside || ((solid >> uint(pixel)) & 1u) == 0u)) {
            int behind = int((rot_word(at + 48 + iy * 8 + (ix >> 2)) >> uint((ix & 3) * 8)) & 15u);
            if (behind == 0)
                return p;
            spun = true;
            from = ivec4(-1);                       // (no art pack picture: the game's colour)
            return ((e >> 9) & 0x30) | behind;
        }
        // Shading stays put: the turned colour unshaded, then shaded where this pixel is in shadow.
        int here = p & 15;
        pixel = int((rot_word(at + 10 + (pixel >> 3)) >> uint((pixel & 7) * 4)) & 15u);   // (unshade)
        if (int((rot_word(at + 10 + (here >> 3)) >> uint((here & 7) * 4)) & 15u) != here)   // (here is shaded)
            pixel = int((rot_word(at + 12 + (pixel >> 3)) >> uint((pixel & 7) * 4)) & 15u);
        spun = true;
        if (single) {
            from = ivec4(-1);                       // (an art pack's pictures are the game's three)
            return pixel != 0 ? ((e >> 9) & 0x30) | pixel : 0;
        }
        // (back to the plane: the source's cell from this one's, mirrored with the flips)
        int dcx = (u >> 3) - (cell & 3), dcy = (v >> 3) - (cell >> 2);
        int px = ((x >> 3) + (hf ? -dcx : dcx)) * 8 + (hf ? 7 - (u & 7) : (u & 7));
        int py = ((y >> 3) + (vf ? -dcy : dcy)) * 8 + (vf ? 7 - (v & 7) : (v & 7));
        int qx = min(int(fract(uf) * 4.0), 3), qy = min(int(fract(vf2) * 4.0), 3);
        from = ivec4(px, py, hf ? 3 - qx : qx, vf ? 3 - qy : qy);
        return pixel != 0 ? ((e >> 9) & 0x30) | pixel : 0;
    }
    return p;
}

// Smooth Animation's drums (Vdp::Drum): plane pixel `p` at plane (x, y) of the table at
// `base`; on a drum, the pixel its teeth turned on bring here where that is a gap (or its
// edge: not its place's face colour), else this place's face colour (its lighting). Sets
// `turned` when inside one, and `from` to the pixel whose art it shows (for a pack; x -1:
// none, the game's colour).
int drum_pixel(int base, int x, int y, int w, int h, int sx, int p, out bool turned, out ivec4 from)
{
    turned = false;
    from = ivec4(0);
    int n = int(rot_word(1003));
    for (int k = 0; k < n; k++) {
        int at = 1004 + k * 6;
        uint pos = rot_word(at), size = rot_word(at + 1), tiles = rot_word(at + 3);
        int dw = int(size & 0xFFFFu), dh = int(size >> 16);
        int lx = (x - int(pos & 0xFFFFu)) & (w * 8 - 1), ly = (y - int(pos >> 16)) & (h * 8 - 1);
        if (lx >= dw || ly >= dh)
            continue;
        int t = plane_entry(base, x, y, w, h) & 0x7FF, first = int(tiles & 0xFFFFu);
        if (t < first || t >= first + int(tiles >> 16) || p == 0)
            return p;
        float rr = float(dw) * 0.5;
        float fx = float(lx) + (float(sx) + 0.5) / float(scale);
        float th = asin(clamp((fx - rr) / rr, -1.0, 1.0)) + uintBitsToFloat(rot_word(at + 2));
        if (abs(th) >= 1.5707)
            return p;                               // (turned past its edge: as it is)
        float ux = rr + rr * sin(th);
        int sx2 = clamp(int(floor(ux)), 0, dw - 1);
        bool pr;
        int q = plane_pixel(base, x - lx + sx2, y, w, h, pr);
        int face_there = int(texelFetch(rot_tex, ivec2(864 + sx2, 1 + ly), 0).r);
        int face_here = int(texelFetch(rot_tex, ivec2(864 + lx, 1 + ly), 0).r);
        turned = true;
        if (q == 0 || face_there == 0 || face_here == 0)
            return p;
        if ((q & 15) != face_there) {               // a gap: it moves
            from = ivec4(x - lx + sx2, y, min(int(fract(ux) * 4.0), 3), sub_y);
            return q;
        }
        from = ivec4(-1);                           // (a pack's picture here may hold a gap)
        return (p & 0x30) | face_here;
    }
    return p;
}

)" R"(
// Smooth Animation's window shutters (Vdp::Shutter): plane pixel `p` at plane (x, y) of the
// table at `base`; in a shutter's tiles, its doors swung to their angle in front of the open
// window. Sets `swung` when it did (the game's colours: no art pack picture).
uint shutter_word(int i) { return texelFetch(rot_tex, ivec2(864 + i, 33), 0).r; }
int shutter_picture(int k, int x, int y)
{
    return int((texelFetch(rot_tex, ivec2(864 + y * 4 + (x >> 3), 34 + k), 0).r >> uint((x & 7) * 4)) & 15u);
}
int shutter_pixel(int base, int x, int y, int w, int h, int sx, int p, out bool swung)
{
    swung = false;
    if (shutter_word(0) == 0u)
        return p;
    int e = plane_entry(base, x, y, w, h), t = e & 0x7FF, first = int(shutter_word(1));
    if (t < first || t >= first + 16)
        return p;
    int i = t - first;
    int cell = int((shutter_word(2 + (i >> 3)) >> uint((i & 7) * 4)) & 15u);
    bool hf = (e & 0x0800) != 0, vf = (e & 0x1000) != 0;   // (its own coordinates, as spin_pixel)
    float ax = (float(sx) + 0.5) / float(scale), ay = (float(sub_y) + 0.5) / float(vscale);
    float fx = float((cell & 3) * 8 + (hf ? 7 - (x & 7) : (x & 7))) + (hf ? 1.0 - ax : ax);
    float fy = float((cell >> 2) * 8 + (vf ? 7 - (y & 7) : (y & 7))) + (vf ? 1.0 - ay : ay);
    float co = uintBitsToFloat(shutter_word(4)), si = uintBitsToFloat(shutter_word(5));
    uint geo = shutter_word(6), cols = shutter_word(10);
    int sill = int(geo >> 24);
    float dist = uintBitsToFloat(shutter_word(7)), mid = uintBitsToFloat(shutter_word(8));
    float k = uintBitsToFloat(shutter_word(9));
    int wall = int(cols & 15u), shadow = int((cols >> 4) & 15u), back = int((cols >> 8) & 15u), edge = int((cols >> 12) & 15u);
    int colour = shutter_picture(1, int(fx), int(fy));
    float best = -1.0;
    int door = -1;
    for (int side = 0; side < 2; side++) {
        // A door: the picture's column u from its hinge, turned toward the viewer (screen across
        // u cos s, s = dist / (dist - u sin)), so u = across * dist / (dist cos + across sin).
        float hinge = float(side == 0 ? int(geo & 255u) : int((geo >> 16) & 255u));
        int width = side == 0 ? int((geo >> 8) & 255u) - int(geo & 255u) : int((geo >> 16) & 255u) - int((geo >> 8) & 255u);
        float across = side == 0 ? fx - hinge : hinge - fx;
        float den = dist * co + across * si;
        if (abs(den) < 1e-5)
            continue;
        float u = across * dist / den;
        if (u < 0.0 || u >= float(width))
            continue;
        float z = u * si;
        float v = mid + (fy - mid) * (dist - z) / dist;
        int vi = int(floor(v)), ui = int(u);
        if (vi < 0 || vi >= sill)
            continue;
        int c = shutter_picture(0, side == 0 ? int(hinge) + ui : int(hinge) - 1 - ui, vi);
        if (c == 0 || c == wall || c == shadow)
            continue;
        if (co < 0.0)                               // (past a right angle: its back)
            c = ui >= width - 1 ? edge : back;
        if (z > best)
            best = z, door = c;
    }
    if (door >= 0)
        colour = door;
    else if (colour == wall)
        for (int side = 0; side < 2 && colour == wall; side++) {
            // Its shadow on the wall: a door pixel u out, k u sin further right.
            float hinge = float(side == 0 ? int(geo & 255u) : int((geo >> 16) & 255u));
            int width = side == 0 ? int((geo >> 8) & 255u) - int(geo & 255u) : int((geo >> 16) & 255u) - int((geo >> 8) & 255u);
            float a = (side == 0 ? co : -co) + k * si;
            if (abs(a) < 1e-5)
                continue;
            float u = (fx - hinge) / a;
            if (u < 0.0 || u > float(width) || u * si <= 0.5)
                continue;
            int vi = int(floor(fy - k * 0.5 * u * si)), ui = min(int(u), width - 1);
            if (vi < 0 || vi >= sill)
                continue;
            int c = shutter_picture(0, side == 0 ? int(hinge) + ui : int(hinge) - 1 - ui, vi);
            if (c != 0 && c != wall && c != shadow)
                colour = shadow;
        }
    swung = true;
    return colour != 0 ? ((e >> 9) & 0x30) | colour : 0;
}

)" R"(
// Smooth Animation's chains (Vdp::Chain): plane pixel `p` at plane (x, y) of the table at
// `base`; near a swinging chain, its links bent to where they hang now, and its own pixels left
// behind cleared. Sets `moved` when it did, and `from` to the link pixel shown (plane x, y,
// texel across and down, for a pack; x -1: none).
uint chain_word(int k, int i) { return texelFetch(rot_tex, ivec2(864 + i % 160, 52 + k * 4 + i / 160), 0).r; }
bool chain_tile(int t) { return t >= 0x168 && t <= 0x171; }
int chain_pixel(int base, int x, int y, int w, int h, int sx, int p, out bool moved, out ivec4 from)
{
    moved = false;
    from = ivec4(0);
    int n = int(texelFetch(rot_tex, ivec2(910, 33), 0).r);
    int pw = w * 8, ph = h * 8;
    for (int k = 0; k < n; k++) {
        uint pos = chain_word(k, 0);
        int len = int(chain_word(k, 1)), anchor = int(chain_word(k, 2));
        int ix = x - int(pos & 0xFFFFu), iy = y - int(pos >> 16);   // (nearest way round the plane)
        ix -= pw * int(floor(float(ix + pw / 2) / float(pw)));
        iy -= ph * int(floor(float(iy + ph / 2) / float(ph)));
        if (iy < 0 || iy >= len || abs(ix) > 140)
            continue;
        float half_w = iy >= anchor ? 16.0 : 8.0;     // (the anchor 32 wide, the links 16)
        float fx = float(ix) + (float(sx) + 0.5) / float(scale);
        float u = (fx - uintBitsToFloat(chain_word(k, 8 + 2 * iy))) * cos(uintBitsToFloat(chain_word(k, 9 + 2 * iy)));
        bool mine = chain_tile(plane_entry(base, x, y, w, h) & 0x7FF);
        if (p != 0 && !mine)
            continue;                               // (a wall in front of it)
        if (u >= -half_w && u < half_w) {
            int src = int(pos & 0xFFFFu) + int(floor(u));
            if (chain_tile(plane_entry(base, src, y, w, h) & 0x7FF)) {
                bool pr;
                int q = plane_pixel(base, src, y, w, h, pr);
                if (q != 0) {
                    moved = true;
                    from = ivec4(src, y, min(int(fract(u) * 4.0), 3), sub_y);
                    return q;
                }
            }
        }
        if (fx >= -half_w && fx < half_w && mine) {
            moved = true;                           // (its own place, left behind)
            from = ivec4(-1);
            return 0;
        }
    }
    return p;
}

)" R"(
// Smooth Animation's flower buds (Vdp::Bud): plane pixel `p` at plane (x, y) of the table at
// `base`; in the buds' tiles, the morphed look (the game's colours, no art pack picture). Sets
// `bloomed` when it did.
int bud_pixel(int base, int x, int y, int w, int h, int sx, int p, out bool bloomed)
{
    bloomed = false;
    if (texelFetch(rot_tex, ivec2(900, 33), 0).r == 0u)
        return p;
    int e = plane_entry(base, x, y, w, h), t = e & 0x7FF, first = int(texelFetch(rot_tex, ivec2(901, 33), 0).r);
    if (t < first || t >= first + 4)
        return p;
    int i = t - first;
    bool hf = (e & 0x0800) != 0, vf = (e & 0x1000) != 0;   // (its own coordinates, as spin_pixel)
    float ax = (float(sx) + 0.5) / float(scale), ay = (float(sub_y) + 0.5) / float(vscale);
    float fx = float((i & 1) * 8 + (hf ? 7 - (x & 7) : (x & 7))) + (hf ? 1.0 - ax : ax);
    float fy = float((i >> 1) * 8 + (vf ? 7 - (y & 7) : (y & 7))) + (vf ? 1.0 - ay : ay);
    int tx = clamp(int(fx * 8.0), 0, 127), ty = clamp(int(fy * 8.0), 0, 127);
    uint word = texelFetch(rot_tex, ivec2(864 + (ty & 7) * 16 + (tx >> 3), 36 + (ty >> 3)), 0).r;
    int c = int((word >> uint((tx & 7) * 4)) & 15u);
    bloomed = true;
    return c != 0 ? ((e >> 9) & 0x30) | c : 0;
}

// Smooth Animation's colour-cycled wheels (Vdp::Cycler): plane pixel `p` at plane (x, y) of the
// table at `base`; on a wheel's stripes, the stripe colour found its turn further round. Sets
// `turned` when it did, and `from` to where (plane x, y, texel across and down, for a pack).
int cycle_pixel(int base, int x, int y, int w, int h, int sx, int p, out bool turned, out ivec4 from)
{
    turned = false;
    from = ivec4(0);
    int n = int(rot_word(940));                     // (at most 4)
    for (int k = 0; k < n; k++) {
        int at = 941 + k * 14;
        uint set = rot_word(at + 5);
        if (p == 0 || (p >> 4) != int(set >> 16) || ((set >> uint(p & 15)) & 1u) == 0u)
            continue;
        int pw = w * 8, ph = h * 8;
        float shift = uintBitsToFloat(rot_word(at + 11));   // (the turn, in stripes or segments)
        for (int part = 0; part < 2; part++) {      // the rim, then the spokes
            uint pos = rot_word(part == 0 ? at + 12 : at);
            int ix = x - int(pos & 0xFFFFu), iy = y - int(pos >> 16);   // (nearest way round the plane)
            ix -= pw * int(floor(float(ix + pw / 2) / float(pw)));
            iy -= ph * int(floor(float(iy + ph / 2) / float(ph)));
            float dx = float(ix) + (float(sx) + 0.5) / float(scale), dy = float(iy) + (float(sub_y) + 0.5) / float(vscale);
            float r = sqrt(dx * dx + dy * dy);
            float stripe;                           // which stripe, from the first colour's
            if (part == 0) {
                if (r < uintBitsToFloat(rot_word(at + 7)) || r >= uintBitsToFloat(rot_word(at + 8)))
                    continue;
                float segs = uintBitsToFloat(rot_word(at + 9));
                stripe = (atan(dy, dx) - uintBitsToFloat(rot_word(at + 10))) / 6.2831853 * segs;
                stripe -= segs * floor(stripe / segs);
            } else {
                if (r < uintBitsToFloat(rot_word(at + 3)) || r >= uintBitsToFloat(rot_word(at + 4)))
                    continue;
                stripe = (atan(dy, dx) - uintBitsToFloat(rot_word(at + 2))) / uintBitsToFloat(rot_word(at + 1));
            }
            int i = int(floor(stripe + shift));
            i -= 3 * int(floor(float(i) / 3.0));
            turned = true;
            from = ivec4(-1);                       // (drawn from the angle: the game's colours)
            return (p & 0x30) | int((rot_word(at + 6) >> uint(i * 4)) & 15u);
        }
    }
    return p;
}

// Smooth Animation's gliders (Vdp::Glider): plane pixel `p` at plane (x, y); inside a glider's
// 2 x 3 cells, its picture moved by its offset (see-through where nothing moved in). Sets
// `glided` when inside one, `e` to the entry drawn there (-1: none) and `from` to that pixel's
// place within the entry's picture: plane x and y, and the texel across and down (0-3).
int glide_pixel(int x, int y, int w, int h, int sx, int p, inout bool pri, out bool glided, out int e, out ivec4 from)
{
    glided = false;
    e = -1;
    from = ivec4(0);
    int n = int(rot_word(840));
    for (int k = 0; k < n; k++) {
        int at = 841 + k * 8;
        uint pos = rot_word(at);
        int gx = int(pos & 0xFFFFu), gy = int(pos >> 16);
        int lx = (x - gx) & (w * 8 - 1), ly = (y - gy) & (h * 8 - 1);
        if (lx >= 16 || ly >= 24)
            continue;
        glided = true;
        float fy = float(ly) + (float(sub_y) + 0.5) / float(vscale) - uintBitsToFloat(rot_word(at + 1));
        if (fy < 0.0 || fy >= 24.0)
            return 0;
        int sy = int(floor(fy));
        uint pair = rot_word(at + 2 + (sy >> 3));
        e = int(((lx >> 3) != 0 ? pair >> 16 : pair) & 0xFFFFu);
        from = ivec4(gx + lx, gy + sy, sx, min(int(fract(fy) * 4.0), 3));
        return entry_pixel(e, lx, sy, pri);
    }
    return p;
}

// A plane pixel `p` at plane (x, y) of the table at `base`, with an art pack: its picture's
// texel (sx: the texel within the game pixel, 0-3; sub_y the row), true with its colour in c.
// p becomes 0 where the picture is see-through and is shown (nonzero) where it isn't.
// `blocks`: hd_table's first row of the plane's cells' block pictures (1 plane A, 3 plane B, 6
// the window), 0 none.
// hd_plane with the tile entry given (a widescreen strip's remapped pixel: its own entry, which
// may come from a cache rather than the plane's table, and its plane coordinates). Block
// pictures only where the plane's table holds that entry there.
bool hd_plane_entry(int e, int base, int x, int y, int w, int h, int sx, int blocks, inout int p, out uint c)
{
    c = 0u;
    if (hd_on == 0)
        return false;
    int n = 8 * hd_scale;
    int tx = (x & 7) * hd_scale + sx * hd_scale / 4, ty = (y & 7) * hd_scale + sub_y * hd_scale / 4;
    if ((e & 0x0800) != 0) tx = n - 1 - tx;
    if ((e & 0x1000) != 0) ty = n - 1 - ty;
    uint block_slot = 0u;
    if (blocks != 0 && w * h <= 4096 && plane_entry(base, x, y, w, h) == e) {
        int cell = ((y & (h * 8 - 1)) >> 3) * w + ((x & (w * 8 - 1)) >> 3);
        block_slot = texelFetch(hd_table, ivec2(cell % 2048, blocks + cell / 2048), 0).r;
    }
    bool clear;
    if (!hd_texel(e, block_slot, tx, ty, (e >> 13) & 3, p & 15, base, x, y, w, h, c, clear))
        return false;
    if (clear) {
        p = 0;
        return false;
    }
    if (p == 0)
        p = ((e >> 9) & 0x30) | 1;
    return true;
}

bool hd_plane(int base, int x, int y, int w, int h, int sx, int blocks, inout int p, out uint c)
{
    return hd_plane_entry(plane_entry(base, x, y, w, h), base, x, y, w, h, sx, blocks, p, c);
}

)" R"(
int floor_mod(int a, int b) { int r = a - b * (a / b); return r < 0 ? r + b : r; }   // (GLSL's % is undefined below 0)
int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
uint strip_at(int x) { return strip_on != 0 ? texelFetch(strips, ivec2(x + strip_left, row), 0).r : 0u; }

// A layer's hidden pixels take the colour of the front layer where both move alike
// (main.cpp's render_layer_line); then this pass's layers go out.
void fill_and_put(uint l[7], bool window_line, uint w0, uint w1)
{
    int shift_a = window_line ? 0 : int(L(284)), shift_s = int(L(285)), shift_b = int(L(286)), shift_sb = int(L(287));
    int moves[7] = int[7](shift_b, shift_a, shift_s, shift_b, shift_a, shift_sb, shift_s);
    int top = 6;
    while (top > 0 && l[top] == 0u)
        top--;
    for (int i = 0; i < top; i++)
        if (moves[i] == moves[top])
            l[i] = l[top];
    put_layers(l, w0, w1);
}

void water_prepass()
{
    frag = vec4(0.0);
    frag1 = vec4(0.0);
    frag2 = vec4(0.0);
    frag3 = vec4(0.0);
    uint w0 = L(0), w1 = L(1), w2 = L(2), w3 = L(3);
    copy = int(L(56) >> 8);
    int r1 = int(w0 & 255u), r2 = int((w0 >> 8) & 255u), r11 = int((w1 >> 8) & 255u), r16 = int(w1 >> 24);
    int r18 = int((w2 >> 8) & 255u);
    int window_vp = (r18 & 0x1F) * 8;
    bool window_line = (r18 & 0x80) != 0 ? line >= window_vp : line < window_vp;
    if ((r1 & 0x40) == 0 || (r11 & 4) != 0 || window_line)
        return;
    int plane_w = plane_size(r16 & 3), plane_h = plane_size((r16 >> 4) & 3);
    int base_a = (r2 & 0x38) << 10;
    int hs_a = int(w3 & 0xFFFFu), vs_a = half_word(4, 0);
    int xa = int(gl_FragCoord.x) - kWaterLeft - view_left;
    int px = xa - hs_a, py = line + vs_a, e, a;
    uint sw = strip_a_word(xa, 1);
    if ((sw & 0x80000000u) != 0u) {             // remapped by a strip rule
        e = int(sw & 0xFFFFu);
        a = int((sw >> 16) & 127u);
        uint p = strip_a_word(xa, 3);
        px = int(p & 0xFFFFu), py = int(p >> 16);
    } else {
        e = plane_entry(base_a, px, py, plane_w, plane_h);
        bool pri;
        a = entry_pixel(e, px, py, pri);
    }
    uint pic;
    int kind = water_kind(e, a, px, py, pic);
    if (kind == 0)
        return;
    int alpha;
    uint tint, behind;
    bool has_behind;
    water_at(xa, kind, pic, hs_a, vs_a, base_a, plane_w, plane_h, alpha, tint, has_behind, behind);
    frag = unpack(uint(alpha) << 24 | (tint & 0xFFFFFFu));
    frag1 = unpack((has_behind ? 3u : 1u) << 24 | (behind & 0xFFFFFFu));
}

void main()
{
    int ox = int(gl_FragCoord.x) - start;
    int fy = int(gl_FragCoord.y);
    row = fy / vscale;
    sub_y = fy - row * vscale;
    line = int(L(219));
    if (layered == 4) {
        water_prepass();
        return;
    }
    uint l[7] = uint[7](0u, 0u, 0u, 0u, 0u, 0u, 0u);
    if (ox < 0 || ox >= count) {
        if (layered != 0) {
            l[0] = black_border != 0 ? 0xFF000000u : 0u;
            put_layers(l, 0u, 0u);
        } else {
            frag = vec4(0.0, 0.0, 0.0, 1.0);
        }
        return;
    }
    uint w0 = L(0), w1 = L(1), w2 = L(2), w3 = L(3);
    copy = int(L(56) >> 8);
    int r1 = int(w0 & 255u), r2 = int((w0 >> 8) & 255u), r3 = int((w0 >> 16) & 255u), r4 = int(w0 >> 24);
    int r7 = int(w1 & 255u), r11 = int((w1 >> 8) & 255u), r12 = int((w1 >> 16) & 255u), r16 = int(w1 >> 24);
    int r17 = int(w2 & 255u), r18 = int((w2 >> 8) & 255u), window_period = int(w2 >> 16);
    if ((r1 & 0x40) == 0) {                         // display disabled
        if (layered != 0)
            put_layers(l, 0u, 0u);
        else
            frag = color(r7);
        return;
    }
    bool h40 = (r12 & 1) != 0;
    int native_width = h40 ? 320 : 256;
    int plane_w = plane_size(r16 & 3), plane_h = plane_size((r16 >> 4) & 3);
    int base_a = (r2 & 0x38) << 10, base_b = (r4 & 7) << 13;
    int base_window = h40 ? (r3 & 0x3C) << 10 : (r3 & 0x3E) << 10;
    int hs_a = int(w3 & 0xFFFFu), hs_b = int(w3 >> 16);
    bool column_vscroll = (r11 & 4) != 0;
    int window_vp = (r18 & 0x1F) * 8;
    bool window_line = (r18 & 0x80) != 0 ? line >= window_vp : line < window_vp;
    int window_hp = (r17 & 0x1F) * 16;
    bool window_right = (r17 & 0x80) != 0;
    int window_w = h40 ? 64 : 32;

    // x: the unshifted position (the window), xa and xb plane A's and B's (render_line).
    int sub_a = int(L(217)), sub_b = int(L(218));
    int x = floor_div(ox, scale) - view_left;
    int xa = floor_div(ox - sub_a, scale) - view_left;
    int xb = floor_div(ox - sub_b, scale) - view_left;
    int column = clamp(xa >> 4, 0, 19);
    int vs_a = half_word(4, column_vscroll ? column * 2 : 0);
    int vs_b = half_word(4, column_vscroll ? column * 2 + 1 : 1);

    // Art packs: the texel within the game pixel, for each plane.
    int sx_a = ox - sub_a - floor_div(ox - sub_a, scale) * scale, sx_b = ox - sub_b - floor_div(ox - sub_b, scale) * scale;
    int sx_w = ox - floor_div(ox, scale) * scale;
    uint hd_a = 0u, hd_b = 0u, hd_s = 0u;
    bool a_hd = false, b_hd = false, s_hd = false;

    bool pri_a, pri_b;
    int b;
    uint sw_b = strip_at(xb);
    if ((sw_b & 0x40000000u) != 0u) {               // remapped by a strip rule
        b = int((sw_b >> 16) & 127u);
        pri_b = (sw_b & 0x800000u) != 0u;
        uint here_b = strip_a_word(xb, 4);          // (art packs: its tile and plane coordinates)
        if (hd_on != 0 && (here_b & 0x80000000u) != 0u) {
            uint at = strip_a_word(xb, 5);
            b_hd = hd_plane_entry(int(here_b & 0xFFFFu), base_b, int(at & 0xFFFFu), int(at >> 16), plane_w, plane_h, sx_b, 3, b, hd_b);
        }
    } else {
        b = plane_pixel(base_b, xb - hs_b, line + vs_b, plane_w, plane_h, pri_b);
        b_hd = hd_plane(base_b, xb - hs_b, line + vs_b, plane_w, plane_h, sx_b, 3, b, hd_b);
        bool spun;                                  // Smooth Animation's spinners (stage 26's gears)
        ivec4 from;
        int sb = spin_pixel(1, base_b, xb - hs_b, line + vs_b, plane_w, plane_h, sx_b, b, spun, from);
        if (spun) {
            b = sb;
            int row = sub_y;
            sub_y = from.w;
            b_hd = from.z >= 0 && hd_plane(base_b, from.x, from.y, plane_w, plane_h, from.z, 3, b, hd_b);
            sub_y = row;
        }
    }
    uint sw = strip_at(xa);
    int a, a_game = 0;                              // (a_game: the game's own pixel, before a pack's)
    bool front = false;                             // a strip fill drawn in front of everything
    bool in_native = x >= 0 && x < native_width;
    bool in_window = in_native && (window_line || (window_right ? x >= window_hp : x < window_hp));
    if (in_window) {
        a = plane_pixel(base_window, x, line, window_w, 32, pri_a);
        a_hd = hd_plane(base_window, x, line, window_w, 32, sx_w, 6, a, hd_a);
    } else if (window_line)                           // past the picture's edges: the HUD extended
        a = plane_pixel(base_window, window_period != 0 ? floor_mod(x, window_period)
                                                        : (x < 0 ? 0 : native_width - 8) + (x & 7),
                        line, window_w, 32, pri_a);
    else if ((sw & 0x80000000u) != 0u) {            // remapped by a strip rule
        a = int(sw & 127u);
        a_game = a;
        pri_a = (sw & 0x80u) != 0u;
        // Art packs: the remapped pixel's own tile and plane coordinates (strip_a's words).
        uint here = strip_a_word(xa, 1);
        if (hd_on != 0 && (here & 0x80000000u) != 0u) {
            uint at = strip_a_word(xa, 3);
            a_hd = hd_plane_entry(int(here & 0xFFFFu), base_a, int(at & 0xFFFFu), int(at >> 16), plane_w, plane_h, sx_a, 1, a, hd_a);
        }
        if ((sw & 0x100u) != 0u && a != 0) {        // in front of everything
            if (layered != 0) {
                front = true;
            } else {
                frag = a_hd ? unpack(hd_a) : color(a);
                return;
            }
        }
    } else {
        a = plane_pixel(base_a, xa - hs_a, line + vs_a, plane_w, plane_h, pri_a);
        a_game = a;
        // Smooth Animation's plane A effects, each from the game's pixel; the last that applies
        // gives the pixel, and `at` where an art pack's texel comes from (plane x, y, texel
        // across and down; across -1: none, the game's colour; plane x and y may be below 0,
        // scrolled) and `at_e` its entry (-1: the plane's).
        // (One art pack lookup for all of them: each one inlined made the shader too long.)
        ivec4 at = ivec4(xa - hs_a, line + vs_a, sx_a, sub_y), from;
        int at_e = -1;
        bool hit;
        int fa = spin_pixel(0, base_a, xa - hs_a, line + vs_a, plane_w, plane_h, sx_a, a_game, hit, from);
        if (hit)                                    // spinners
            a = a_game = fa, at = from;
        fa = shutter_pixel(base_a, xa - hs_a, line + vs_a, plane_w, plane_h, sx_a, a_game, hit);
        if (hit)                                    // window shutters
            a = a_game = fa, at = ivec4(-1);
        fa = chain_pixel(base_a, xa - hs_a, line + vs_a, plane_w, plane_h, sx_a, a_game, hit, from);
        if (hit)                                    // chains
            a = a_game = fa, at = from;
        fa = bud_pixel(base_a, xa - hs_a, line + vs_a, plane_w, plane_h, sx_a, a_game, hit);
        if (hit)                                    // flower buds
            a = a_game = fa, at = ivec4(-1);
        fa = drum_pixel(base_a, xa - hs_a, line + vs_a, plane_w, plane_h, sx_a, a_game, hit, from);
        if (hit && fa != a_game)                    // drums
            a = a_game = fa, at = from;
        fa = cycle_pixel(base_a, xa - hs_a, line + vs_a, plane_w, plane_h, sx_a, a_game, hit, from);
        if (hit)                                    // colour-cycled wheels
            a = a_game = fa, at = from;
        int ge;
        fa = glide_pixel(xa - hs_a, line + vs_a, plane_w, plane_h, sx_a, a_game, pri_a, hit, ge, from);
        if (hit) {                                  // gliders
            a = a_game = fa, at = ge >= 0 ? from : ivec4(-1);
            at_e = ge;
        }
        if (at.z >= 0) {
            int row = sub_y;
            sub_y = at.w;
            a_hd = hd_plane_entry(at_e >= 0 ? at_e : plane_entry(base_a, at.x, at.y, plane_w, plane_h),
                                  base_a, at.x, at.y, plane_w, plane_h, at.z, 1, a, hd_a);
            sub_y = row;
        }
    }

)" R"(
    int s = 0;
    bool pri_s = false, s_on_b = false;
    int count = front ? 0 : int(L(56) & 255u);
    for (int i = 0; i < count; i++) {
        uint e = L(57 + 2 * i);
        int cells_w, cells_h, cx, attr, row, sxs, sys;
        if ((e & 0x80000000u) != 0u) {          // Smooth Animation: a turned picture
            int k = int(e & 7u);
            float cy = float(int((e >> 8) & 0xFFFFu) - 32768) / 16.0;
            float cxt = float(int(L(58 + 2 * i))) / 16.0;
            float px = (float(ox) + 0.5 - cxt) / float(scale);
            float py = float(line) + (float(sub_y) + 0.5) / float(vscale) - cy;
            float co = uintBitsToFloat(texelFetch(rot_tex, ivec2(k * 16, 0), 0).r);
            float si = uintBitsToFloat(texelFetch(rot_tex, ivec2(k * 16 + 1, 0), 0).r);
            float u = co * px + si * py + uintBitsToFloat(texelFetch(rot_tex, ivec2(k * 16 + 2, 0), 0).r);
            float v = co * py - si * px + uintBitsToFloat(texelFetch(rot_tex, ivec2(k * 16 + 3, 0), 0).r);
            int pw = int(texelFetch(rot_tex, ivec2(k * 16 + 5, 0), 0).r), ph = int(texelFetch(rot_tex, ivec2(k * 16 + 6, 0), 0).r);
            if (u < 0.0 || v < 0.0 || u >= float(pw) || v >= float(ph))
                continue;
            int clip = int(texelFetch(rot_tex, ivec2(k * 16 + 7, 0), 0).r);
            if (clip != 0) {                        // only where the game's frames draw
                float qx = px - uintBitsToFloat(texelFetch(rot_tex, ivec2(k * 16 + 10, 0), 0).r);
                float qy = py - uintBitsToFloat(texelFetch(rot_tex, ivec2(k * 16 + 11, 0), 0).r);
                int cw = int(texelFetch(rot_tex, ivec2(k * 16 + 8, 0), 0).r), ch = int(texelFetch(rot_tex, ivec2(k * 16 + 9, 0), 0).r);
                if (qx < 0.0 || qy < 0.0 || qx >= float(cw) || qy >= float(ch)
                    || texelFetch(rot_tex, ivec2(clip - 1 + int(qx), 1 + int(qy)), 0).r == 0u)
                    continue;
            }
            uint pword = texelFetch(rot_tex, ivec2(int(texelFetch(rot_tex, ivec2(k * 16 + 4, 0), 0).r) + int(u), 1 + int(v)), 0).r;
            if (pword == 0u)
                continue;
            attr = int(pword & 0xFFFFu);
            cx = int((pword >> 16) & 31u);
            cells_w = int((pword >> 26) & 3u) + 1;
            cells_h = int((pword >> 28) & 3u) + 1;
            row = int((pword >> 21) & 31u);
            if ((attr & 0x1000) != 0)
                row = cells_h * 8 - 1 - row;
            sxs = min(int(fract(u) * float(hd_scale)), hd_scale - 1);
            sys = min(int(fract(v) * float(hd_scale)), hd_scale - 1);
        } else {
            cells_w = int((e >> 24) & 3u) + 1;
            cells_h = int((e >> 26) & 3u) + 1;
            int first = int(L(58 + 2 * i));
            if (ox < first)
                continue;
            cx = (ox - first) / scale;
            if (cx >= cells_w * 8)
                continue;
            attr = int(e & 0xFFFFu);
            row = int((e >> 16) & 31u);
            sxs = (ox - first - cx * scale) * hd_scale / 4;
            sys = sub_y * hd_scale / 4;
        }
        int col = (attr & 0x0800) != 0 ? cells_w * 8 - 1 - cx : cx;
        int tile = (attr & 0x7FF) + (col >> 3) * cells_h + (row >> 3);
        int pair = vb(tile * 32 + (row & 7) * 4 + ((col & 7) >> 1));
        int pixel = (col & 1) != 0 ? pair & 15 : pair >> 4;
        if (hd_on != 0) {                       // an art pack's picture of the tile
            int tx = (col & 7) * hd_scale + ((attr & 0x0800) != 0 ? hd_scale - 1 - sxs : sxs);
            int ty = (row & 7) * hd_scale + ((attr & 0x1000) != 0 ? hd_scale - 1 - sys : sys);
            uint c;
            bool clear;
            // The sprite's whole piece, when the pack has it: this tile's picture within it.
            uint piece = texelFetch(hd_table, ivec2(attr & 0x7FF, 5), 0).r, piece_slot = 0u;
            if (piece != 0u && int((piece >> 28) & 3u) == cells_w - 1 && int(piece >> 30) == cells_h - 1)
                piece_slot = (piece & 0x0FFFFFFFu) + uint((col >> 3) * cells_h + (row >> 3));
            if (hd_texel(tile, piece_slot, tx, ty, (attr >> 13) & 3, pixel, 0, 0, 0, 0, 0, c, clear)) {
                if (clear)
                    continue;
                if (pixel == 0)
                    pixel = 1;
                s_hd = true;
                hd_s = c;
            }
        }
        if (pixel == 0)
            continue;
        s = ((attr >> 9) & 0x30) | pixel;
        if (recolor_tiles > 0 && (attr & 0x6000) == 0 && tile >= recolor_tile && tile < recolor_tile + recolor_tiles
            && texelFetch(recolor, ivec2((row & 7) * 8 + (col & 7), tile - recolor_tile), 0).r != 0u)
            s = 64 | pixel;
        // An art pack's red beside them (an upscaler's shorts reach past the game's pixels):
        // recoloured too, so no red shows round Player 2's shorts.
        if (s_hd && s < 64 && recolor_tiles > 0 && (attr & 0x6000) == 0 && tile >= recolor_tile
            && tile < recolor_tile + recolor_tiles) {
            vec4 t = unpack(hd_s);
            if (t.r > 2.0 * t.g + 0.05 && t.r > 2.0 * t.b + 0.05)
                for (int n = 0; n < 9 && s < 64; n++) {
                    int nc = col + n % 3 - 1, nr = row + n / 3 - 1;
                    if (nc < 0 || nr < 0 || nc >= cells_w * 8 || nr >= cells_h * 8)
                        continue;
                    int nt = (attr & 0x7FF) + (nc >> 3) * cells_h + (nr >> 3);
                    if (nt >= recolor_tile && nt < recolor_tile + recolor_tiles
                        && texelFetch(recolor, ivec2((nr & 7) * 8 + (nc & 7), nt - recolor_tile), 0).r != 0u)
                        s = 64 | max(pixel, 1);
                }
        }
        pri_s = (attr & 0x8000) != 0;
        s_on_b = (e & 0x10000000u) != 0u;
        break;
    }

)" R"(
    // render_pixels' additions (lines without per-column scroll): plane B's own colours
    // (Vdp::plane_b_rgb), Smooth Water and tile pictures on plane A, and the overlays.
    uint cb = b != 0 ? color_u(b) : 0u, cs = s != 0 ? color_u(s) : 0u;
    if (s >= 64 && !column_vscroll) {            // recoloured: render_pixels' colours 64-79
        uint c = color_u(s & 15);
        cs = (c & 0xFF00FF00u) | ((c >> 16) & 255u) | (c & 255u) << 16;
    }
    if (b != 0 && !column_vscroll && b_rgb_on != 0) {
        ivec2 size = textureSize(b_rgb, 0);
        if (size.x == plane_w * 8 && size.y == plane_h * 8) {
            uint c = texelFetch(b_rgb, ivec2((xb - hs_b) & (size.x - 1), (line + vs_b) & (size.y - 1)), 0).r;
            if (c != 0u) {
                cb = c;
                b_hd = false;                       // (Smooth Water's blend of the frames: an art pack's
            }                                       // picture of one frame would stop it animating smoothly)
        }
    }
    int kind = 0, alpha = 0;
    uint tint = 0u, behind = 0u;
    bool has_behind = false;
    if (!column_vscroll && !window_line && !in_window && water_pre_on != 0) {
        ivec2 at = ivec2(xa + kWaterLeft + view_left, row);
        uvec4 p0 = uvec4(texelFetch(water_pre0, at, 0) * 255.0 + 0.5);
        uvec4 p1 = uvec4(texelFetch(water_pre1, at, 0) * 255.0 + 0.5);
        kind = int(p1.a & 1u);
        alpha = int(p0.a);
        tint = p0.r << 16 | p0.g << 8 | p0.b;
        has_behind = (p1.a & 2u) != 0u;
        behind = 0xFF000000u | p1.r << 16 | p1.g << 8 | p1.b;
    }
    // Clean Rock Edges: plane A's own dry black pixels (not the window's, nor water).
    // With an art pack, by the game's pixel: where it would be repainted, the texel is the pack
    // picture's at the pixel the colour comes from (or clear). (Skipped there, the rocks kept
    // their black seams, and a pack with them taken out showed the background through.)
    if (edges_on != 0 && kind == 0 && !in_window && !window_line && edge_black(a_game)) {
        int ex = xa - hs_a, ey = line + vs_a;
        if ((sw & 0x80000000u) != 0u) {         // remapped: where the strip rule put it
            uint p = strip_a_word(xa, 3);
            ex = int(p & 0xFFFFu), ey = int(p >> 16);
        }
        int src_dx;
        int r = clean_edge(a_game, base_a, ex, ey, plane_w, plane_h, src_dx);
        bool by_pack = a_hd || a == 0;           // (drawn from a pack: a texel, or a clear one)
        if (!by_pack) {
            a = r;
        } else if (r != a_game) {
            a = r;
            a_hd = false;
            uint c;
            if (r != 0 && hd_plane(base_a, ex + src_dx, ey, plane_w, plane_h, sx_a, 1, a, c)) {
                a_hd = true;
                hd_a = c;
            }
        }
    }
    if (kind != 0)
        a_hd = false;                               // (Smooth Water's water, as for plane B above)
    if (b_hd) cb = hd_b;
    if (s_hd) {
        cs = hd_s;
        // (Co-op's Player 2: its picture's red recoloured alike; only red, since the picture's
        // shapes needn't keep to the game's pixels: its buttons turned blue.)
        vec4 t = unpack(cs);
        if (s >= 64 && t.r > 2.0 * t.g + 0.05 && t.r > 2.0 * t.b + 0.05)
            cs = (cs & 0xFF00FF00u) | ((cs >> 16) & 255u) | (cs & 255u) << 16;
    }
    uint ca = a_hd ? hd_a : color_u(a);
    int overlay_rows = int(L(289));
    uint backdrop = color_u(r7);

    if (kind != 0) {
        // Water: what lies in front of plane A at its priority shows as it is; beneath it,
        // the water is laid over what lies there.
        if (layered != 0) {
            if (b != 0) l[pri_b ? 3 : 0] = cb;
            if (s != 0) l[s_on_b ? 5 : pri_s ? 6 : 2] = cs;
            uint water = alpha != 0 ? uint(alpha) << 24 | (blend(0u, tint, alpha) & 0xFFFFFFu) : 0u;
            if (has_behind)
                l[pri_a ? 4 : 1] = behind;
            fill_and_put(l, window_line, pri_a ? 0u : water, pri_a ? water : 0u);
            return;
        }
        bool over_priority;
        uint over = overlay_rows > 0 && s == 0 ? overlay_at(ox, overlay_rows, over_priority) : 0u;
        if (has_behind) {                       // plane A paints what lies beneath
            frag = unpack(s != 0 && (pri_s || !pri_a) ? cs : !pri_a && b != 0 && pri_b ? cb : blend(behind, tint, alpha));
            return;
        }
        uint front_c = 0u, under = 0u;
        bool has_front = true;
        if (s != 0 && pri_s) front_c = cs;
        else if (!pri_a && b != 0 && pri_b) front_c = cb;
        else if (!pri_a && s != 0) front_c = cs;
        else {
            has_front = false;
            if (b != 0 && pri_b) under = cb;
            else if (s != 0) under = cs;
            else under = cb;
        }
        uint beneath = under != 0u ? under : backdrop;
        if (over != 0u && (over_priority || !(b != 0 && pri_b)))
            beneath = blend(beneath, over, int(over >> 24));
        frag = unpack(has_front ? front_c : blend(beneath, tint, alpha));
        return;
    }

    bool over_priority;
    uint over = overlay_rows > 0 && s == 0 && !front ? overlay_at(ox, overlay_rows, over_priority) : 0u;
    if (layered != 0) {
        if (front) {
            l[6] = color_u(a);
        } else {
            if (b != 0) l[pri_b ? 3 : 0] = cb;
            if (a != 0) l[pri_a ? 4 : 1] = color_u(a);
            if (s != 0) l[s_on_b ? 5 : pri_s ? 6 : 2] = cs;
            else if ((over >> 24) >= 128u) l[over_priority ? 6 : 2] = 0xFF000000u | over;   // (layers keep pixels whole)
        }
        fill_and_put(l, window_line, 0u, 0u);
        return;
    }

    uint out_c;
    if (s != 0 && pri_s) out_c = cs;
    else if (a != 0 && pri_a) out_c = ca;
    else if (b != 0 && pri_b) out_c = cb;
    else if (s != 0) out_c = cs;
    else if (a != 0) out_c = ca;
    else if (b != 0) out_c = cb;
    else out_c = backdrop;
    if ((over >> 24) != 0u && (over_priority || !((a != 0 && pri_a) || (b != 0 && pri_b))))
        out_c = blend(out_c, over, int(over >> 24));
    frag = unpack(out_c);
}
)";

// A shader's whole source: its version line (and on Android its precisions), then `body`.
std::string shader_source(const char* body)
{
#ifdef __ANDROID__
    std::string code = "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                       "precision highp usampler2D;\nprecision highp usampler2DArray;\n";
#else
    std::string code = "#version 330 core\n";
#endif
    return code + body;
}

GLuint compile(GLenum type, const char* body)
{
    const std::string code = shader_source(body);
    const char* source = code.c_str();
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "gpu-vdp: shader compile error:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint integer_texture(GLenum format, int width, int height)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_RED_INTEGER,
                 format == GL_R8UI ? GL_UNSIGNED_BYTE : GL_UNSIGNED_INT, nullptr);
    return t;
}

// --- The shared program (gpu_vdp.h's gpu_shader) -------------------------------------------

#ifndef COI_VERSION
#define COI_VERSION "dev"
#endif

constexpr GLenum GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02;
constexpr GLenum GL_PROGRAM_BINARY_RETRIEVABLE_HINT = 0x8257, GL_PROGRAM_BINARY_LENGTH = 0x8741;
constexpr GLenum GL_NUM_PROGRAM_BINARY_FORMATS = 0x87FE;

// The time a build is expected to take before one has been timed on this device.
#ifdef __ANDROID__
constexpr double kFirstGuess = 40.0;
#else
constexpr double kFirstGuess = 5.0;
#endif

std::string pref_file(const char* name)
{
    char* dir = SDL_GetPrefPath("CastleOfIllusion", "Port");   // (settings.cpp's folder)
    if (!dir)
        return {};
    std::string path = std::string(dir) + name;
    SDL_free(dir);
    return path;
}

// What a saved program must match: this version, the shaders' source and the driver.
std::string program_key()
{
    auto text = [](GLenum name) {
        const unsigned char* s = glGetString(name);
        return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
    };
    const std::string source = shader_source(kVertex) + '\0' + shader_source(kFragment);
    uint64_t hash = 1469598103934665603ull;         // FNV-1a
    for (unsigned char c : source)
        hash = (hash ^ c) * 1099511628211ull;
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hash);
    return std::string("COI GPU renderer program 1\n") + COI_VERSION + "\n" + text(GL_VENDOR) + "\n"
         + text(GL_RENDERER) + "\n" + text(GL_VERSION) + "\n" + hex + "\n";
}

bool binaries_supported()
{
    if (!glGetProgramBinary || !glProgramBinary || !glProgramParameteri)
        return false;
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    return formats > 0;
}

// The saved program, if it matches `key` and the driver takes it; 0 otherwise.
GLuint load_saved(const std::string& key)
{
    SDL_IOStream* file = SDL_IOFromFile(pref_file("gpu_renderer_program.bin").c_str(), "rb");
    if (!file)
        return 0;
    std::vector<uint8_t> data;
    const Sint64 size = SDL_GetIOSize(file);
    if (size > 0 && size < (64 << 20)) {
        data.resize((size_t)size);
        if (SDL_ReadIO(file, data.data(), data.size()) != data.size())
            data.clear();
    }
    SDL_CloseIO(file);
    const size_t head = key.size() + 8;
    if (data.size() < head || std::memcmp(data.data(), key.data(), key.size()) != 0)
        return 0;
    uint32_t format, length;
    std::memcpy(&format, data.data() + key.size(), 4);
    std::memcpy(&length, data.data() + key.size() + 4, 4);
    if (length == 0 || length != data.size() - head)
        return 0;
    GLuint program = glCreateProgram();
    glProgramBinary(program, format, data.data() + head, (GLsizei)length);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {                                      // (a driver may refuse it: then it is built again)
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void save_program(const std::string& key, GLenum format, const std::vector<uint8_t>& binary)
{
    if (binary.empty())
        return;
    SDL_IOStream* file = SDL_IOFromFile(pref_file("gpu_renderer_program.bin").c_str(), "wb");
    if (!file)
        return;
    const uint32_t f = format, length = (uint32_t)binary.size();
    SDL_WriteIO(file, key.data(), key.size());
    SDL_WriteIO(file, &f, 4);
    SDL_WriteIO(file, &length, 4);
    SDL_WriteIO(file, binary.data(), binary.size());
    SDL_CloseIO(file);
}

// The last build's time on this device (kept apart from the program, so a new version's
// estimate still has it).
double saved_build_time()
{
    double seconds = 0;
    if (FILE* f = std::fopen(pref_file("gpu_renderer_build_time.txt").c_str(), "r")) {
        if (std::fscanf(f, "%lf", &seconds) != 1)
            seconds = 0;
        std::fclose(f);
    }
    return seconds > 0.1 && seconds < 3600 ? seconds : kFirstGuess;
}

void save_build_time(double seconds)
{
    if (FILE* f = std::fopen(pref_file("gpu_renderer_build_time.txt").c_str(), "w")) {
        std::fprintf(f, "%.2f\n", seconds);
        std::fclose(f);
    }
}

// Compiles and links the program with the current context; with `binary`, also takes the
// linked program's binary for save_program.
struct Build {
    GLuint program = 0;
    GLenum format = 0;
    std::vector<uint8_t> binary;
    double seconds = 0;
};

// Draws once in each of the renderer's set-ups, with the same targets (render's picture, the
// water's first pass, the two passes of layers, the water layers), off screen. A driver that
// finishes a program for the targets it is drawn to on its first draws (the Shield's) does so
// here, on the building thread, rather than at the GPU renderer's first frame.
void warm_up(GLuint program)
{
    constexpr int kSize = 8;
    GLuint tex[4] = {}, fbo = 0, vao = 0;
    glGenTextures(4, tex);
    for (GLuint t : tex) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glGenFramebuffers(1, &fbo);
    glGenVertexArrays(1, &vao);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindVertexArray(vao);
    glViewport(0, 0, kSize, kSize);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(program);
    // The samplers on their own units, as draw sets them (one unit can't serve two kinds).
    static const char* const kSamplers[] = {"vram", "lines", "strips", "water_mask", "pic_index", "pics", "b_rgb",
                                            "overlay_px", "water_pre0", "water_pre1", "edge_table", "recolor",
                                            "strip_a", "hd_table", "hd_atlas", "rot_tex"};
    for (int i = 0; i < 16; i++)
        glUniform1i(glGetUniformLocation(program, kSamplers[i]), i + 1);
    glUniform1i(glGetUniformLocation(program, "count"), kSize);
    glUniform1i(glGetUniformLocation(program, "scale"), 1);
    glUniform1i(glGetUniformLocation(program, "vscale"), 1);
    glUniform1i(glGetUniformLocation(program, "hd_scale"), 1);
    static const struct { int pass, targets, buffers; } kSetups[5] = {
        {0, 1, 0},                                  // render: the picture (the framebuffer's own draw buffer)
        {4, 2, 2},                                  // water_prepass
        {1, 4, 4},                                  // render_layers' first pass
        {2, 3, 4},                                  // its second (three layers, the fourth target none)
        {3, 2, 2},                                  // the water layers
    };
    for (const auto& setup : kSetups) {
        for (int i = 0; i < 4; i++)
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D,
                                   i < setup.targets ? tex[i] : 0, 0);
        GLenum buffers[4];
        for (int i = 0; i < 4; i++)
            buffers[i] = i < setup.targets ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
        if (setup.buffers)
            glDrawBuffers(setup.buffers, buffers);
        else {
            const GLenum first = GL_COLOR_ATTACHMENT0;
            glDrawBuffers(1, &first);
        }
        glUniform1i(glGetUniformLocation(program, "layered"), setup.pass);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(4, tex);
}

// With `warm`, also draws in each set-up once (warm_up: on the building thread).
void build_program(Build& b, bool binary, bool warm)
{
    const Uint64 started = SDL_GetPerformanceCounter();
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (vs && fs) {
        GLuint program = glCreateProgram();
        if (binary)
            glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, 1);
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            std::fprintf(stderr, "gpu-vdp: shader link error:\n%s\n", log);
            glDeleteProgram(program);
        } else {
            b.program = program;
            GLint length = 0;
            if (binary)
                glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
            if (length > 0) {
                b.binary.resize((size_t)length);
                GLsizei got = 0;
                glGetProgramBinary(program, length, &got, &b.format, b.binary.data());
                b.binary.resize(got > 0 ? (size_t)got : 0);
            }
        }
    }
    if (warm && b.program)
        warm_up(b.program);
    if (vs)
        glDeleteShader(vs);
    if (fs)
        glDeleteShader(fs);
    glFinish();                                     // (done before another context uses it)
    b.seconds = (double)(SDL_GetPerformanceCounter() - started) / (double)SDL_GetPerformanceFrequency();
}

// A context sharing the main one's objects, for a build on a thread of its own: an EGL context
// with a 1 x 1 pbuffer (or none, with EGL_KHR_surfaceless_context) on Android, and a hidden
// window's on the desktop. create and destroy on the main thread, bind and unbind on the
// building thread.
struct WorkerContext {
#ifdef __ANDROID__
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    bool create()
    {
        display = eglGetCurrentDisplay();
        EGLContext share = eglGetCurrentContext();
        if (display == EGL_NO_DISPLAY || share == EGL_NO_CONTEXT)
            return false;
        const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
        const bool surfaceless = extensions && std::strstr(extensions, "EGL_KHR_surfaceless_context");
        EGLConfig config = nullptr;
        EGLint found = 0;
        if (surfaceless) {
            EGLint id = 0;
            eglQueryContext(display, share, EGL_CONFIG_ID, &id);
            const EGLint wanted[] = {EGL_CONFIG_ID, id, EGL_NONE};
            eglChooseConfig(display, wanted, &config, 1, &found);
        } else {
            const EGLint wanted[] = {EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE,
                                     EGL_PBUFFER_BIT, EGL_NONE};
            eglChooseConfig(display, wanted, &config, 1, &found);
        }
        if (found < 1)
            return false;
        const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, share, attributes);
        if (context == EGL_NO_CONTEXT)
            return false;
        if (!surfaceless) {
            const EGLint size[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            surface = eglCreatePbufferSurface(display, config, size);
            if (surface == EGL_NO_SURFACE) {
                destroy();
                return false;
            }
        }
        return true;
    }
    bool bind() { return eglMakeCurrent(display, surface, surface, context) == EGL_TRUE; }
    void unbind() { eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }
    void destroy()
    {
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display, surface);
        context = EGL_NO_CONTEXT;
        surface = EGL_NO_SURFACE;
    }
#else
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;

    bool create()
    {
        SDL_Window* main_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext main_context = SDL_GL_GetCurrentContext();
        if (!main_window || !main_context)
            return false;
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        window = SDL_CreateWindow("", 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        context = window ? SDL_GL_CreateContext(window) : nullptr;   // (made current here: undone below)
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
        SDL_GL_MakeCurrent(main_window, main_context);
        if (!context) {
            destroy();
            return false;
        }
        return true;
    }
    bool bind() { return SDL_GL_MakeCurrent(window, context); }
    void unbind() { SDL_GL_MakeCurrent(window, nullptr); }
    void destroy()
    {
        if (context)
            SDL_GL_DestroyContext(context);
        if (window)
            SDL_DestroyWindow(window);
        context = nullptr;
        window = nullptr;
    }
#endif
};

struct Job {
    WorkerContext context;
    Build build;
    bool bound = false;
    std::atomic<bool> done{false};
    std::thread thread;
};

gpu_shader::State g_state = gpu_shader::State::None;
GLuint g_program = 0;
std::string g_key;
bool g_binaries = false;
Job* g_job = nullptr;
Uint64 g_started = 0;
double g_expected = kFirstGuess;

// A build done (on this thread or another): the program in place, saved for the next start.
gpu_shader::State finish(Build& b, bool background)
{
    if (!b.program) {
        g_state = gpu_shader::State::Failed;
        return g_state;
    }
    g_program = b.program;
    save_program(g_key, b.format, b.binary);
    if (b.seconds >= 0.5)                           // (a driver's own cache can make it instant)
        save_build_time(b.seconds);
    SDL_Log("gpu-vdp: the renderer's program built in %.1f s %s%s", b.seconds,
            background ? "on a thread" : "before the first frame",
            b.binary.empty() ? " (the driver can't save it)" : ", saved for the next start");
    g_state = gpu_shader::State::Ready;
    return g_state;
}

}  // namespace

namespace gpu_shader {

State start(bool background)
{
    if (g_state != State::None)
        return g_state;
    if (!load())
        return g_state = State::Failed;
    g_key = program_key();
    g_binaries = binaries_supported();
    if (g_binaries) {
        if (GLuint program = load_saved(g_key)) {
            g_program = program;
            SDL_Log("gpu-vdp: the renderer's saved program loaded");
            return g_state = State::Ready;
        }
    }
    g_expected = saved_build_time();
    g_started = SDL_GetPerformanceCounter();
    if (background) {
        Job* job = new Job;
        if (job->context.create()) {
            const bool binary = g_binaries;
            job->thread = std::thread([job, binary] {
                job->bound = job->context.bind();
                if (job->bound) {
                    build_program(job->build, binary, true);
                    job->context.unbind();
                }
                job->done = true;
            });
            g_job = job;
            return g_state = State::Building;
        }
        delete job;                                 // (no second context: built here instead)
    }
    Build b;
    build_program(b, g_binaries, false);
    return finish(b, false);
}

State poll()
{
    if (g_state != State::Building || !g_job->done)
        return g_state;
    Job* job = g_job;
    g_job = nullptr;
    job->thread.join();
    job->context.destroy();
    Build b = std::move(job->build);
    const bool bound = job->bound;
    delete job;
    if (!bound)                                     // (the thread's context couldn't be used)
        build_program(b, g_binaries, false);
    return finish(b, bound);
}

State state() { return g_state; }
unsigned program() { return g_program; }

double elapsed()
{
    return (double)(SDL_GetPerformanceCounter() - g_started) / (double)SDL_GetPerformanceFrequency();
}

double expected() { return g_expected; }

bool on_nvidia_shield()
{
#ifdef __ANDROID__
    char manufacturer[PROP_VALUE_MAX] = {}, model[PROP_VALUE_MAX] = {};
    __system_property_get("ro.product.manufacturer", manufacturer);
    __system_property_get("ro.product.model", model);
    return SDL_strcasecmp(manufacturer, "NVIDIA") == 0 && SDL_strcasestr(model, "SHIELD") != nullptr;
#else
    return false;
#endif
}

void abandon()
{
    if (g_job && g_job->thread.joinable())
        g_job->thread.detach();                     // (the process ends; the driver's work with it)
    g_job = nullptr;
}

}  // namespace gpu_shader

bool GpuVdp::init()
{
    if (gpu_shader::start(false) != gpu_shader::State::Ready)
        return false;
    program = gpu_shader::program();
    glGenVertexArrays(1, &vao);
    glGenTextures(1, &vram_tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, vram_tex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    vram_layers = 0;                                // (allocated by upload())
    lines_tex = integer_texture(GL_R32UI, kLineWords, kRows);
    strip_tex = integer_texture(GL_R32UI, kMaxWidth, kRows);
    water_tex = integer_texture(GL_R32UI, 1, 1);
    pic_index_tex = integer_texture(GL_R32UI, 1, 1);
    pics_tex = integer_texture(GL_R32UI, 1, 1);
    b_rgb_tex = integer_texture(GL_R32UI, 1, 1);
    overlay_tex = integer_texture(GL_R32UI, 1, 1);
    edge_tex = integer_texture(GL_R32UI, 1, 1);
    recolor_tex = integer_texture(GL_R32UI, 1, 1);
    strip_a_tex = integer_texture(GL_R32UI, kMaxWidth * 6, kRows);
    hd_table_tex = integer_texture(GL_R32UI, 1, 1);
    rot_tex = integer_texture(GL_R32UI, 1, 1);
    rot_changed = true;
    glGenTextures(1, &hd_atlas_tex);                // layer 0 the pictures, layer 1 their texels' blends
    glBindTexture(GL_TEXTURE_2D_ARRAY, hd_atlas_tex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenTextures(2, water_pre_tex);
    glGenFramebuffers(1, &water_pre_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, water_pre_fbo);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, water_pre_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWaterWidth, kRows, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, water_pre_tex[i], 0);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        std::fprintf(stderr, "gpu-vdp: the water pass's framebuffer is incomplete\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGenTextures(1, &target);
    glGenFramebuffers(1, &fbo);
    if (glGenQueries)
        glGenQueries(1, &query);
    return true;
}

void GpuVdp::shutdown()
{
    if (!program)
        return;
    // (the program is gpu_shader's, shared by every GpuVdp: kept)
    glDeleteVertexArrays(1, &vao);
    GLuint textures[15] = {vram_tex, lines_tex, strip_tex, target, water_tex, pic_index_tex, pics_tex, b_rgb_tex,
                           overlay_tex, edge_tex, recolor_tex, strip_a_tex, hd_table_tex, hd_atlas_tex, rot_tex};
    glDeleteTextures(15, textures);
    hd_uploaded = nullptr;
    glDeleteFramebuffers(1, &fbo);
    if (water_layer_tex[0]) {
        glDeleteTextures(2, water_layer_tex);
        glDeleteFramebuffers(1, &water_fbo);
    }
    glDeleteTextures(2, water_pre_tex);
    glDeleteFramebuffers(1, &water_pre_fbo);
    if (layer_tex[0]) {
        glDeleteTextures(kLayerCount, layer_tex);
        glDeleteFramebuffers(2, layer_fbo);
    }
    if (query)
        glDeleteQueries(1, &query);
    program = 0;
}

void GpuVdp::begin_frame(int frame_rows)
{
    rows = std::clamp(frame_rows, 1, kRows);
    versions = 0;
    strip_used = false;
    strip_a_used = false;
    layer_maybe = 0x03;                             // (planes A and B low: always counted)
    priority_a.base = priority_b.base = priority_window.base = -1;
}

void GpuVdp::capture_line(const Vdp& vdp, int line, int row_at, int width, int view_left)
{
    if (line < 0 || line >= kLines || row_at < 0 || row_at >= rows)
        return;
    strip_width = std::min(width + 2 * kStripMargin, kMaxWidth);
    strip_left = view_left + kStripMargin;
    if (line == 0) {                                // (as render_line's line 0, each time)
        take_water(vdp);
        if (hd && pack)
            take_pack(vdp);
        take_rotated(vdp);
        recolor_count = 0;
        if (vdp.recolor_tile >= 0) {
            // render_line builds the mask on line 0 when the display is on (a GPU frame doesn't
            // run it, so this does, the same way); otherwise the last one stays.
            const uint8_t* r = vdp.reg;
            const bool h40 = r[12] & 0x01;
            if (r[1] & 0x40)
                const_cast<Vdp&>(vdp).build_recolor_mask(h40 ? (r[5] & 0x7E) << 9 : (r[5] & 0x7F) << 9, h40 ? 80 : 64);
            if (vdp.recolor_tiles > 0 && vdp.recolor_mask.size() == (size_t)vdp.recolor_tiles * 64) {
                recolor_first = vdp.recolor_tile, recolor_count = vdp.recolor_tiles;
                if (recolor_table.size() != vdp.recolor_mask.size()
                    || !std::equal(recolor_table.begin(), recolor_table.end(), vdp.recolor_mask.begin())) {
                    recolor_table.assign(vdp.recolor_mask.begin(), vdp.recolor_mask.end());
                    recolor_changed = true;
                }
            }
        }
    }
    // (Lines with the display off show only the backdrop, so they need no copy.)
    if ((vdp.reg[1] & 0x40) && (versions == 0 || vdp.vram_writes != vram_writes)) {
        versions++;
        copies++;
        most_copies = std::max(most_copies, versions);
        if (vram.size() < (size_t)versions * 0x10000)
            vram.resize((size_t)versions * 0x10000);
        std::memcpy(&vram[(size_t)(versions - 1) * 0x10000], vdp.vram, 0x10000);
        vram_writes = vdp.vram_writes;
    }
    uint8_t regs[24];
    std::memcpy(regs, vdp.reg, sizeof(regs));
    if (vdp.ignore_window)                          // (render_line: no window on this row)
        regs[17] = regs[18] = 0;
    const uint8_t* reg = regs;
    uint32_t* w = lines[row_at];
    w[219] = (uint32_t)line;
    w[0] = reg[1] | reg[2] << 8 | reg[3] << 16 | (uint32_t)reg[4] << 24;
    w[1] = reg[7] | reg[11] << 8 | reg[12] << 16 | (uint32_t)reg[16] << 24;

    const bool h40 = reg[12] & 0x01;
    const int native_width = h40 ? 320 : 256;
    const int window_vp = (reg[18] & 0x1F) * 8;
    const bool window_line = (reg[18] & 0x80) ? line >= window_vp : line < window_vp;
    int window_period = 0;                          // as Vdp::render_line works it out
    if (window_line) {
        const int base_window = h40 ? (reg[3] & 0x3C) << 10 : (reg[3] & 0x3E) << 10;
        const int window_w = h40 ? 64 : 32, native_cells = native_width / 8;
        const int row_at = base_window + (line >> 3) * window_w * 2;
        auto entry = [&](int c) { return vdp.vram[(row_at + c * 2) & 0xFFFF] << 8 | vdp.vram[(row_at + c * 2 + 1) & 0xFFFF]; };
        for (int p = 1; p <= 8 && !window_period; p++) {
            bool repeats = true;
            for (int c = 2; c + p < native_cells - 2 && repeats; c++)
                repeats = entry(c) == entry(c + p);
            if (repeats)
                window_period = p * 8;
        }
    }
    w[2] = reg[17] | reg[18] << 8 | (uint32_t)window_period << 16;
    int hs_a, hs_b;
    vdp.line_scroll(line, hs_a, hs_b);
    w[3] = (uint32_t)hs_a | (uint32_t)hs_b << 16;
    {
        int hs_up, hs_down, unused;
        vdp.line_scroll(line > 0 ? line - 1 : line, hs_up, unused);
        vdp.line_scroll(line < kLines - 1 ? line + 1 : line, hs_down, unused);
        w[288] = ((uint32_t)hs_up & 0xFFFF) | (uint32_t)hs_down << 16;
    }
    for (int i = 0; i < 20; i++)
        w[4 + i] = vdp.vsram[i * 2] | (uint32_t)vdp.vsram[i * 2 + 1] << 16;
    for (int i = 0; i < 32; i++)
        w[24 + i] = vdp.cram[i * 2] | (uint32_t)vdp.cram[i * 2 + 1] << 16;
    for (int i = 0; i < 64; i++)
        w[220 + i] = vdp.color(i);

    // The planes' shifts, as render_line asks for them: for drawing, or (layers) for the layers.
    int sub_a = 0, sub_b = 0, layer_a = 0, layer_b = 0;
    backdrops[row_at] = vdp.color(reg[7]);
    w[284] = w[285] = w[286] = 0;
    w[287] = layered ? (uint32_t)vdp.sub_sprites_b : 0;
    if (reg[1] & 0x40) {
        if (!layered && (scale > 1 || vdp.scroll_at_scale_1) && vdp.smooth_scroll)
            vdp.smooth_scroll(line, hs_a, hs_b, scale, sub_a, sub_b);
        if (layered && vdp.smooth_scroll)
            vdp.smooth_scroll(line, hs_a, hs_b, kLayerSub, layer_a, layer_b);
        if (layered) {
            w[284] = (uint32_t)layer_a;
            w[285] = (uint32_t)(vdp.sprite_layer_shift ? vdp.sprite_layer_shift(line, kLayerSub) : layer_a);
            w[286] = (uint32_t)layer_b;
        }
    }
    w[217] = (uint32_t)sub_a;
    w[218] = (uint32_t)sub_b;

    // The line's sprites, as render_line picks them: in link order, at most 20 (16 in the
    // 256-pixel mode) counted, stopping at a masking sprite; each moved by sprite_offset.
    int count = 0;
    // Overlays (Smooth Water's spouts) replace the sprites they hide (render_line's `hiding`).
    const bool hiding = !vdp.overlays.empty() && !(reg[11] & 0x04) && !window_line;
    if (reg[1] & 0x40) {
        const int base_sprites = h40 ? (reg[5] & 0x7E) << 9 : (reg[5] & 0x7F) << 9;
        const int max_sprites = h40 ? 80 : 64;
        const int max_per_line = vdp.sprite_limit ? (h40 ? 20 : 16) : max_sprites;
        int link = 0, on_line = 0;
        bool nonzero_x_seen = false;
        for (int n = 0; n < max_sprites; n++) {
            const uint8_t* s = &vdp.vram[(base_sprites + link * 8) & 0xFFFF];
            // Smooth Animation: a turned picture in the place of its object's first sprite.
            for (size_t k = 0; k < vdp.rotated.size() && k < (size_t)kRotMax && count < 80; k++) {
                const Vdp::Rotated& r = vdp.rotated[k];
                if (r.first != link)
                    continue;
                int dx = 0, dy = 0;
                if (vdp.sprite_offset)
                    vdp.sprite_offset(link, line, (uint16_t)(s[4] << 8 | s[5]), scale, dx, dy);
                const float cy = r.y + (float)(dy + vdp.sprite_y_shift);
                if (line + 1 < cy - r.radius || line > cy + r.radius)
                    continue;
                const float cx = (r.x + (float)view_left) * (float)scale + (float)((layered ? 0 : sub_a) + dx);
                w[57 + count * 2] = 1u << 31 | (uint32_t)k | (uint32_t)(std::lround(cy * 16) + 32768) << 8;
                w[58 + count * 2] = (uint32_t)(int32_t)std::lround(cx * 16);
                count++;
            }
            const int y = ((s[0] << 8 | s[1]) & 0x3FF) - 128 + vdp.sprite_y_shift;
            const int cells_w = ((s[2] >> 2) & 3) + 1, cells_h = (s[2] & 3) + 1;
            const uint16_t attr = s[4] << 8 | s[5];
            const int raw_x = (s[6] << 8 | s[7]) & 0x1FF;
            int line_y = y;                         // where the line test sees the sprite
            if (vdp.sprite_offset) {
                int dx = 0, dy = 0;
                vdp.sprite_offset(link, line, attr, scale, dx, dy);
                line_y += dy;
            }
            if (line >= line_y && line < line_y + cells_h * 8 && on_line++ < max_per_line) {
                const bool nonzero_x_seen_before = nonzero_x_seen;
                if (raw_x == 0) {
                    if (nonzero_x_seen && vdp.sprite_masking)
                        break;
                } else {
                    nonzero_x_seen = true;
                }
                const bool on_b = vdp.sprite_moves_with_b && vdp.sprite_moves_with_b(attr);
                int shift = layered ? 0 : on_b ? (scale > 1 ? vdp.sub_sprites_b : 0) : sub_a, dy = 0;
                if (vdp.sprite_offset) {
                    int dx = 0;
                    vdp.sprite_offset(link, line, attr, scale, dx, dy);
                    shift += dx;
                }
                int row = line - (y + dy);
                for (size_t k = 0; k < vdp.rotated.size(); k++)
                    if (vdp.rotated[k].replaces[link & 0x7F])
                        row = -1;                   // drawn turned instead (Smooth Animation)
                if (hiding && (size_t)link < vdp.hidden_sprites.size() && vdp.hidden_sprites[link])
                    row = -1;                       // drawn as an overlay instead
                if (!nonzero_x_seen_before && vdp.masking_lead(base_sprites, link, line))
                    row = -1;                       // the masking's first sprite (Vdp::masking_lead)
                if (row >= 0 && row < cells_h * 8) {
                    if (attr & 0x1000)
                        row = cells_h * 8 - 1 - row;
                    w[57 + count * 2] = attr | (uint32_t)row << 16 | (uint32_t)(cells_w - 1) << 24
                                        | (uint32_t)(cells_h - 1) << 26 | (layered && on_b ? 1u << 28 : 0);
                    w[58 + count * 2] = (uint32_t)((raw_x - 128 + view_left) * scale + shift);
                    count++;
                }
            }
            link = s[3] & 0x7F;
            if (link == 0)
                break;
        }
    }
    w[56] = count | (versions > 0 ? versions - 1 : 0) << 8;
    int rows = 0;
    if (hiding && (reg[1] & 0x40))
        for (size_t k = 0; k < vdp.overlays.size(); k++) {
            const Vdp::Overlay& o = vdp.overlays[k];
            int dx = 0, dy = 0;
            if (vdp.sprite_offset)
                vdp.sprite_offset(o.sprite, line, 0, scale, dx, dy);
            const int row = line - (o.y + vdp.sprite_y_shift + dy);
            if (row < 0 || row >= o.h)
                continue;
            if (rows == kOverlayRows) {
                overlay_overflow++;
                break;
            }
            w[290 + 3 * rows] = (uint32_t)((o.x + view_left) * scale + (layered ? 0 : sub_a) + dx);
            w[291 + 3 * rows] = overlay_start[k] + (uint32_t)(row * o.w);
            w[292 + 3 * rows] = (uint32_t)o.w | (o.priority ? 1u << 16 : 0u);
            rows++;
        }
    w[289] = (uint32_t)rows;
    overlay_lines += rows > 0;
    if (layered && (reg[1] & 0x40))
        note_layers(vdp, row_at, width, window_line);

    // The strip rules' remapped pixels (render_line's per-pixel path, at scale 1).
    uint32_t* row = strip[row_at];
    std::memset(row, 0, sizeof(strip[0]));
    uint32_t* row_a = strip_a[row_at];
    const bool a_words = water_on || pictures_on || edges_on || pack != nullptr;   // (packs: the strips' tiles)
    if (a_words)
        std::memset(row_a, 0, sizeof(strip_a[0]));
    if (!strips || !(reg[1] & 0x40) || (!vdp.extend_plane_a && !vdp.extend_plane_b))
        return;
    const int hs_up = (int)(w[288] & 0xFFFF), hs_down = (int)(w[288] >> 16);
    static const int kSizes[4] = {32, 64, 32, 128};
    const int plane_w = kSizes[reg[16] & 3], plane_h = kSizes[(reg[16] >> 4) & 3];
    const int base_a = (reg[2] & 0x38) << 10, base_b = (reg[4] & 0x07) << 13;
    const bool column_vscroll = reg[11] & 0x04;
    for (int i = 0; i < strip_width; i++) {
        const int x = i - strip_left;
        const bool in_native = x >= 0 && x < native_width;
        int column = x >> 4;
        column = column < 0 ? 0 : column > 19 ? 19 : column;
        const int vs_a = column_vscroll ? vdp.vsram[column * 2] : vdp.vsram[0];
        const int vs_b = column_vscroll ? vdp.vsram[column * 2 + 1] : vdp.vsram[1];
        bool pri;
        if ((vdp.extend_plane_b_inside || !in_native) && vdp.extend_plane_b) {
            int bx = x - hs_b, by = line + vs_b;
            if (vdp.extend_plane_b(x, line, bx, by)) {
                const int b = vdp.plane_pixel(base_b, bx, by, plane_w, plane_h, pri);
                if (vdp.strip_b_priority_right && x >= native_width)
                    pri = true;
                row[i] |= 0x40000000u | (uint32_t)b << 16 | (pri ? 0x800000u : 0);
                strip_used = true;
                if (a_words) {                      // (art packs: its tile and plane coordinates)
                    const int at = (base_b + ((((by % (plane_h * 8)) + plane_h * 8) % (plane_h * 8)) >> 3) * plane_w * 2
                                    + ((((bx % (plane_w * 8)) + plane_w * 8) % (plane_w * 8)) >> 3) * 2) & 0xFFFF;
                    row_a[i * 6 + 4] = 0x80000000u | (uint32_t)(vdp.vram[at] << 8 | vdp.vram[at | 1]);
                    row_a[i * 6 + 5] = ((uint32_t)bx & 0xFFFF) | (uint32_t)by << 16;
                    strip_a_used = true;
                }
            }
        }
        // (Whether the window covers it is the shader's to say: with shifted planes it goes by the
        // unshifted position, not plane A's.)
        if ((vdp.extend_plane_a_inside || !in_native) && vdp.extend_plane_a) {
            int px = x - hs_a, py = line + vs_a;
            if (vdp.extend_plane_a(x, line, px, py)) {
                const int a = vdp.strip_pixel(base_a, px, py, plane_w, plane_h, pri);
                const bool front = (vdp.strip_fill_in_front || (vdp.strip_fill_in_front_left && x < 0)) && a;
                row[i] |= 0x80000000u | (uint32_t)a | (pri ? 0x80u : 0) | (front ? 0x100u : 0);
                strip_used = true;
                if (a_words)
                    row_a[i * 6 + 3] = ((uint32_t)px & 0xFFFF) | (uint32_t)py << 16;
            }
            // For Smooth Water's neighbours and its own kind: the tile entry and pixel on the
            // lines above, at and below, each with its own scroll (render_pixels' fetch_a).
            if (a_words)
                for (int d = -1; d <= 1; d++) {
                    int qx = x - (d < 0 ? hs_up : d > 0 ? hs_down : hs_a), qy = line + d + vs_a;
                    if (!vdp.extend_plane_a(x, line + d, qx, qy))
                        continue;
                    uint16_t entry = 0;
                    bool p;
                    const int a = vdp.strip_pixel(base_a, qx, qy, plane_w, plane_h, p, &entry);
                    row_a[i * 6 + d + 1] = 0x80000000u | (uint32_t)(a & 127) << 16 | entry;
                    strip_a_used = true;
                }
        }
    }
}

void GpuVdp::take_water(const Vdp& vdp)
{
    edges_on = vdp.clean_black_edges;
    if (edges_on) {
        static std::vector<uint32_t> table;
        table.assign(2048 * 2, 0u);
        for (uint16_t t : vdp.edge_tiles)
            table[t & 0x7FF] = 1;
        for (int i = 0; i < 64; i++)
            table[2048 + i] = vdp.edge_palette[i];
        if (table != edge_table) {
            edge_table.swap(table);
            edges_changed = true;
        }
    }
    water_on = vdp.water_tiles.size() == 2048;
    if (water_on && (water_mask.size() != 2048 || !std::equal(water_mask.begin(), water_mask.end(), vdp.water_tiles.begin()))) {
        water_mask.assign(vdp.water_tiles.begin(), vdp.water_tiles.end());
        water_mask_changed = true;
    }
    pictures_on = vdp.tile_rgba_index.size() == 2048;
    if (pictures_on) {
        static std::vector<uint32_t> index, pictures;   // (compared with what went up last)
        index.resize(2048);
        for (int t = 0; t < 2048; t++)
            index[t] = vdp.tile_rgba_index[t] >= 0 ? (uint32_t)vdp.tile_rgba_index[t] + 1 : 0;
        pictures.clear();
        for (const auto& p : vdp.tile_rgba)
            pictures.insert(pictures.end(), p.begin(), p.end());
        if (pictures.empty())
            pictures.resize(64);
        if (index != pic_index || pictures != pics) {
            pic_index.swap(index);
            pics.swap(pictures);
            pics_changed = true;
        }
    }
    b_rgb_on = vdp.plane_b_rgb && vdp.plane_b_rgb_w > 0 && vdp.plane_b_rgb_h > 0;
    if (b_rgb_on) {
        const size_t n = (size_t)vdp.plane_b_rgb_w * vdp.plane_b_rgb_h;
        if (b_rgb_w != vdp.plane_b_rgb_w || b_rgb_h != vdp.plane_b_rgb_h || b_rgb.size() != n
            || std::memcmp(b_rgb.data(), vdp.plane_b_rgb, n * 4)) {
            b_rgb.assign(vdp.plane_b_rgb, vdp.plane_b_rgb + n);
            b_rgb_w = vdp.plane_b_rgb_w, b_rgb_h = vdp.plane_b_rgb_h;
            b_rgb_changed = true;
        }
    }
    static std::vector<uint32_t> px;
    px.clear();
    overlay_start.clear();
    for (const Vdp::Overlay& o : vdp.overlays) {
        overlay_start.push_back((uint32_t)px.size());
        px.insert(px.end(), o.argb.begin(), o.argb.end());
    }
    px.resize((px.size() + 1023) / 1024 * 1024);
    if (px != overlay_px) {
        overlay_px.swap(px);
        overlays_changed = true;
    }
}

bool GpuVdp::PriorityRows::any(const Vdp& vdp, int table, int cells, int r)
{
    if (base != table || width != cells || vram_writes != vdp.vram_writes) {
        base = table, width = cells, vram_writes = vdp.vram_writes;
        std::memset(row, -1, sizeof(row));
    }
    if (row[r] < 0) {
        row[r] = 0;
        for (int c = 0; c < cells && !row[r]; c++)
            row[r] = (vdp.vram[(table + (r * cells + c) * 2) & 0xFFFF] & 0x80) != 0;
    }
    return row[r] != 0;
}

// Which layers line `line` may put pixels in (the shader's layered output, kept on the safe
// side: a sprite or a priority tile counts even where its pixels are clear or hidden).
void GpuVdp::note_layers(const Vdp& vdp, int row_at, int width, bool window_line)
{
    const uint32_t* w = lines[row_at];
    const int line = (int)w[219];
    uint8_t reg[24];
    std::memcpy(reg, vdp.reg, sizeof(reg));
    reg[17] = w[2] & 0xFF, reg[18] = (w[2] >> 8) & 0xFF;   // (none when the row ignores the window)
    unsigned own = 0x03;
    for (int i = 0; i < (int)(w[56] & 255); i++) {
        const uint32_t e = w[57 + 2 * i];
        const int first = (int)w[58 + 2 * i], cells_w = (int)((e >> 24) & 3) + 1;
        if (first >= width || first + cells_w * 8 <= 0)   // (outside the view: the shader's ox is 0 to width)
            continue;
        own |= (e & (1u << 28)) ? 1u << 5 : (e & 0x8000) ? 1u << 6 : 1u << 2;
    }
    for (int k = 0; k < (int)w[289]; k++)           // overlays: drawn in a sprite layer
        own |= (w[292 + 3 * k] >> 16) ? 1u << 6 : 1u << 2;
    static const int kSizes[4] = {32, 64, 32, 128};
    const int plane_w = kSizes[reg[16] & 3], plane_h = kSizes[(reg[16] >> 4) & 3];
    const int base_a = (reg[2] & 0x38) << 10, base_b = (reg[4] & 0x07) << 13;
    const bool column_vscroll = reg[11] & 0x04;
    for (int column = 0; column < (column_vscroll ? 20 : 1); column++) {
        const int row_a = ((line + vdp.vsram[column * 2]) & (plane_h * 8 - 1)) >> 3;
        const int row_b = ((line + vdp.vsram[column * 2 + 1]) & (plane_h * 8 - 1)) >> 3;
        if (!window_line && priority_a.any(vdp, base_a, plane_w, row_a))
            own |= 1u << 4;
        if (priority_b.any(vdp, base_b, plane_w, row_b))
            own |= 1u << 3;
    }
    const bool h40 = reg[12] & 0x01;
    const int window_hp = (reg[17] & 0x1F) * 16;
    const bool window_right = reg[17] & 0x80;
    if (window_line || (window_right ? window_hp < (h40 ? 320 : 256) : window_hp > 0)) {
        const int base_window = h40 ? (reg[3] & 0x3C) << 10 : (reg[3] & 0x3E) << 10;
        if (priority_window.any(vdp, base_window, h40 ? 64 : 32, (line >> 3) & 31))
            own |= 1u << 4;
    }
    // The hidden-pixel fill: a layer under one with pixels takes them where both move alike.
    const int shift_a = window_line ? 0 : (int)w[284], shift_s = (int)w[285], shift_b = (int)w[286],
              shift_sb = (int)w[287];
    const int moves[7] = {shift_b, shift_a, shift_s, shift_b, shift_a, shift_sb, shift_s};
    unsigned maybe = own;
    for (int j = 1; j < 7; j++)
        if (own & 1u << j)
            for (int i = 0; i < j; i++)
                if (moves[i] == moves[j])
                    maybe |= 1u << i;
    layer_maybe |= maybe;
}

void GpuVdp::collect_query()
{
    if (!query_pending)
        return;
    GLuint64 ns = 0;
    glGetQueryObjectui64v(query, GL_QUERY_RESULT, &ns);
    gpu_ms += ns / 1e6;
    gpu_renders++;
    query_pending = false;
}

bool GpuVdp::line_shifts(int row_at, int& sub_a, int& sub_b) const
{
    if (row_at < 0 || row_at >= rows || (scale <= 1 && !layered))
        return false;
    sub_a = (int)lines[row_at][layered ? 284 : 217];
    sub_b = (int)lines[row_at][layered ? 286 : 218];
    return true;
}

void GpuVdp::layer_line(int row_at, int shifts[4], bool& window_line, uint32_t& backdrop) const
{
    const uint32_t* w = lines[row_at];
    const int line = (int)w[219];
    const int reg18 = (w[2] >> 8) & 0xFF;
    const int window_vp = (reg18 & 0x1F) * 8;
    window_line = (w[0] & 0x40) && ((reg18 & 0x80) ? line >= window_vp : line < window_vp);
    shifts[0] = (int)w[284];
    shifts[1] = (int)w[285];
    shifts[2] = (int)w[286];
    shifts[3] = (int)w[287];
    backdrop = backdrops[row_at];
}

void GpuVdp::take_rotated(const Vdp& vdp)
{
    std::vector<uint32_t> words(kRotWidth * kRotRows, 0);
    int column = 0;
    for (size_t k = 0; k < vdp.rotated.size() && k < (size_t)kRotMax; k++) {
        const Vdp::Rotated& r = vdp.rotated[k];
        const Vdp::RotatedPicture& pic = vdp.rotated_pictures[(size_t)r.picture];
        if (pic.w <= 0 || pic.h <= 0 || column + pic.w > kRotPictures || pic.h > kRotRows - 1)
            continue;
        const float c = std::cos(r.angle), s = std::sin(r.angle);
        auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        uint32_t* head = &words[k * 16];
        head[0] = bits(c), head[1] = bits(s), head[2] = bits(pic.pivot_x), head[3] = bits(pic.pivot_y);
        head[4] = (uint32_t)column, head[5] = (uint32_t)pic.w, head[6] = (uint32_t)pic.h;
        for (int y = 0; y < pic.h; y++)
            std::memcpy(&words[(size_t)(1 + y) * kRotWidth + column], &pic.words[(size_t)y * pic.w], (size_t)pic.w * 4);
        column += pic.w;
        if (r.clip >= 0 && (size_t)r.clip < vdp.rotated_pictures.size()) {
            const Vdp::RotatedPicture& m = vdp.rotated_pictures[(size_t)r.clip];
            if (m.w > 0 && m.h > 0 && column + m.w <= kRotPictures && m.h <= kRotRows - 1) {
                head[7] = (uint32_t)column + 1, head[8] = (uint32_t)m.w, head[9] = (uint32_t)m.h;
                head[10] = bits(r.clip_x), head[11] = bits(r.clip_y);
                for (int y = 0; y < m.h; y++)
                    std::memcpy(&words[(size_t)(1 + y) * kRotWidth + column], &m.words[(size_t)y * m.w], (size_t)m.w * 4);
                column += m.w;
            }
        }
    }
    auto bits2 = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
    const int spinners = (int)std::min<size_t>(vdp.spinners.size(), 2);
    words[128] = (uint32_t)spinners;
    for (int k = 0; k < spinners; k++) {
        const Vdp::Spinner& sp = vdp.spinners[(size_t)k];
        uint32_t* w = &words[129 + k * 304];
        std::copy(sp.moving, sp.moving + 32, w + 16);
        for (int c = 0; c < 16; c++) {               // words 10-11 unshade, 12-13 shade
            w[10 + c / 8] |= (uint32_t)(sp.unshade[c] & 15) << ((c & 7) * 4);
            w[12 + c / 8] |= (uint32_t)(sp.shade[c] & 15) << ((c & 7) * 4);
        }
        w[0] = (uint32_t)sp.first;
        w[14] = (uint32_t)sp.plane;
        w[15] = sp.solid | (sp.single ? 1u << 16 : 0u);
        for (int i = 0; i < 32 * 32; i++)
            w[48 + (i / 32) * 8 + (i % 32) / 4] |= (uint32_t)((sp.behind[i] & 15) | (sp.single ? (sp.base[i] & 15) << 4 : 0))
                                                   << ((i % 4) * 8);
        for (int i = 0; i < 16; i++) {
            w[1 + i / 8] |= (uint32_t)(sp.cell[i] & 15) << ((i & 7) * 4);
            w[3 + sp.cell[i] / 8] |= (uint32_t)i << ((sp.cell[i] & 7) * 4);
        }
        w[5] = bits2(sp.cx), w[6] = bits2(sp.cy), w[7] = bits2(sp.radius);
        w[8] = bits2(std::cos(sp.angle)), w[9] = bits2(std::sin(sp.angle));
    }
    // Drums: column 1003 how many, drum k from 1004 + k * 6; their look from column 864 of rows
    // 1-32 (the rotated pictures stop short of it).
    const int drums = (int)std::min<size_t>(vdp.drums.size(), 3);
    words[1003] = (uint32_t)drums;
    for (int k = 0; k < drums; k++) {
        const Vdp::Drum& d = vdp.drums[(size_t)k];
        uint32_t* w = &words[1004 + k * 6];
        w[0] = (uint32_t)(d.x & 0xFFFF) | (uint32_t)(d.y & 0xFFFF) << 16;
        w[1] = (uint32_t)d.w | (uint32_t)d.h << 16;
        w[2] = bits2(d.angle);
        w[3] = (uint32_t)d.first | (uint32_t)d.tiles << 16;
    }
    if (drums > 0 && vdp.drum_look_w <= 160 && vdp.drum_look_h <= 32
        && vdp.drum_look.size() == (size_t)vdp.drum_look_w * vdp.drum_look_h)
        for (int y = 0; y < vdp.drum_look_h; y++)
            for (int x = 0; x < vdp.drum_look_w; x++)
                words[(size_t)(1 + y) * kRotWidth + kRotPictures + x] = vdp.drum_look[(size_t)y * vdp.drum_look_w + x];
    const int cyclers = (int)std::min<size_t>(vdp.cyclers.size(), 4);
    words[940] = (uint32_t)cyclers;
    for (int k = 0; k < cyclers; k++) {
        const Vdp::Cycler& c = vdp.cyclers[(size_t)k];
        uint32_t* w = &words[941 + k * 14];
        w[0] = (uint32_t)(c.x & 0xFFFF) | (uint32_t)(c.y & 0xFFFF) << 16;
        w[1] = bits2(c.step), w[2] = bits2(c.spoke_start);
        w[12] = (uint32_t)(c.rim_x & 0xFFFF) | (uint32_t)(c.rim_y & 0xFFFF) << 16;
        w[3] = bits2(c.inner), w[4] = bits2(c.outer);
        w[5] = (uint32_t)c.colours | (uint32_t)c.palette << 16;
        w[6] = (uint32_t)c.order[0] | (uint32_t)c.order[1] << 4 | (uint32_t)c.order[2] << 8;
        w[7] = bits2(c.rim_inner), w[8] = bits2(c.rim_outer), w[9] = bits2(c.rim_segments);
        w[10] = bits2(c.rim_start), w[11] = bits2(c.angle / c.step);
    }
    const int gliders = (int)std::min<size_t>(vdp.gliders.size(), 12);
    words[840] = (uint32_t)gliders;
    for (int k = 0; k < gliders; k++) {
        const Vdp::Glider& g = vdp.gliders[(size_t)k];
        uint32_t* w = &words[841 + k * 8];
        w[0] = (uint32_t)(g.x & 0xFFFF) | (uint32_t)(g.y & 0xFFFF) << 16;
        w[1] = bits2(g.offset);
        for (int row = 0; row < 3; row++)
            w[2 + row] = (uint32_t)g.cell[row * 2] | (uint32_t)g.cell[row * 2 + 1] << 16;
    }
    if (!vdp.shutters.empty()) {
        const Vdp::Shutter& s = vdp.shutters[0];   // (all shutters show the same tiles)
        uint32_t* w = &words[(size_t)33 * kRotWidth + kRotPictures];
        w[0] = 1, w[1] = (uint32_t)s.first;
        for (int i = 0; i < 16; i++)
            w[2 + i / 8] |= (uint32_t)(s.cell[i] & 15) << ((i & 7) * 4);
        w[4] = bits2(std::cos(s.angle)), w[5] = bits2(std::sin(s.angle));
        w[6] = (uint32_t)s.hinge_l | (uint32_t)s.split << 8 | (uint32_t)s.hinge_r << 16 | (uint32_t)s.sill << 24;
        w[7] = bits2(s.distance), w[8] = bits2(s.middle), w[9] = bits2(s.shadow_k);
        w[10] = (uint32_t)(s.wall & 15) | (uint32_t)(s.shadow & 15) << 4 | (uint32_t)(s.back & 15) << 8 | (uint32_t)(s.edge & 15) << 12;
        for (int i = 0; i < 32 * 32; i++) {
            const int at = (i / 32) * 4 + (i % 32) / 8, shift = (i % 8) * 4;
            words[(size_t)34 * kRotWidth + kRotPictures + at] |= (uint32_t)(s.door[i] & 15) << shift;
            words[(size_t)35 * kRotWidth + kRotPictures + at] |= (uint32_t)(s.behind[i] & 15) << shift;
        }
    }
    const int chains = (int)std::min<size_t>(vdp.chains.size(), 8);
    words[(size_t)33 * kRotWidth + 910] = (uint32_t)chains;
    for (int k = 0; k < chains; k++) {
        const Vdp::Chain& c = vdp.chains[(size_t)k];
        auto put = [&](int i, uint32_t v) {         // (4 rows of 160 words, run on)
            words[(size_t)(52 + k * 4 + i / 160) * kRotWidth + kRotPictures + i % 160] = v;
        };
        const int n = std::min(c.length, 316);
        put(0, (uint32_t)(c.x & 0xFFFF) | (uint32_t)(c.y & 0xFFFF) << 16);
        put(1, (uint32_t)n);
        put(2, (uint32_t)c.anchor);
        for (int r = 0; r < n && r < (int)c.offset.size(); r++)
            put(8 + 2 * r, bits2(c.offset[(size_t)r])), put(9 + 2 * r, bits2(c.angle[(size_t)r]));
    }
    if (!vdp.buds.empty()) {
        const Vdp::Bud& b = vdp.buds[0];           // (all buds show the same tiles)
        const int n = 16 * Vdp::kBudScale;
        words[(size_t)33 * kRotWidth + 900] = 1, words[(size_t)33 * kRotWidth + 901] = (uint32_t)b.first;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                words[(size_t)(36 + y / 8) * kRotWidth + kRotPictures + (y % 8) * 16 + x / 8]
                    |= (uint32_t)(b.look[y * n + x] & 15) << ((x % 8) * 4);
    }
    if (words != rot_words) {
        rot_words.swap(words);
        rot_changed = true;
    }
}

void GpuVdp::upload()
{
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, vram_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (versions > vram_layers) {                   // grown: room for at least twice as many
        vram_layers = std::min(std::max(versions, 2 * vram_layers), kRows);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8UI, 256, 256, vram_layers, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    }
    if (versions > 0)
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 256, 256, versions, GL_RED_INTEGER, GL_UNSIGNED_BYTE, vram.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    // Only the words in use: the most sprites any line has.
    int used = kLineWords;
    for (const auto& l : lines)
        used = std::max(used, 57 + 2 * (int)(l[56] & 255));
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, lines_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, kLineWords);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, used, rows, GL_RED_INTEGER, GL_UNSIGNED_INT, lines);
    if (strip_used) {
        strip_frames++;
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_2D, strip_tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, kMaxWidth);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, strip_width, rows, GL_RED_INTEGER, GL_UNSIGNED_INT, strip);
    }
    if (strip_a_used) {
        glActiveTexture(GL_TEXTURE0 + 13);
        glBindTexture(GL_TEXTURE_2D, strip_a_tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, kMaxWidth * 6);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, strip_width * 6, rows, GL_RED_INTEGER, GL_UNSIGNED_INT, strip_a);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    auto table = [](GLuint tex, const std::vector<uint32_t>& data, int width, int height) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, width, height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, data.data());
    };
    glActiveTexture(GL_TEXTURE0 + 4);
    if (water_on && water_mask_changed) {
        table(water_tex, water_mask, 2048, 1);
        water_mask_changed = false;
    }
    if (pictures_on && pics_changed) {
        table(pic_index_tex, pic_index, 2048, 1);
        table(pics_tex, pics, 64, (int)pics.size() / 64);
        pics_changed = false;
    }
    if (b_rgb_on && b_rgb_changed) {
        table(b_rgb_tex, b_rgb, b_rgb_w, b_rgb_h);
        b_rgb_changed = false;
    }
    if (recolor_count && recolor_changed) {
        table(recolor_tex, recolor_table, 64, recolor_count);
        recolor_changed = false;
    }
    if (edges_on && edges_changed) {
        table(edge_tex, edge_table, 2048, 2);
        edges_changed = false;
    }
    if (!overlay_px.empty() && overlays_changed) {
        table(overlay_tex, overlay_px, 1024, (int)overlay_px.size() / 1024);
        overlays_changed = false;
    }
    if (rot_changed) {
        glActiveTexture(GL_TEXTURE0 + 16);
        table(rot_tex, rot_words, kRotWidth, kRotRows);
        rot_changed = false;
        glActiveTexture(GL_TEXTURE0 + 4);
    }
    if (pack && hd_uploaded != pack) {              // a new pack: its pictures and colours
        glBindTexture(GL_TEXTURE_2D_ARRAY, hd_atlas_tex);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, pack->atlas_width, pack->atlas_height, 2, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, pack->atlas_width, pack->atlas_height, 1, GL_RGBA,
                        GL_UNSIGNED_BYTE, pack->atlas.data());
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, pack->atlas_width, pack->atlas_height, 1, GL_RGBA,
                        GL_UNSIGNED_BYTE, pack->atlas_blend.data());
        hd_uploaded = pack;
        hd_slots_changed = true;
    }
    if (pack && hd_slots_changed) {
        // Row 0: the tiles' slots; 1-2 plane A's cells, 3-4 plane B's; 5 the sprites' pieces;
        // 6 the window's cells; from 7 each slot's place in the atlas; then the colours (from
        // hd_ref_row).
        const size_t atlas_rows = (pack->slot_atlas.size() + 2047) / 2048;
        std::vector<uint32_t> t((7 + atlas_rows) * 2048 + ((pack->reference.size() + 2047) / 2048) * 2048, 0);
        std::memcpy(t.data(), hd_slots, sizeof(hd_slots));
        std::memcpy(t.data() + 2048, hd_cells[0], sizeof(hd_cells[0]));
        std::memcpy(t.data() + 3 * 2048, hd_cells[1], sizeof(hd_cells[1]));
        std::memcpy(t.data() + 5 * 2048, hd_pieces, sizeof(hd_pieces));
        std::memcpy(t.data() + 6 * 2048, hd_cells[2], 2048 * 4);
        std::memcpy(t.data() + 7 * 2048, pack->slot_atlas.data(), pack->slot_atlas.size() * 4);
        std::memcpy(t.data() + (7 + atlas_rows) * 2048, pack->reference.data(), pack->reference.size() * 4);
        hd_ref_row = 7 + (int)atlas_rows;
        table(hd_table_tex, t, 2048, (int)t.size() / 2048);
        hd_slots_changed = false;
    }
    glActiveTexture(GL_TEXTURE0);
}

void GpuVdp::set_pack(const art::Pack* p)
{
    pack = p;
    hd_uploaded = nullptr;
    hd_vram.clear();
    std::memset(hd_slots, 0, sizeof(hd_slots));
    std::memset(hd_hash, 0, sizeof(hd_hash));
    std::memset(hd_cells, 0, sizeof(hd_cells));
    std::memset(hd_pieces, 0, sizeof(hd_pieces));
    hd_slots_changed = true;
}

void GpuVdp::take_pack(const Vdp& vdp)
{
    if (!pack)
        return;
    // Only the tiles that changed since the last look (the Vdp's write count won't do: the
    // pictures between frames copy VRAM into a Vdp of their own).
    const bool all = hd_vram.size() != sizeof(vdp.vram);
    const uint8_t regs[6] = {vdp.reg[2], vdp.reg[3], vdp.reg[4], vdp.reg[5], vdp.reg[12], vdp.reg[16]};
    const bool regs_same = std::memcmp(regs, hd_regs, sizeof(regs)) == 0 && std::memcmp(vdp.cram, hd_cram, sizeof(hd_cram)) == 0;
    std::memcpy(hd_cram, vdp.cram, sizeof(hd_cram));
    if (!all && regs_same && std::memcmp(hd_vram.data(), vdp.vram, sizeof(vdp.vram)) == 0)
        return;
    if (all)
        hd_vram.resize(sizeof(vdp.vram));
    hd_look++;
    for (int tile = 0; tile < 2048; tile++) {
        if (!all && std::memcmp(&hd_vram[tile * 32], &vdp.vram[tile * 32], 32) == 0)
            continue;
        std::memcpy(&hd_vram[tile * 32], &vdp.vram[tile * 32], 32);
        hd_hash[tile] = art::pattern_hash(&vdp.vram[tile * 32]);
        if (!all)
            hd_rewritten[tile] = hd_look;
        auto found = pack->slot.find(hd_hash[tile]);
        const uint32_t s = found == pack->slot.end() ? 0 : (uint32_t)found->second + 1;
        if (hd_slots[tile] != s) {
            hd_slots[tile] = s;
            hd_slots_changed = true;
        }
    }
    std::memcpy(hd_regs, regs, sizeof(regs));
    const uint64_t empty = art::empty_pattern();

    // Sprites: each sprite in the table whose tiles make a piece the pack has takes that
    // piece's pictures (looked up by its first tile and size).
    uint32_t pieces[2048] = {};
    if (!pack->piece.empty()) {
        const int table = (vdp.reg[5] & ((vdp.reg[12] & 1) ? 0x7E : 0x7F)) << 9;
        int link = 0;
        for (int n = 0; n < 80; n++) {
            const uint8_t* e = &vdp.vram[(table + link * 8) & 0xFFFF];
            const int w = ((e[2] >> 2) & 3) + 1, h = (e[2] & 3) + 1, first = (e[4] << 8 | e[5]) & 0x7FF;
            uint64_t pattern[16];
            for (int k = 0; k < w * h; k++)
                pattern[k] = hd_hash[(first + k) & 0x7FF];
            auto found = pack->piece.find(art::piece_key(w, h, pattern));
            if (found != pack->piece.end())
                pieces[first] = (uint32_t)(found->second + 1) | (uint32_t)(w - 1) << 28 | (uint32_t)(h - 1) << 30;
            link = e[3] & 0x7F;
            if (link == 0)
                break;
        }
        // Smooth Animation's upright pictures: their pieces, though the game shows other frames.
        for (const Vdp::RotatedPicture& pic : vdp.rotated_pictures)
            for (uint32_t word : pic.words) {
                if (!word || ((word >> 16) & 31) || ((word >> 21) & 31))
                    continue;                       // (each piece once: its first pixel)
                const int w = ((word >> 26) & 3) + 1, h = ((word >> 28) & 3) + 1, first = word & 0x7FF;
                uint64_t pattern[16];
                for (int k = 0; k < w * h; k++)
                    pattern[k] = hd_hash[(first + k) & 0x7FF];
                auto found = pack->piece.find(art::piece_key(w, h, pattern));
                if (found != pack->piece.end())
                    pieces[first] = (uint32_t)(found->second + 1) | (uint32_t)(w - 1) << 28 | (uint32_t)(h - 1) << 30;
            }
    }
    if (std::memcmp(pieces, hd_pieces, sizeof(pieces)) != 0) {
        std::memcpy(hd_pieces, pieces, sizeof(pieces));
        hd_slots_changed = true;
    }

    // Planes A and B are laid out in blocks of 2 x 2 cells from even cells: each such square
    // whose four tiles make a block the pack has takes that block's pictures. The art dump's
    // screens give every square of their planes and window as a block, so the window too.
    auto size = [](int s) { return s == 1 ? 64 : s == 3 ? 128 : 32; };
    const bool h40 = vdp.reg[12] & 1;
    // The palette lines as the art dump writes colours (0xAARRGGBB, 3 bits a channel).
    uint32_t lines[4][16];
    for (int i = 0; i < 64; i++) {
        const uint16_t c = vdp.cram[i];
        auto level = [](int v) { return (uint32_t)(v * 255 / 7); };
        lines[i / 16][i % 16] = 0xFF000000 | level((c >> 1) & 7) << 16 | level((c >> 5) & 7) << 8 | level((c >> 9) & 7);
    }
    for (int plane = 0; plane < 3; plane++) {
        const int w = plane == 2 ? (h40 ? 64 : 32) : size(vdp.reg[16] & 3);
        const int h = plane == 2 ? 32 : size((vdp.reg[16] >> 4) & 3);
        const int base = plane == 0 ? (vdp.reg[2] & 0x38) << 10
                         : plane == 1 ? (vdp.reg[4] & 7) << 13
                                      : (vdp.reg[3] & (h40 ? 0x3C : 0x3E)) << 10;
        if (w * h > 4096 || pack->block.empty())
            continue;
        // Every square's key and colours (its tiles' palette lines as the art dump writes
        // them), then each looked up: among the squares round it in these colours, in these
        // colours, among the squares round it, alone.
        const int sw = w / 2, sh = h / 2;
        std::vector<uint64_t> keys((size_t)sw * sh), colours((size_t)sw * sh);
        std::vector<uint8_t> animated((size_t)sw * sh);     // a tile rewritten in the last 64 looks
        for (int sy = 0; sy < sh; sy++)
            for (int sx = 0; sx < sw; sx++) {
                uint64_t pattern[4];
                int flips[4];
                const uint32_t* line_of[4];
                for (int q = 0; q < 4; q++) {
                    const int cell = (sy * 2 + (q >> 1)) * w + sx * 2 + (q & 1);
                    const int at = (base + cell * 2) & 0xFFFF, e = vdp.vram[at] << 8 | vdp.vram[at | 1];
                    const uint32_t when = hd_rewritten[e & 0x7FF];
                    animated[(size_t)sy * sw + sx] |= when != 0 && hd_look - when < 64;
                    pattern[q] = hd_hash[e & 0x7FF];
                    flips[q] = pattern[q] == empty ? 0 : (e >> 11) & 3;
                    line_of[q] = pattern[q] == empty ? nullptr : lines[(e >> 13) & 3];
                }
                keys[(size_t)sy * sw + sx] = art::block_key(pattern, flips);
                colours[(size_t)sy * sw + sx] = art::colours_key(line_of);
            }
        for (int cy = 0; cy < h; cy += 2)
            for (int cx = 0; cx < w; cx += 2) {
                const int sx = cx / 2, sy = cy / 2;
                const int cell[4] = {cy * w + cx, cy * w + cx + 1, (cy + 1) * w + cx, (cy + 1) * w + cx + 1};
                const uint64_t key = keys[(size_t)sy * sw + sx];
                const uint64_t col = colours[(size_t)sy * sw + sx];
                const uint64_t around[4] = {keys[(size_t)sy * sw + (sx + sw - 1) % sw], keys[(size_t)sy * sw + (sx + 1) % sw],
                                            keys[(size_t)((sy + sh - 1) % sh) * sw + sx], keys[(size_t)((sy + 1) % sh) * sw + sx]};
                // (Beside an animated square the neighbours change every few frames, and the
                // picture would flip between neighbour-matched versions: stage 11's waterfalls.)
                const bool near_animated = animated[(size_t)sy * sw + sx] || animated[(size_t)sy * sw + (sx + sw - 1) % sw]
                                           || animated[(size_t)sy * sw + (sx + 1) % sw]
                                           || animated[(size_t)((sy + sh - 1) % sh) * sw + sx]
                                           || animated[(size_t)((sy + 1) % sh) * sw + sx];
                const uint64_t ctx = pack->block_context.empty() || near_animated ? 0 : art::context_key(key, around);
                int first = -1;                     // the block's first slot
                auto look = [&](const std::unordered_map<uint64_t, int>& map, uint64_t k) {
                    if (first >= 0)
                        return;
                    auto it = map.find(k);
                    if (it != map.end())
                        first = it->second;
                };
                if (ctx)
                    look(pack->block_context, art::with_colours(ctx, col));
                look(pack->block, art::with_colours(key, col));
                if (ctx)
                    look(pack->block_context, ctx);
                look(pack->block, key);
                for (int q = 0; q < 4; q++) {
                    const uint32_t s = first < 0 ? 0 : (uint32_t)(first + q) + 1;
                    if (hd_cells[plane][cell[q]] != s) {
                        hd_cells[plane][cell[q]] = s;
                        hd_slots_changed = true;
                    }
                }
            }
    }
}

void GpuVdp::draw(int start, int count, int view_left, int pass, bool black_border)
{
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "vram"), 1);
    glUniform1i(glGetUniformLocation(program, "lines"), 2);
    glUniform1i(glGetUniformLocation(program, "strips"), 3);
    glUniform1i(glGetUniformLocation(program, "start"), start);
    glUniform1i(glGetUniformLocation(program, "count"), count);
    glUniform1i(glGetUniformLocation(program, "scale"), layered ? 1 : scale);
    glUniform1i(glGetUniformLocation(program, "view_left"), view_left);
    glUniform1i(glGetUniformLocation(program, "strip_on"), strip_used ? 1 : 0);
    glUniform1i(glGetUniformLocation(program, "strip_left"), strip_left);
    glUniform1i(glGetUniformLocation(program, "layered"), pass);
    glUniform1i(glGetUniformLocation(program, "black_border"), black_border ? 1 : 0);
    const GLuint tables[5] = {water_tex, pic_index_tex, pics_tex, b_rgb_tex, overlay_tex};
    static const char* const kNames[5] = {"water_mask", "pic_index", "pics", "b_rgb", "overlay_px"};
    for (int i = 0; i < 5; i++) {
        glActiveTexture(GL_TEXTURE0 + 4 + i);
        glBindTexture(GL_TEXTURE_2D, tables[i]);
        glUniform1i(glGetUniformLocation(program, kNames[i]), 4 + i);
    }
    for (int i = 0; i < 2; i++) {
        glActiveTexture(GL_TEXTURE0 + 9 + i);
        glBindTexture(GL_TEXTURE_2D, pass == 4 ? 0 : water_pre_tex[i]);
        glUniform1i(glGetUniformLocation(program, i ? "water_pre1" : "water_pre0"), 9 + i);
    }
    glUniform1i(glGetUniformLocation(program, "water_pre_on"), water_on || pictures_on ? 1 : 0);
    glActiveTexture(GL_TEXTURE0 + 11);
    glBindTexture(GL_TEXTURE_2D, edge_tex);
    glUniform1i(glGetUniformLocation(program, "edge_table"), 11);
    glUniform1i(glGetUniformLocation(program, "edges_on"), edges_on ? 1 : 0);
    glActiveTexture(GL_TEXTURE0 + 12);
    glBindTexture(GL_TEXTURE_2D, recolor_tex);
    glUniform1i(glGetUniformLocation(program, "recolor"), 12);
    glUniform1i(glGetUniformLocation(program, "recolor_tile"), recolor_first);
    glUniform1i(glGetUniformLocation(program, "recolor_tiles"), recolor_count);
    glActiveTexture(GL_TEXTURE0 + 13);
    glBindTexture(GL_TEXTURE_2D, strip_a_tex);
    glUniform1i(glGetUniformLocation(program, "strip_a"), 13);
    glUniform1i(glGetUniformLocation(program, "strip_a_on"), strip_a_used ? 1 : 0);
    const bool hd_pass = pack && hd && pass == 0;
    glActiveTexture(GL_TEXTURE0 + 14);
    glBindTexture(GL_TEXTURE_2D, hd_table_tex);
    glUniform1i(glGetUniformLocation(program, "hd_table"), 14);
    glActiveTexture(GL_TEXTURE0 + 15);
    glBindTexture(GL_TEXTURE_2D_ARRAY, hd_atlas_tex);
    glUniform1i(glGetUniformLocation(program, "hd_atlas"), 15);
    glUniform1i(glGetUniformLocation(program, "hd_on"), hd_pass ? 1 : 0);
    glUniform1i(glGetUniformLocation(program, "hd_scale"), pack ? pack->scale : 1);
    glUniform1i(glGetUniformLocation(program, "hd_ref_row"), hd_ref_row);
    static const bool art_debug = SDL_getenv("COI_ART_DEBUG") != nullptr;
    glUniform1i(glGetUniformLocation(program, "hd_debug"), art_debug ? 1 : 0);
    glUniform1i(glGetUniformLocation(program, "vscale"), hd && pass == 0 ? kHdScale : 1);
    glActiveTexture(GL_TEXTURE0 + 16);
    glBindTexture(GL_TEXTURE_2D, rot_tex);
    glUniform1i(glGetUniformLocation(program, "rot_tex"), 16);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(program, "water_on"), water_on ? 1 : 0);
    glUniform1i(glGetUniformLocation(program, "pictures_on"), pictures_on ? 1 : 0);
    glUniform1i(glGetUniformLocation(program, "b_rgb_on"), b_rgb_on ? 1 : 0);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void GpuVdp::water_prepass(int view_left)
{
    if (!water_on && !pictures_on)
        return;
    static const GLenum kTargets[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1};
    glBindFramebuffer(GL_FRAMEBUFFER, water_pre_fbo);
    glDrawBuffers(2, kTargets);
    glViewport(0, 0, kWaterWidth, rows);
    draw(0, kWaterWidth, view_left, 4, false);
}

void GpuVdp::render(int width, int start, int count, int view_left)
{
    collect_query();
    last_start = start, last_count = count, last_view_left = view_left;
    upload();
    water_prepass(view_left);
    const int out_rows = rows * (hd ? kHdScale : 1);
    if (width != target_width || out_rows != target_rows) {
        glBindTexture(GL_TEXTURE_2D, target);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, out_rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::fprintf(stderr, "gpu-vdp: the frame's framebuffer is incomplete\n");
        target_width = width;
        target_rows = out_rows;
    }
    water_drawn = false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, out_rows);
    if (query && timed)
        glBeginQuery(GL_TIME_ELAPSED, query);
    draw(start, count, view_left, 0, false);
    if (query && timed) {
        glEndQuery(GL_TIME_ELAPSED);
        query_pending = true;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GpuVdp::render_layers(int width, int start, int count, int view_left, bool black_border)
{
    collect_query();
    last_start = start, last_count = count, last_view_left = view_left;
    storm_drawn = false;
    upload();
    water_prepass(view_left);
    if (width != layer_width || rows != layer_rows) {
        if (!layer_tex[0]) {
            glGenTextures(kLayerCount, layer_tex);
            glGenFramebuffers(2, layer_fbo);
        }
        for (int i = 0; i < kLayerCount; i++) {
            glBindTexture(GL_TEXTURE_2D, layer_tex[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
        for (int pass = 0; pass < 2; pass++) {
            glBindFramebuffer(GL_FRAMEBUFFER, layer_fbo[pass]);
            const int first = pass * 4, n = pass ? kLayerCount - 4 : 4;
            for (int i = 0; i < n; i++)
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, layer_tex[first + i], 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                std::fprintf(stderr, "gpu-vdp: the layers' framebuffer is incomplete\n");
        }
        if (!water_layer_tex[0]) {
            glGenTextures(2, water_layer_tex);
            glGenFramebuffers(1, &water_fbo);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, water_fbo);
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, water_layer_tex[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, water_layer_tex[i], 0);
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::fprintf(stderr, "gpu-vdp: the water layers' framebuffer is incomplete\n");
        layer_width = width;
        layer_rows = rows;
    }
    static const GLenum kTargets[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1, GL_COLOR_ATTACHMENT0 + 2,
                                       GL_COLOR_ATTACHMENT0 + 3};
    static const GLenum kTargets3[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1, GL_COLOR_ATTACHMENT0 + 2,
                                        GL_NONE};
    glViewport(0, 0, width, rows);
    if (query && timed)
        glBeginQuery(GL_TIME_ELAPSED, query);
    for (int pass = 0; pass < 2; pass++) {
        glBindFramebuffer(GL_FRAMEBUFFER, layer_fbo[pass]);
        glDrawBuffers(4, pass ? kTargets3 : kTargets);
        draw(start, count, view_left, pass + 1, black_border);
    }
    water_drawn = water_on || pictures_on;          // (Smooth Water: its two layers)
    if (water_drawn) {
        glBindFramebuffer(GL_FRAMEBUFFER, water_fbo);
        glDrawBuffers(2, kTargets);
        draw(start, count, view_left, 3, black_border);
    }
    if (query && timed) {
        glEndQuery(GL_TIME_ELAPSED);
        query_pending = true;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GpuVdp::read_layer(int i, std::vector<uint32_t>& argb)
{
    argb.resize((size_t)layer_width * layer_rows);
    glBindFramebuffer(GL_FRAMEBUFFER, layer_fbo[i / 4]);
    glReadBuffer(GL_COLOR_ATTACHMENT0 + i % 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
#ifdef __ANDROID__
    glReadPixels(0, 0, layer_width, layer_rows, GL_RGBA, GL_UNSIGNED_BYTE, argb.data());
    for (uint32_t& c : argb)
        c = (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
#else
    glReadPixels(0, 0, layer_width, layer_rows, GL_BGRA, GL_UNSIGNED_BYTE, argb.data());
#endif
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    collect_query();
}

void GpuVdp::read_water(int i, std::vector<uint32_t>& argb)
{
    argb.resize((size_t)layer_width * layer_rows);
    glBindFramebuffer(GL_FRAMEBUFFER, water_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
#ifdef __ANDROID__
    glReadPixels(0, 0, layer_width, layer_rows, GL_RGBA, GL_UNSIGNED_BYTE, argb.data());
    for (uint32_t& c : argb)
        c = (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
#else
    glReadPixels(0, 0, layer_width, layer_rows, GL_BGRA, GL_UNSIGNED_BYTE, argb.data());
#endif
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GpuVdp::read(std::vector<uint32_t>& argb)
{
    argb.resize((size_t)target_width * target_rows);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
#ifdef __ANDROID__
    glReadPixels(0, 0, target_width, target_rows, GL_RGBA, GL_UNSIGNED_BYTE, argb.data());
    for (uint32_t& c : argb)                        // R, G, B, A in memory: to 0xAARRGGBB
        c = (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
#else
    glReadPixels(0, 0, target_width, target_rows, GL_BGRA, GL_UNSIGNED_BYTE, argb.data());
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    collect_query();
}

// --- Weather - Stage 4 (storm.h): rain and splashes, its own small program -----------------------

namespace {

// Drawn over the finished picture (or into the front layer), blended: each pixel is worked out
// in level coordinates (the camera, the view's offset and Smooth Scrolling's shift), so the drops
// keep their place as the view scrolls. Drops are a game pixel wide; they fall only above the
// level's roofs (`heights`, a texel per 16-pixel column: its first solid block's Y, low byte in
// red, high in green), and the game's splash (`splash`, kSplashFrames frames of 48 x 16, the
// surface at the bottom) plays where they land.
const char* kRainFragment = R"(
uniform sampler2D heights;
uniform sampler2D splash;
uniform int start, count, scale, vscale, view_left, sub_a, hud_top, columns, level_height, splash_on;
uniform int opaque;           // 1: a layer, whose pixels are whole (the drops mixed with the night's blue)
uniform int camera_x, camera_y;
uniform float time, strength, flash, splash_scale;
out vec4 frag;

float hash(vec2 p)
{
    p = fract(p * vec2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x);
}

// The roof's Y (with 32768 added when it is a slope, which takes no splashes).
float roof_raw(float x)
{
    int c = clamp(int(floor(x / 16.0)), 0, columns - 1);
    vec4 h = texelFetch(heights, ivec2(c, 0), 0);
    return floor(h.r * 255.0 + 0.5) + floor(h.g * 255.0 + 0.5) * 256.0;
}

float roof(float x)
{
    float v = roof_raw(x);
    return v >= 32768.0 ? v - 32768.0 : v;
}

// A ledge above the roof (the top of a branch Mickey can stand on), 1-3, or 65535 for none.
float ledge(float x, int n)
{
    int c = clamp(int(floor(x / 16.0)), 0, columns - 1);
    vec4 h = texelFetch(heights, ivec2(c, n / 2), 0);
    vec2 v = n == 2 ? h.rg : h.ba;
    return floor(v.x * 255.0 + 0.5) + floor(v.y * 255.0 + 0.5) * 256.0;
}

// The surface splashes play on in a slot: the ground (0) or a ledge (1-3), the same under the
// splash's whole width; -1: none.
float surface(float cx, float half_w, int kind)
{
    if (kind == 0) {                                // (not on a slope, nor half over one)
        float ground = roof_raw(cx);
        if (ground >= float(level_height) || abs(roof_raw(cx - half_w) - ground) > 0.5 || abs(roof_raw(cx + half_w) - ground) > 0.5)
            return -1.0;
        return ground;
    }
    float top = ledge(cx, kind);
    if (top >= 65535.0)
        return -1.0;
    for (int n = 1; n <= 3; n++)                    // (the ledge may be another one in the next column)
        if (abs(ledge(cx - half_w, n) - top) < 0.5) {
            for (int m = 1; m <= 3; m++)
                if (abs(ledge(cx + half_w, m) - top) < 0.5)
                    return top;
            return -1.0;
        }
    return -1.0;
}

// One layer of drops at world pixel p: its brightness (0: none).
float drops(vec2 p, vec2 cell, float speed, float slant, float len, float density, float seed)
{
    float qx = p.x + floor(p.y * slant);
    float qy = p.y - floor(time * speed);
    vec2 c = floor(vec2(qx, qy) / cell);
    vec2 local = vec2(qx, qy) - c * cell;
    if (hash(c + seed) > density * strength)
        return 0.0;
    float dx = floor(hash(c + seed + 11.7) * cell.x);
    float dy = floor(hash(c + seed + 23.1) * (cell.y - len));
    if (local.x != dx || local.y < dy || local.y >= dy + len)
        return 0.0;
    return 0.35 + 0.65 * (local.y - dy + 1.0) / len;      // the tail faint, the head bright
}

// The splash covering world pixel p, if any (premultiplied colour and alpha).
vec4 splashes(vec2 p)
{
    const float slot = 20.0;
    float half_w = 24.0 * splash_scale;
    vec4 got = vec4(0.0);
    float base = floor(p.x / slot);
    for (int k = 0; k < 12; k++) {
        int kind = k / 3;
        float s = base + float(k - kind * 3 - 1);
        float seed = float(kind) * 7.0;
        float cx = s * slot + 4.0 + floor(hash(vec2(s, 5.0 + seed)) * (slot - 8.0));
        float ground = surface(cx, half_w, kind);
        if (ground < 0.0)
            continue;
        float period = 23.0 + floor(10.0 + hash(vec2(s, 9.0 + seed)) * 40.0);
        float phase = hash(vec2(s, 2.0 + seed)) * period;
        float t = mod(time + phase, period);
        if (t >= 23.0 || hash(vec2(s + seed * 13.0, floor((time + phase) / period))) > strength)
            continue;
        int frame = t < 5.0 ? 0 : t < 11.0 ? 1 : t < 17.0 ? 2 : 3;
        float sx = floor((p.x - cx) / splash_scale) + 24.0;
        float sy = floor((p.y - ground) / splash_scale) + 16.0;
        if (sx < 0.0 || sx >= 48.0 || sy < 0.0 || sy >= 16.0)
            continue;
        vec4 c = texelFetch(splash, ivec2(frame * 48 + int(sx), int(sy)), 0);
        if (c.a > 0.0)
            got = vec4(c.rgb, 1.0);
    }
    return got;
}

void main()
{
    float ox = gl_FragCoord.x - float(start);
    if (ox < 0.0 || ox >= float(count))
        discard;
    float gy = gl_FragCoord.y / float(vscale);
    if (gy >= float(hud_top))
        discard;
    float gx = (ox - float(sub_a)) / float(scale) - float(view_left);
    vec2 w = vec2(float(camera_x), float(camera_y)) + vec2(gx, gy);
    vec2 p = floor(w);
    float ground = roof(p.x);
    vec3 colour = vec3(0.0);
    float alpha = 0.0;
    if (p.y < ground) {
        float far = drops(p, vec2(8.0, 30.0), 6.0, 0.22, 6.0, 0.85, 0.0);
        float near = drops(p, vec2(17.0, 56.0), 10.0, 0.3, 11.0, 0.6, 41.0);
        vec3 tint = mix(vec3(0.62, 0.70, 0.88), vec3(0.95, 0.97, 1.0), flash);
        if (near > 0.0) {
            colour = mix(tint, vec3(1.0), 0.35);
            alpha = 0.7 * near;
        } else if (far > 0.0) {
            colour = tint;
            alpha = 0.45 * far;
        }
    }
    if (splash_on != 0) {
        vec4 s = splashes(w);
        if (s.a > 0.0) {
            colour = mix(s.rgb, vec3(1.0), 0.4 * flash);
            alpha = 0.85 * strength;
        }
    }
    if (alpha <= 0.0)
        discard;
    if (opaque != 0) {
        colour = mix(vec3(0.13, 0.15, 0.34), colour, alpha);
        alpha = 1.0;
    }
    frag = vec4(colour, alpha);
}
)";

GLuint g_rain_program = 0;
bool g_rain_failed = false;

GLuint rain_program()
{
    if (g_rain_program || g_rain_failed)
        return g_rain_program;
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kRainFragment);
    if (vs && fs) {
        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok) {
            g_rain_program = program;
        } else {
            char log[2048];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            std::fprintf(stderr, "gpu-vdp: the rain's shader link error:\n%s\n", log);
            glDeleteProgram(program);
        }
    }
    if (vs)
        glDeleteShader(vs);
    if (fs)
        glDeleteShader(fs);
    g_rain_failed = !g_rain_program;
    return g_rain_program;
}

GLuint rgba_texture(int width, int height, const void* data)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    return t;
}

}  // namespace

void GpuVdp::draw_storm(const storm::Picture& p)
{
    if (!p.heights || p.heights->empty() || !target_width && !layered)
        return;
    const GLuint program = rain_program();
    if (!program)
        return;
    if (storm_heights_version != p.heights_version || !storm_heights_tex) {
        std::vector<uint32_t> texels(p.heights->size());
        for (size_t i = 0; i < texels.size(); i++)
            texels[i] = (*p.heights)[i];            // (R, G: the roof's low and high bytes; B, A: the ledge's)
        if (storm_heights_tex)
            glDeleteTextures(1, &storm_heights_tex);
        storm_heights_tex = rgba_texture((int)texels.size() / 2, 2, texels.data());
        storm_heights_version = p.heights_version;
        storm_columns = (int)texels.size() / 2;
    }
    if (p.splash && (storm_splash_version != p.splash_version || !storm_splash_tex)) {
        if (storm_splash_tex)
            glDeleteTextures(1, &storm_splash_tex);
        storm_splash_tex = rgba_texture(storm::kSplashWidth, storm::kSplashHeight, p.splash->data());
        storm_splash_version = p.splash_version;
    }
    int sub_a = 0, sub_b = 0;
    int width, out_rows, vscale, scale_used;
    if (layered) {
        if (!layer_tex[6])
            return;
        if (!storm_fbo)
            glGenFramebuffers(1, &storm_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, storm_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layer_tex[6], 0);
        const GLenum first = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &first);
        width = layer_width, out_rows = layer_rows, vscale = 1, scale_used = 1;
        // A layer's colours are whole (not premultiplied): the rain goes where the layer is
        // clear, and stays behind what it holds (the high-priority sprites).
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ONE, GL_ONE);
        storm_drawn = true;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        width = target_width, out_rows = target_rows, vscale = hd ? kHdScale : 1, scale_used = scale;
        if (!line_shifts(std::min(rows - 1, 100), sub_a, sub_b))
            sub_a = 0;
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    }
    glViewport(0, 0, width, out_rows);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, storm_heights_tex);
    glUniform1i(glGetUniformLocation(program, "heights"), 1);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, storm_splash_tex);
    glUniform1i(glGetUniformLocation(program, "splash"), 2);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(program, "start"), last_start);
    glUniform1i(glGetUniformLocation(program, "count"), last_count);
    glUniform1i(glGetUniformLocation(program, "scale"), scale_used);
    glUniform1i(glGetUniformLocation(program, "vscale"), vscale);
    glUniform1i(glGetUniformLocation(program, "view_left"), last_view_left);
    glUniform1i(glGetUniformLocation(program, "sub_a"), sub_a);
    glUniform1i(glGetUniformLocation(program, "hud_top"), p.hud_top);
    glUniform1i(glGetUniformLocation(program, "columns"), storm_columns);
    glUniform1i(glGetUniformLocation(program, "level_height"), p.level_height);
    glUniform1i(glGetUniformLocation(program, "splash_on"), storm_splash_tex && p.splash ? 1 : 0);
    glUniform1i(glGetUniformLocation(program, "opaque"), layered ? 1 : 0);
    glUniform1i(glGetUniformLocation(program, "camera_x"), p.camera_x);
    glUniform1i(glGetUniformLocation(program, "camera_y"), p.camera_y);
    glUniform1f(glGetUniformLocation(program, "time"), (float)p.time);
    glUniform1f(glGetUniformLocation(program, "strength"), p.strength);
    glUniform1f(glGetUniformLocation(program, "flash"), p.flash);
    glUniform1f(glGetUniformLocation(program, "splash_scale"), p.splash_scale);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
