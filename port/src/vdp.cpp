#include "vdp.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bus.h"

extern "C" {
#include "m68k.h"
}

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

uint16_t Vdp::read_port(uint32_t port)
{
    if (port < 0x04) {                              // data port
        pending = false;
        uint16_t value = 0;
        switch (code & 0x0F) {
        case 0x00: value = vram[address & 0xFFFE] << 8 | vram[(address & 0xFFFE) | 1]; break;
        case 0x04: value = vsram[(address >> 1) % 40]; break;
        case 0x08: value = cram[(address >> 1) & 63]; break;
        default: break;
        }
        address += reg[15];
        return value;
    }
    if (port < 0x08) {                              // control port: status
        pending = false;
        return 0x3400 | 0x0200 | (vblank ? 0x0008 : 0);   // FIFO empty, never DMA busy
    }
    if (port < 0x10)                                // HV counter
        return (vcounter & 0xFF) << 8;
    return 0xFFFF;
}

void Vdp::write_port(uint32_t port, uint16_t value)
{
    if (port < 0x04) {                              // data port
        pending = false;
        write_data(value);
        if (fill_pending) {
            fill_pending = false;
            int length = reg[19] | reg[20] << 8;
            if (length == 0)
                length = 0x10000;
            for (int i = 0; i < length; i++) {
                vram[(address ^ 1) & 0xFFFF] = value >> 8;
                address += reg[15];
            }
            reg[19] = reg[20] = 0;
        }
        return;
    }
    if (port >= 0x08)                               // HV counter (read-only), PSG: ignored for now
        return;
    if (trace)
        std::printf("[vdp] PC=%06X control write $%04X%s\n", m68k_get_reg(nullptr, M68K_REG_PPC), value,
                    pending ? " (second word)" : "");

    if (pending) {                                  // second word of a command
        pending = false;
        code = (code & 0x03) | ((value >> 2) & 0x3C);
        address = (address & 0x3FFF) | ((value & 0x03) << 14);
        if (trace)
            std::printf("[vdp] PC=%06X command code=$%02X address=$%04X\n",
                        m68k_get_reg(nullptr, M68K_REG_PPC), code, address);
        if ((code & 0x20) && (reg[1] & 0x10))
            do_dma();
        return;
    }
    if ((value & 0xC000) == 0x8000) {               // register write
        int r = (value >> 8) & 0x1F;
        if (r < 24)
            reg[r] = value & 0xFF;
        if (trace)
            std::printf("[vdp] PC=%06X reg%d = $%02X\n", m68k_get_reg(nullptr, M68K_REG_PPC), r, value & 0xFF);
        return;
    }
    code = (code & 0x3C) | (value >> 14);           // first word of a command
    address = (address & 0xC000) | (value & 0x3FFF);
    pending = true;
}

void Vdp::write_data(uint16_t value)
{
    switch (code & 0x0F) {
    case 0x01: {                                    // VRAM; an odd address swaps the bytes
        uint32_t a = address & 0xFFFF;
        vram[a] = value >> 8;
        vram[a ^ 1] = value & 0xFF;
        break;
    }
    case 0x03: cram[(address >> 1) & 63] = value & 0x0EEE; break;
    case 0x05: {
        uint32_t i = (address >> 1) & 63;
        if (i < 40)
            vsram[i] = value & 0x07FF;
        break;
    }
    default: break;
    }
    address += reg[15];
}

