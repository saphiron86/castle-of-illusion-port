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
        std::printf("[vdp] PC=%06X control write $%04X%s\n", current_pc(), value,
                    pending ? " (second word)" : "");

    if (pending) {                                  // second word of a command
        pending = false;
        code = (code & 0x03) | ((value >> 2) & 0x3C);
        address = (address & 0x3FFF) | ((value & 0x03) << 14);
        if (trace)
            std::printf("[vdp] PC=%06X command code=$%02X address=$%04X\n",
                        current_pc(), code, address);
        if ((code & 0x20) && (reg[1] & 0x10))
            do_dma();
        return;
    }
    if ((value & 0xC000) == 0x8000) {               // register write
        int r = (value >> 8) & 0x1F;
        if (r < 24)
            reg[r] = value & 0xFF;
        if (trace)
            std::printf("[vdp] PC=%06X reg%d = $%02X\n", current_pc(), r, value & 0xFF);
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
int Vdp::strip_pixel(int base, int x, int y, int width_cells, int height_cells, bool& priority,
                     uint16_t* entry_out) const
{
    x &= width_cells * 8 - 1;
    y &= height_cells * 8 - 1;
    const int cx = (x >> 3) - strip_cells_x, cy = (y >> 3) - strip_cells_y;
    if (strip_cells_w > 0 && cx >= 0 && cx < strip_cells_w && cy >= 0
        && (size_t)(cy * strip_cells_w + cx) < strip_cells.size()) {
        const uint16_t entry = strip_cells[cy * strip_cells_w + cx];
        if (entry_out)
            *entry_out = entry;
        return entry_pixel(entry, x, y, priority);
    }
    return plane_pixel(base, x, y, width_cells, height_cells, priority, entry_out);
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
    // Overlays (drawn by render_pixels) replace the sprites they hide.
    overlay_rows.clear();
    const bool hiding = !overlays.empty() && !column_vscroll && !window_line;
    if (hiding)
        for (const Overlay& o : overlays) {
            int dx = 0, dy = 0;
            if (sprite_offset)
                sprite_offset(o.sprite, line, 0, scale, dx, dy);
            const int row = line - (o.y + dy);
            if (row >= 0 && row < o.h)
                overlay_rows.push_back({&o, row, (o.x + left) * scale + (layers ? 0 : sub_a) + dx});
        }
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
            if (hiding && (size_t)link < hidden_sprites.size() && hidden_sprites[link])
                row = -1;                           // drawn as an overlay instead
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

    if (!column_vscroll) {
        render_pixels(line, out, width, left, scale, layers, backdrop, sprite_pixel, sprite_priority, sprite_on_b,
                      sub_a, sub_b, hs_a, hs_b, window_line, window_hp, window_right, window_w, native_width,
                      window_period, base_a, base_b, base_window, plane_w, plane_h);
        return;
    }

    // Each output pixel is 1/scale of a game pixel; x is the unshifted position in the
    // original view (the window and HUD), xa and xb the positions in planes A and B.
    // (Per-column vertical scroll: plane B's row depends on plane A's column, so this draws
    // pixel by pixel; render_pixels does the rest.)
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
        int bx = xb - hs_b, by = line + vs_b;
        const bool b_remapped = (extend_plane_b_inside || xb < 0 || xb >= native_width) && extend_plane_b
                                && extend_plane_b(xb, line, bx, by);
        int b = plane_pixel(base_b, bx, by, plane_w, plane_h, pri_b);
        if (b_remapped && strip_b_priority_right && xb >= native_width)
            pri_b = true;
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

uint32_t Vdp::overlay_at(int ox, int scale, bool& priority) const
{
    for (const OverlayRow& r : overlay_rows) {
        const int rel = ox - r.start;
        if (rel < 0)
            continue;
        const int x = rel / scale;
        if (x < r.overlay->w)
            if (const uint32_t c = r.overlay->argb[r.row * r.overlay->w + x]) {
                priority = r.overlay->priority;
                return c;
            }
    }
    return 0;
}

// The planes of one line, fetched once per game pixel. With sub-pixel scrolling (scale > 1)
// several output pixels show the same plane pixel, each plane at its own offset, so plane A,
// plane B and the window are looked up once for each game pixel they show (with their colours
// from a 64-entry table) and the output pixels then only pick from those. Draws exactly what
// the per-pixel loop in render_line does.
void Vdp::render_pixels(int line, uint32_t* out, int width, int left, int scale, LineLayers* layers,
                        uint32_t backdrop, const uint8_t* sprite_pixel, const bool* sprite_priority,
                        const bool* sprite_on_b, int sub_a, int sub_b, int hs_a, int hs_b, bool window_line,
                        int window_hp, bool window_right, int window_w, int native_width, int window_period,
                        int base_a, int base_b, int base_window, int plane_w, int plane_h)
{
    auto floor_div = [](int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); };
    if (water_tiles.empty() && overlay_rows.empty() && !plane_b_rgb && tile_rgba_index.empty()) {
        render_pixels_plain(line, out, width, left, scale, layers, backdrop, sprite_pixel, sprite_priority, sprite_on_b,
                            sub_a, sub_b, hs_a, hs_b, window_line, window_hp, window_right, window_w, native_width,
                            window_period, base_a, base_b, base_window, plane_w, plane_h);
        return;
    }
    const int out_width = width * scale;

    // The palette's colours, then (from 64) colours given for single plane B pixels
    // (plane_b_rgb): such a pixel's index points at its own entry.
    uint32_t colors[256];
    for (int i = 0; i < 64; i++)
        colors[i] = color(i);
    int extra_colors = 64;
    const bool b_rgb = plane_b_rgb && plane_b_rgb_w == plane_w * 8 && plane_b_rgb_h == plane_h * 8;

    // Plane pixels: the palette index (0 = transparent) and priority; for plane A also
    // whether it is a strip fill drawn in front of everything.
    struct Pixel { uint8_t index; bool priority; bool front; };
    constexpr int kSpan = kMaxWidth + 8;
    Pixel window_px[kSpan], a_px[kSpan], b_px[kSpan];
    bool window_used[kSpan];

    // The window (the HUD), by unshifted position x.
    const int x_lo = -left, x_hi = floor_div(out_width - 1, scale) - left;
    for (int x = x_lo; x <= x_hi; x++) {
        Pixel& p = window_px[x - x_lo];
        bool pri = false;
        const bool in_native = x >= 0 && x < native_width;
        const bool in_window = in_native && (window_line || (window_right ? x >= window_hp : x < window_hp));
        window_used[x - x_lo] = in_window || (window_line && !in_native);
        if (in_window) {
            p.index = (uint8_t)plane_pixel(base_window, x, line, window_w, 32, pri);
        } else if (window_line && !in_native) {
            int wx = window_period ? ((x % window_period) + window_period) % window_period
                                   : (x < 0 ? 0 : native_width - 8) + (((x % 8) + 8) % 8);
            p.index = (uint8_t)plane_pixel(base_window, wx, line, window_w, 32, pri);
        } else {
            p.index = 0;
        }
        p.priority = pri;
        p.front = false;
    }

    // Plane A, by its position xa, from a_lo - 1 to a_hi + 1 (the pixels either side are
    // Smooth Water's neighbours): a_px[i] is at xa = a_lo - 1 + i. water[i] marks the pixels
    // of water tiles (Smooth Water): kWater for water and transparent ones, kBehind for those
    // that show what lies behind the water, plus kHasBehind in tiles that have such pixels.
    const int vs_a = vsram[0], vs_b = vsram[1];
    const int a_lo = floor_div(-sub_a, scale) - left, a_hi = floor_div(out_width - 1 - sub_a, scale) - left;
    const int a_first = a_lo - 1, a_count = a_hi - a_lo + 3;
    const bool water_on = water_tiles.size() == 2048 && !window_line;
    // kPicture: a pixel of a tile drawn from tile_rgba (its colour and opacity in a_picture).
    enum : uint8_t { kDry, kWater, kBehind, kKind = 3, kHasBehind = 4, kPicture = 8 };
    const bool pictures = tile_rgba_index.size() == 2048 && !window_line;
    static uint32_t a_picture[kSpan];
    static uint8_t water[kSpan];                    // static: off the stack (lines are drawn one at a time)
    int water_lo = a_count, water_hi = -1;          // the span of this line's water pixels
    auto fetch_a = [&](int ln, int hs, Pixel* row, uint8_t* row_water, int from, int to) {
        bool any = false;
        for (int i = from; i < to; i++) {
            const int xa = a_first + i;
            Pixel& p = row[i];
            bool pri = false;
            uint16_t entry = 0;
            int px = xa - hs, py = ln + vs_a;
            const bool remapped = (extend_plane_a_inside || xa < 0 || xa >= native_width) && extend_plane_a
                                  && extend_plane_a(xa, ln, px, py);
            int a = remapped ? strip_pixel(base_a, px, py, plane_w, plane_h, pri, &entry)
                             : plane_pixel(base_a, px, py, plane_w, plane_h, pri, &entry);
            const uint32_t mask = water_on ? water_tiles[entry & 0x7FF] : 0;
            row_water[i] = !mask ? kDry
                         : !(a & 15) || (mask >> (a & 15) & 1) ? kWater
                         : (mask >> (16 + (a & 15)) & 1) ? kBehind : kDry;
            if (row_water[i] && mask >> 16)
                row_water[i] |= kHasBehind;
            if (pictures && tile_rgba_index[entry & 0x7FF] >= 0) {
                const int fx = entry & 0x0800 ? 7 - (px & 7) : px & 7, fy = entry & 0x1000 ? 7 - (py & 7) : py & 7;
                row_water[i] = kPicture;
                if (row == a_px)
                    a_picture[i] = tile_rgba[tile_rgba_index[entry & 0x7FF]][fy * 8 + fx];
            }
            any |= row_water[i] != kDry;
            if (row_water[i] == kDry && clean_black_edges && a && (cram[a] & 0xEEE) == 0 && (edge_palette[a] & 0xEEE) == 0)
                a = clean_black_edge(a, base_a, px, py, plane_w, plane_h);
            p.index = (uint8_t)a;
            p.priority = pri;
            p.front = remapped && (strip_fill_in_front || (strip_fill_in_front_left && xa < 0)) && a;
        }
        return any;
    };
    const bool any_water = fetch_a(line, hs_a, a_px, water, 0, a_count);
    if (any_water)
        for (int i = 0; i < a_count; i++)
            if (water[i]) {
                water_lo = std::min(water_lo, i);
                water_hi = i;
            }

    // Smooth Water: each water pixel's colour (the average of the opaque water pixels around
    // it) and opacity (their share of the pixels of water tiles around it), over 3x3 pixels
    // weighted 1-2-1, and, where the tile paints what lies behind the water (kBehind
    // pixels), the average of those around it as the colour beneath. The lines above and
    // below are fetched with their own scroll.
    static uint32_t water_rgb[kSpan], behind_rgb[kSpan];
    static uint8_t water_alpha[kSpan];
    static bool has_behind[kSpan];
    if (any_water) {
        static Pixel above[kSpan], below[kSpan];
        static uint8_t above_water[kSpan], below_water[kSpan];
        int hs_up, hs_down, hs_b_unused;
        line_scroll(line > 0 ? line - 1 : line, hs_up, hs_b_unused);
        line_scroll(line < kScreenHeight - 1 ? line + 1 : line, hs_down, hs_b_unused);
        // only across the water and a pixel either side (the kernel's reach)
        const int from = std::max(0, water_lo - 1), to = std::min(a_count, water_hi + 2);
        fetch_a(line - 1, hs_up, above, above_water, from, to);
        fetch_a(line + 1, hs_down, below, below_water, from, to);
        const Pixel* rows[3] = {above, a_px, below};
        const uint8_t* rows_water[3] = {above_water, water, below_water};
        auto average = [](int r, int g, int b, int n) {
            return n ? (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(b / n) : 0u;
        };
        for (int i = std::max(1, water_lo); i <= std::min(a_count - 2, water_hi); i++) {
            if (water[i] == kDry)
                continue;
            if (water[i] & kPicture) {              // drawn from tile_rgba
                water_alpha[i] = (uint8_t)(a_picture[i] >> 24);
                water_rgb[i] = a_picture[i] & 0xFFFFFF;
                has_behind[i] = false;
                continue;
            }
            int total = 0, cover = 0, r = 0, g = 0, b = 0, behind = 0, br = 0, bg = 0, bb = 0;
            for (int dy = 0; dy < 3; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const uint8_t kind = rows_water[dy][i + dx];
                    if (kind == kDry || (kind & kPicture))
                        continue;
                    const int w = (dy == 1 ? 2 : 1) * (dx == 0 ? 2 : 1);
                    const int index = rows[dy][i + dx].index;
                    const uint32_t c = colors[index];
                    total += w;
                    if ((kind & kKind) == kBehind) {
                        behind += w;
                        br += w * (int)(c >> 16 & 0xFF), bg += w * (int)(c >> 8 & 0xFF), bb += w * (int)(c & 0xFF);
                    } else if (index) {
                        cover += w;
                        r += w * (int)(c >> 16 & 0xFF), g += w * (int)(c >> 8 & 0xFF), b += w * (int)(c & 0xFF);
                    }
                }
            water_alpha[i] = (uint8_t)(cover * 255 / total);
            water_rgb[i] = average(r, g, b, cover);
            has_behind[i] = behind > 0 && (water[i] & kHasBehind);  // only inside such tiles
            behind_rgb[i] = 0xFF000000u | average(br, bg, bb, behind);
        }
    }
    auto blend = [](uint32_t under, uint32_t over, int alpha) {
        auto channel = [&](int shift) {
            return ((under >> shift & 0xFF) * (255 - alpha) + (over >> shift & 0xFF) * alpha + 127) / 255 << shift;
        };
        return 0xFF000000u | channel(16) | channel(8) | channel(0);
    };

    // Plane B, by its position xb.
    const int b_lo = floor_div(-sub_b, scale) - left, b_hi = floor_div(out_width - 1 - sub_b, scale) - left;
    for (int xb = b_lo; xb <= b_hi; xb++) {
        Pixel& p = b_px[xb - b_lo];
        bool pri = false;
        int bx = xb - hs_b, by = line + vs_b;
        const bool b_remapped = (extend_plane_b_inside || xb < 0 || xb >= native_width) && extend_plane_b
                                && extend_plane_b(xb, line, bx, by);
        p.index = (uint8_t)plane_pixel(base_b, bx, by, plane_w, plane_h, pri);
        if (b_rgb && p.index && extra_colors < 256)
            if (const uint32_t c = plane_b_rgb[(by & (plane_h * 8 - 1)) * plane_b_rgb_w + (bx & (plane_w * 8 - 1))]) {
                colors[extra_colors] = c;
                p.index = (uint8_t)extra_colors++;
            }
        if (b_remapped && strip_b_priority_right && xb >= native_width)
            pri = true;
        p.priority = pri;
        p.front = false;
    }

    // Each output pixel: step the three positions (x, xa, xb) along without dividing.
    int x_i = 0, x_r = 0;
    int a_i = 1, a_r = -sub_a - floor_div(-sub_a, scale) * scale;
    int b_i = 0, b_r = -sub_b - floor_div(-sub_b, scale) * scale;
    if (!any_water && overlay_rows.empty()) {
        // Lines without water or overlays: the plain loop (kept apart, as it is the
        // renderer's busiest; the checks below slow it down noticeably).
        for (int ox = 0; ox < out_width; ox++) {
            const Pixel& pa = window_used[x_i] ? window_px[x_i] : a_px[a_i];
            const Pixel& pb = b_px[b_i];
            if (++x_r == scale) x_r = 0, x_i++;
            if (++a_r == scale) a_r = 0, a_i++;
            if (++b_r == scale) b_r = 0, b_i++;
            const int a = pa.index, b = pb.index;
            const bool pri_a = pa.priority, pri_b = pb.priority;
            if (pa.front) {
                if (layers)                         // in front of everything
                    layers->layer[LineLayers::SpritesHigh][ox] = colors[a];
                else
                    out[ox] = colors[a];
                continue;
            }
            const int s = sprite_pixel[ox];
            const bool pri_s = s && sprite_priority[ox];
            if (layers) {
                if (b) layers->layer[pri_b ? LineLayers::BHigh : LineLayers::BLow][ox] = colors[b];
                if (a) layers->layer[pri_a ? LineLayers::AHigh : LineLayers::ALow][ox] = colors[a];
                if (s && sprite_on_b[ox]) layers->layer[LineLayers::SpritesB][ox] = colors[s];
                else if (s) layers->layer[pri_s ? LineLayers::SpritesHigh : LineLayers::SpritesLow][ox] = colors[s];
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
            out[ox] = pixel < 0 ? backdrop : colors[pixel];
        }
        return;
    }
    uint64_t memo_key = ~0ull;                      // the water's last result (see below)
    const Pixel* memo_pb = nullptr;
    uint32_t memo_out = 0;
    for (int ox = 0; ox < out_width; ox++) {
        const bool in_window = window_used[x_i];
        const Pixel& pa = in_window ? window_px[x_i] : a_px[a_i];
        const Pixel& pb = b_px[b_i];
        const int a_at = a_i;
        if (++x_r == scale) x_r = 0, x_i++;
        if (++a_r == scale) a_r = 0, a_i++;
        if (++b_r == scale) b_r = 0, b_i++;
        const int a = pa.index, b = pb.index;
        const bool pri_a = pa.priority, pri_b = pb.priority;
        if (any_water && !in_window && water[a_at] != kDry) {
            // Water: what lies in front of plane A at its priority shows as it is; beneath
            // it, the water is laid over what lies there.
            const int s = sprite_pixel[ox];
            const bool pri_s = s && sprite_priority[ox];
            const int alpha = water_alpha[a_at];
            const uint32_t tint = water_rgb[a_at];
            if (layers) {
                if (b) layers->layer[pri_b ? LineLayers::BHigh : LineLayers::BLow][ox] = colors[b];
                if (s && sprite_on_b[ox]) layers->layer[LineLayers::SpritesB][ox] = colors[s];
                else if (s) layers->layer[pri_s ? LineLayers::SpritesHigh : LineLayers::SpritesLow][ox] = colors[s];
                if (layers->water[pri_a]) {         // premultiplied
                    layers->water[pri_a][ox] = alpha ? (uint32_t)alpha << 24 | (blend(0, tint, alpha) & 0xFFFFFF) : 0;
                    if (has_behind[a_at])
                        layers->layer[pri_a ? LineLayers::AHigh : LineLayers::ALow][ox] = behind_rgb[a_at];
                } else if (a) {
                    layers->layer[pri_a ? LineLayers::AHigh : LineLayers::ALow][ox] = colors[a];
                }
                continue;
            }
            // Neighbouring output pixels (with smooth scrolling, several per game pixel) mostly
            // have the same inputs: reuse the last result then.
            bool over_priority = false;
            const uint32_t over = !overlay_rows.empty() && !s ? overlay_at(ox, scale, over_priority) : 0;
            const uint64_t key = (uint64_t)over << 32 | (uint32_t)a_at << 16 | (uint32_t)(s << 1 | pri_s) << 8
                                 | (uint32_t)(b << 1 | pri_b);
            if (key == memo_key && &pb == memo_pb) {
                out[ox] = memo_out;
                continue;
            }
            memo_key = key, memo_pb = &pb;
            if (has_behind[a_at]) {                 // plane A paints what lies beneath
                out[ox] = memo_out = s && (pri_s || !pri_a) ? colors[s] : !pri_a && b && pri_b ? colors[b]
                                   : blend(behind_rgb[a_at], tint, alpha);
                continue;
            }
            int front = 0, under = 0;
            if (s && pri_s) front = s;
            else if (!pri_a && b && pri_b) front = b;
            else if (!pri_a && s) front = s;
            else if (b && pri_b) under = b;
            else if (s) under = s;
            else under = b;
            uint32_t beneath = under ? colors[under] : backdrop;
            if (over && (over_priority || !(b && pri_b)))
                beneath = blend(beneath, over, over >> 24);
            out[ox] = memo_out = front ? colors[front] : blend(beneath, tint, alpha);
            continue;
        }
        if (pa.front) {
            if (layers)                             // in front of everything
                layers->layer[LineLayers::SpritesHigh][ox] = colors[a];
            else
                out[ox] = colors[a];
            continue;
        }
        const int s = sprite_pixel[ox];
        const bool pri_s = s && sprite_priority[ox];
        // An overlay, behind the other sprites: its colour here (alpha in the top byte).
        bool over_priority = false;
        const uint32_t over = !overlay_rows.empty() && !s ? overlay_at(ox, scale, over_priority) : 0;
        if (layers) {
            if (b) layers->layer[pri_b ? LineLayers::BHigh : LineLayers::BLow][ox] = colors[b];
            if (a) layers->layer[pri_a ? LineLayers::AHigh : LineLayers::ALow][ox] = colors[a];
            if (s && sprite_on_b[ox]) layers->layer[LineLayers::SpritesB][ox] = colors[s];
            else if (s) layers->layer[pri_s ? LineLayers::SpritesHigh : LineLayers::SpritesLow][ox] = colors[s];
            else if ((over >> 24) >= 128)           // the layers keep pixels whole
                layers->layer[over_priority ? LineLayers::SpritesHigh : LineLayers::SpritesLow][ox] =
                    0xFF000000u | over;
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
        out[ox] = pixel < 0 ? backdrop : colors[pixel];
        if ((over >> 24) && (over_priority || !((a && pri_a) || (b && pri_b))))
            out[ox] = blend(out[ox], over, over >> 24);
    }
}

// render_pixels as it was before Smooth Water, Smooth Glass and the spout and waterfall
// morphs: used while none of them is active (nearly everywhere), as it is measurably faster.
void Vdp::render_pixels_plain(int line, uint32_t* out, int width, int left, int scale, LineLayers* layers,
                        uint32_t backdrop, const uint8_t* sprite_pixel, const bool* sprite_priority,
                        const bool* sprite_on_b, int sub_a, int sub_b, int hs_a, int hs_b, bool window_line,
                        int window_hp, bool window_right, int window_w, int native_width, int window_period,
                        int base_a, int base_b, int base_window, int plane_w, int plane_h)
{
    auto floor_div = [](int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); };
    const int out_width = width * scale;

    uint32_t colors[64];
    for (int i = 0; i < 64; i++)
        colors[i] = color(i);

    // Plane pixels: the palette index (0 = transparent) and priority; for plane A also
    // whether it is a strip fill drawn in front of everything.
    struct Pixel { uint8_t index; bool priority; bool front; };
    constexpr int kSpan = kMaxWidth + 8;
    Pixel window_px[kSpan], a_px[kSpan], b_px[kSpan];
    bool window_used[kSpan];

    // The window (the HUD), by unshifted position x.
    const int x_lo = -left, x_hi = floor_div(out_width - 1, scale) - left;
    for (int x = x_lo; x <= x_hi; x++) {
        Pixel& p = window_px[x - x_lo];
        bool pri = false;
        const bool in_native = x >= 0 && x < native_width;
        const bool in_window = in_native && (window_line || (window_right ? x >= window_hp : x < window_hp));
        window_used[x - x_lo] = in_window || (window_line && !in_native);
        if (in_window) {
            p.index = (uint8_t)plane_pixel(base_window, x, line, window_w, 32, pri);
        } else if (window_line && !in_native) {
            int wx = window_period ? ((x % window_period) + window_period) % window_period
                                   : (x < 0 ? 0 : native_width - 8) + (((x % 8) + 8) % 8);
            p.index = (uint8_t)plane_pixel(base_window, wx, line, window_w, 32, pri);
        } else {
            p.index = 0;
        }
        p.priority = pri;
        p.front = false;
    }

    // Plane A, by its position xa.
    const int vs_a = vsram[0], vs_b = vsram[1];
    const int a_lo = floor_div(-sub_a, scale) - left, a_hi = floor_div(out_width - 1 - sub_a, scale) - left;
    for (int xa = a_lo; xa <= a_hi; xa++) {
        Pixel& p = a_px[xa - a_lo];
        bool pri = false;
        int px = xa - hs_a, py = line + vs_a;
        const bool remapped = (extend_plane_a_inside || xa < 0 || xa >= native_width) && extend_plane_a
                              && extend_plane_a(xa, line, px, py);
        int a = remapped ? strip_pixel(base_a, px, py, plane_w, plane_h, pri)
                         : plane_pixel(base_a, px, py, plane_w, plane_h, pri);
        if (clean_black_edges && a && (cram[a] & 0xEEE) == 0 && (edge_palette[a] & 0xEEE) == 0)
            a = clean_black_edge(a, base_a, px, py, plane_w, plane_h);
        p.index = (uint8_t)a;
        p.priority = pri;
        p.front = remapped && (strip_fill_in_front || (strip_fill_in_front_left && xa < 0)) && a;
    }

    // Plane B, by its position xb.
    const int b_lo = floor_div(-sub_b, scale) - left, b_hi = floor_div(out_width - 1 - sub_b, scale) - left;
    for (int xb = b_lo; xb <= b_hi; xb++) {
        Pixel& p = b_px[xb - b_lo];
        bool pri = false;
        int bx = xb - hs_b, by = line + vs_b;
        const bool b_remapped = (extend_plane_b_inside || xb < 0 || xb >= native_width) && extend_plane_b
                                && extend_plane_b(xb, line, bx, by);
        p.index = (uint8_t)plane_pixel(base_b, bx, by, plane_w, plane_h, pri);
        if (b_remapped && strip_b_priority_right && xb >= native_width)
            pri = true;
        p.priority = pri;
        p.front = false;
    }

    // Each output pixel: step the three positions (x, xa, xb) along without dividing.
    int x_i = 0, x_r = 0;
    int a_i = 0, a_r = -sub_a - floor_div(-sub_a, scale) * scale;
    int b_i = 0, b_r = -sub_b - floor_div(-sub_b, scale) * scale;
    for (int ox = 0; ox < out_width; ox++) {
        const Pixel& pa = window_used[x_i] ? window_px[x_i] : a_px[a_i];
        const Pixel& pb = b_px[b_i];
        if (++x_r == scale) x_r = 0, x_i++;
        if (++a_r == scale) a_r = 0, a_i++;
        if (++b_r == scale) b_r = 0, b_i++;
        const int a = pa.index, b = pb.index;
        const bool pri_a = pa.priority, pri_b = pb.priority;
        if (pa.front) {
            if (layers)                             // in front of everything
                layers->layer[LineLayers::SpritesHigh][ox] = colors[a];
            else
                out[ox] = colors[a];
            continue;
        }
        const int s = sprite_pixel[ox];
        const bool pri_s = s && sprite_priority[ox];
        if (layers) {
            if (b) layers->layer[pri_b ? LineLayers::BHigh : LineLayers::BLow][ox] = colors[b];
            if (a) layers->layer[pri_a ? LineLayers::AHigh : LineLayers::ALow][ox] = colors[a];
            if (s && sprite_on_b[ox]) layers->layer[LineLayers::SpritesB][ox] = colors[s];
            else if (s) layers->layer[pri_s ? LineLayers::SpritesHigh : LineLayers::SpritesLow][ox] = colors[s];
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
        out[ox] = pixel < 0 ? backdrop : colors[pixel];
    }
}