void Vdp::do_dma()
{
    int length = reg[19] | reg[20] << 8;
    if (length == 0)
        length = 0x10000;

    switch (reg[23] >> 6) {
    case 2:                                         // fill: happens on the next data port write
        fill_pending = true;
        return;
    case 3: {                                       // VRAM copy
        uint16_t source = reg[21] | reg[22] << 8;
        for (int i = 0; i < length; i++) {
            vram[address & 0xFFFF] = vram[source++];
            address += reg[15];
        }
        reg[21] = source & 0xFF;
        reg[22] = source >> 8;
        break;
    }
    default: {                                      // 68000 memory to VRAM, CRAM or VSRAM
        uint32_t source = (reg[21] | reg[22] << 8 | (reg[23] & 0x7F) << 16) << 1;
        if (on_dma)
            on_dma(source);
        for (int i = 0; i < length; i++) {
            write_data(bus.read16(source));
            source = (source & 0xFE0000) | ((source + 2) & 0x1FFFF);   // wraps within 128 KB
        }
        source >>= 1;
        reg[21] = source & 0xFF;
        reg[22] = (source >> 8) & 0xFF;
        break;
    }
    }
    reg[19] = reg[20] = 0;
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

void Vdp::power_on()
{
    std::memset(vram, 0, sizeof(vram));
    std::memset(cram, 0, sizeof(cram));
    std::memset(vsram, 0, sizeof(vsram));
    std::memset(reg, 0, sizeof(reg));
    address = 0;
    code = 0;
    pending = fill_pending = vblank = false;
    hint_counter = vcounter = 0;
}

void Vdp::start_frame()
{
    vblank = false;
    hint_counter = reg[10];
}

bool Vdp::hint_line()
{
    if (vcounter > kScreenHeight) {                 // reloaded on every line during vblank
        hint_counter = reg[10];
        return false;
    }
    if (hint_counter == 0) {
        hint_counter = reg[10];
        return true;
    }
    hint_counter--;
    return false;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

uint32_t Vdp::color(int index) const
{
    uint16_t c = cram[index & 63];
    if (palette && palette->source[index & 63] == c)
        return palette->rgb[index & 63];
    auto scale = [](int v) { return (uint32_t)(v * 255 / 7); };
    return 0xFF000000 | scale((c >> 1) & 7) << 16 | scale((c >> 5) & 7) << 8 | scale((c >> 9) & 7);
}

// Returns the palette index (0 = transparent) of a plane pixel at plane
// coordinates (x, y), wrapping around the plane.
int Vdp::plane_pixel(int base, int x, int y, int width_cells, int height_cells, bool& priority,
                     uint16_t* entry_out) const
{
    x &= width_cells * 8 - 1;
    y &= height_cells * 8 - 1;
    uint32_t entry_at = (base + ((y >> 3) * width_cells + (x >> 3)) * 2) & 0xFFFF;
    uint16_t entry = vram[entry_at] << 8 | vram[entry_at | 1];
    if (entry_out)
        *entry_out = entry;
    return entry_pixel(entry, x, y, priority);
}

// The pixel at (x, y) within the cell of name table entry `entry`.
int Vdp::entry_pixel(uint16_t entry, int x, int y, bool& priority) const
{
    priority = entry & 0x8000;
    int fx = x & 7, fy = y & 7;
    if (entry & 0x0800) fx = 7 - fx;
    if (entry & 0x1000) fy = 7 - fy;
    uint8_t pair = vram[((entry & 0x7FF) * 32 + fy * 4 + (fx >> 1)) & 0xFFFF];
    int pixel = (fx & 1) ? pair & 15 : pair >> 4;
    return pixel ? ((entry >> 9) & 0x30) | pixel : 0;
}

// A plane A pixel remapped by extend_plane_a: from strip_cells where they cover it.
int Vdp::strip_pixel(int base, int x, int y, int width_cells, int height_cells, bool& priority) const
{
    x &= width_cells * 8 - 1;
    y &= height_cells * 8 - 1;
    const int cx = (x >> 3) - strip_cells_x, cy = (y >> 3) - strip_cells_y;
    if (strip_cells_w > 0 && cx >= 0 && cx < strip_cells_w && cy >= 0
        && (size_t)(cy * strip_cells_w + cx) < strip_cells.size())
        return entry_pixel(strip_cells[cy * strip_cells_w + cx], x, y, priority);
    return plane_pixel(base, x, y, width_cells, height_cells, priority);
}

// For a black plane pixel `a` (CRAM colour 0) at plane coordinates (x, y): a black gap
// between stones that reaches the outside of a rock wall takes the colour of the first pixel
// beyond it (0 when that is transparent, which shows what is behind). Inside a rock edge tile
// that is black connected along the line to the tile's outer side; anywhere, black within 3
// pixels of a transparent pixel. Returns the pixel to draw.
int Vdp::clean_black_edge(int a, int base, int x, int y, int width_cells, int height_cells) const
{
    auto black = [this](int p) { return p && (cram[p] & 0xEEE) == 0 && (edge_palette[p] & 0xEEE) == 0; };
    bool pri;
    uint16_t entry;
    plane_pixel(base, x, y, width_cells, height_cells, pri, &entry);
    if (std::find(edge_tiles.begin(), edge_tiles.end(), entry & 0x7FF) != edge_tiles.end()) {
        const bool flipped = entry & 0x0800;
        const int column = flipped ? 7 - (x & 7) : (x & 7);  // 0 = the outer side
        const int dir = flipped ? 1 : -1;
        int k = 1;
        for (; k <= column; k++)
            if (!black(plane_pixel(base, x + dir * k, y, width_cells, height_cells, pri)))
                break;
        if (k > column)
            return plane_pixel(base, x + dir * k, y, width_cells, height_cells, pri);
    }
    for (int dir = -1; dir <= 1; dir += 2)
        for (int k = 1; k <= 3; k++) {
            int n = plane_pixel(base, x + dir * k, y, width_cells, height_cells, pri);
            if (!n)
                return 0;
            if (!black(n))
                break;
        }
    return a;
}

void Vdp::line_scroll(int line, int& hs_a, int& hs_b) const
{
    const int base_hscroll = (reg[13] & 0x3F) << 10;
    int hs_offset = 0;
    switch (reg[11] & 3) {
    case 2: hs_offset = (line & ~7) * 4; break;
    case 3: hs_offset = line * 4; break;
    case 1: hs_offset = (line & 7) * 4; break;
    default: break;
    }
    hs_a = (vram[(base_hscroll + hs_offset) & 0xFFFF] << 8 | vram[(base_hscroll + hs_offset + 1) & 0xFFFF]) & 0x3FF;
    hs_b = (vram[(base_hscroll + hs_offset + 2) & 0xFFFF] << 8 | vram[(base_hscroll + hs_offset + 3) & 0xFFFF]) & 0x3FF;
}

void Vdp::render_line(int line, uint32_t* out, int width, int left, int scale, LineLayers* layers)
{
    uint32_t backdrop = color(reg[7]);
    if (layers) {
        layers->backdrop = backdrop;
        layers->sub_a = layers->sub_b = 0;
        layers->window_line = false;
        for (uint32_t* l : layers->layer)
            std::memset(l, 0, width * sizeof(uint32_t));
    }
    if (!(reg[1] & 0x40)) {                         // display disabled
        if (!layers)
            for (int x = 0; x < width * scale; x++)
                out[x] = backdrop;
        return;
    }

    const bool h40 = reg[12] & 0x01;
    const int native_width = h40 ? 320 : 256;
    static const int kSizes[4] = {32, 64, 32, 128};
    const int plane_w = kSizes[reg[16] & 3];
    const int plane_h = kSizes[(reg[16] >> 4) & 3];
    const int base_a = (reg[2] & 0x38) << 10;
    const int base_b = (reg[4] & 0x07) << 13;
    const int base_window = h40 ? (reg[3] & 0x3C) << 10 : (reg[3] & 0x3E) << 10;
    const int base_sprites = h40 ? (reg[5] & 0x7E) << 9 : (reg[5] & 0x7F) << 9;

    // Horizontal scroll: full screen, per 8-line cell, or per line.
    int hs_a, hs_b;
    line_scroll(line, hs_a, hs_b);
    const bool column_vscroll = reg[11] & 0x04;

    // Sub-pixel scroll corrections, in 1/scale pixels: plane A (and the sprites, which move
    // with it) and plane B are drawn that far right of their whole-pixel positions.
    int sub_a = 0, sub_b = 0;
    if ((scale > 1 || (scroll_at_scale_1 && !layers)) && smooth_scroll)   // layers move when drawn
        smooth_scroll(line, hs_a, hs_b, scale, sub_a, sub_b);
    if (layers && smooth_scroll)                    // the layers are moved when they are drawn
        smooth_scroll(line, hs_a, hs_b, layers->sub_scale, layers->sub_a, layers->sub_b);
    if (layers)
        layers->sub_sprites = sprite_layer_shift ? sprite_layer_shift(line, layers->sub_scale) : layers->sub_a;
    auto floor_div = [](int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); };

    // Window: whole lines from the vertical setting, or columns from the horizontal one.
    const int window_vp = (reg[18] & 0x1F) * 8;
    const bool window_line = (reg[18] & 0x80) ? line >= window_vp : line < window_vp;
    if (layers)
        layers->window_line = window_line;
    const int window_hp = (reg[17] & 0x1F) * 16;
    const bool window_right = reg[17] & 0x80;
    const int window_w = h40 ? 64 : 32;

    // Outside the original columns, a whole-line window (the HUD) is extended: a row
    // that repeats with a short period (the HUD border) keeps repeating; any other row
    // repeats its edge tile (blank in the HUD), so the HUD reaches the screen edges.
    int window_period = 0;                          // in pixels; 0 = repeat the edge tile
    if (window_line) {
        const int native_cells = native_width / 8;
        const int row_at = base_window + (line >> 3) * window_w * 2;
        auto entry = [&](int c) { return vram[(row_at + c * 2) & 0xFFFF] << 8 | vram[(row_at + c * 2 + 1) & 0xFFFF]; };
        for (int p = 1; p <= 8 && !window_period; p++) {
            bool repeats = true;
            for (int c = 2; c + p < native_cells - 2 && repeats; c++)
                repeats = entry(c) == entry(c + p);
            if (repeats)
                window_period = p * 8;
        }
    }
    auto extended_window_x = [&](int x) {
        if (window_period)
            return ((x % window_period) + window_period) % window_period;
        int edge = x < 0 ? 0 : native_width - 8;
        return edge + (((x % 8) + 8) % 8);
    };

    // Sprites for this line, front to back: the first sprite drawn at a pixel wins. They are
    // laid out in output pixels, so each can move by its own fraction of a pixel: with the
    // planes they move with (plane A's shift, or sub_sprites_b for those moving with the
    // background camera; the layers are moved later instead), plus sprite_offset's.
    const int out_width = width * scale;
    uint8_t sprite_pixel[kMaxWidth * kMaxScale];
    bool sprite_priority[kMaxWidth * kMaxScale];
    bool sprite_on_b[kMaxWidth * kMaxScale];        // moves with the background camera
    std::memset(sprite_pixel, 0, out_width);
    std::memset(sprite_priority, 0, out_width);
    std::memset(sprite_on_b, 0, out_width);
    int link = 0, on_line = 0;
    bool nonzero_x_seen = false;                    // for sprite masking
    const int max_sprites = h40 ? 80 : 64, max_per_line = sprite_limit ? (h40 ? 20 : 16) : max_sprites;
    for (int n = 0; n < max_sprites; n++) {
        const uint8_t* s = &vram[(base_sprites + link * 8) & 0xFFFF];
        int y = ((s[0] << 8 | s[1]) & 0x3FF) - 128;
        int cells_w = ((s[2] >> 2) & 3) + 1;
        int cells_h = (s[2] & 3) + 1;
        uint16_t attr = s[4] << 8 | s[5];
        int raw_x = (s[6] << 8 | s[7]) & 0x1FF;
        int x = raw_x - 128;
        int line_y = y;                             // where the line test sees the sprite
        if (sprite_offset) {
            int dx = 0, dy = 0;
            sprite_offset(link, line, attr, scale, dx, dy);
            line_y += dy;
        }
        if (line >= line_y && line < line_y + cells_h * 8 && on_line++ < max_per_line) {
            // Masking: a sprite at raw X 0 hides every later sprite on this line, once a
            // sprite with nonzero X has appeared on it. The game hides sprites behind the
            // HUD this way (sprite #0 at X 64, then sprite #1 at X 0 on lines 192-223).
            if (raw_x == 0) {
                if (nonzero_x_seen && sprite_masking)
                    break;
            } else {
                nonzero_x_seen = true;
            }
            const bool on_b = sprite_moves_with_b && sprite_moves_with_b(attr);
            int shift = layers ? 0 : on_b ? (scale > 1 ? sub_sprites_b : 0) : sub_a;
            int dy = 0;
            if (sprite_offset) {
                int dx = 0;
                sprite_offset(link, line, attr, scale, dx, dy);
                shift += dx;
            }
            int row = line - (y + dy);
            if (row >= 0 && row < cells_h * 8) {
                if (attr & 0x1000) row = cells_h * 8 - 1 - row;
                for (int cx = 0; cx < cells_w * 8; cx++) {
                    int col = (attr & 0x0800) ? cells_w * 8 - 1 - cx : cx;
                    int tile = (attr & 0x7FF) + (col >> 3) * cells_h + (row >> 3);
                    uint8_t pair = vram[(tile * 32 + (row & 7) * 4 + ((col & 7) >> 1)) & 0xFFFF];
                    int pixel = (col & 1) ? pair & 15 : pair >> 4;
                    if (!pixel)
                        continue;
                    const int start = (x + cx + left) * scale + shift;
                    for (int ox = start < 0 ? 0 : start; ox < start + scale && ox < out_width; ox++) {
                        if (sprite_pixel[ox])
                            continue;
                        sprite_pixel[ox] = ((attr >> 9) & 0x30) | pixel;
                        sprite_priority[ox] = attr & 0x8000;
                        sprite_on_b[ox] = on_b;
                    }
                }
            }
        }
        link = s[3] & 0x7F;
        if (link == 0)
            break;
    }

    // Each output pixel is 1/scale of a game pixel; x is the unshifted position in the
    // original view (the window and HUD), xa and xb the positions in planes A and B.
    for (int ox = 0; ox < width * scale; ox++) {
        const int x = floor_div(ox, scale) - left;
        const int xa = floor_div(ox - sub_a, scale) - left;
        const int xb = floor_div(ox - sub_b, scale) - left;
        int column = xa >> 4;                       // 16-pixel vertical scroll column
        if (column < 0) column = 0;
        if (column > 19) column = 19;
        const int vs_a = column_vscroll ? vsram[column * 2] : vsram[0];
        const int vs_b = column_vscroll ? vsram[column * 2 + 1] : vsram[1];

        bool pri_a, pri_b;
        int b = plane_pixel(base_b, xb - hs_b, line + vs_b, plane_w, plane_h, pri_b);
        int a;
        bool in_native = x >= 0 && x < native_width;
        bool in_window = in_native && (window_line || (window_right ? x >= window_hp : x < window_hp));
        if (in_window)
            a = plane_pixel(base_window, x, line, window_w, 32, pri_a);
        else if (window_line && !in_native)
            a = plane_pixel(base_window, extended_window_x(x), line, window_w, 32, pri_a);
        else {
            int px = xa - hs_a, py = line + vs_a;
            bool remapped = (extend_plane_a_inside || xa < 0 || xa >= native_width) && extend_plane_a
                            && extend_plane_a(xa, line, px, py);
            a = remapped ? strip_pixel(base_a, px, py, plane_w, plane_h, pri_a)
                         : plane_pixel(base_a, px, py, plane_w, plane_h, pri_a);
            if (clean_black_edges && a && (cram[a] & 0xEEE) == 0 && (edge_palette[a] & 0xEEE) == 0)
                a = clean_black_edge(a, base_a, px, py, plane_w, plane_h);
            if (remapped && (strip_fill_in_front || (strip_fill_in_front_left && xa < 0)) && a) {
                if (layers)                         // in front of everything
                    layers->layer[LineLayers::SpritesHigh][ox] = color(a);
                else
                    out[ox] = color(a);
                continue;
            }
        }
        const int sx = ox;                          // sprites are already in place
        int s = sprite_pixel[sx];
        bool pri_s = s && sprite_priority[sx];

        if (layers) {
            if (b) layers->layer[pri_b ? LineLayers::BHigh : LineLayers::BLow][ox] = color(b);
            if (a) layers->layer[pri_a ? LineLayers::AHigh : LineLayers::ALow][ox] = color(a);
            if (s && sprite_on_b[sx]) layers->layer[LineLayers::SpritesB][ox] = color(s);
            else if (s) layers->layer[pri_s ? LineLayers::SpritesHigh : LineLayers::SpritesLow][ox] = color(s);
            continue;
        }

        int pixel;
        if (s && pri_s) pixel = s;
        else if (a && pri_a) pixel = a;
        else if (b && pri_b) pixel = b;
        else if (s) pixel = s;
        else if (a) pixel = a;
        else if (b) pixel = b;
        else pixel = -1;
        out[ox] = pixel < 0 ? backdrop : color(pixel);
    }
}
