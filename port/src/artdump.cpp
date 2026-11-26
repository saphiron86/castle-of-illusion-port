// coi-art-dump: dumps the game's art, as readable pictures, for making art packs (artpack.h,
// docs/notes/art-packs.md). It sits in a folder beside coi.exe (tools/), finds the player's ROM
// in rom/ beside coi.exe, and writes art-dump/ beside itself:
//
//   shared/     what two or more stages use: blocks.png, backgrounds/, sprites/
//   stageNN/    what only stage NN uses: blocks.png (its 16 x 16 building blocks on one sheet),
//               background.png (plane B, whole) and sprites/<object>/animAA_frameFF.png
//
// Each picture has a .tiles file beside it: which 8 x 8 tile (by its pattern, artpack.h) sits
// where, flipped how, and the colours it was drawn in. A pack is this folder with pictures
// redrawn larger; the port cuts them back into tiles by those files.
//
// The game's art is in no file of the port: coi.exe draws it from the player's ROM. For each
// stage this runs coi.exe headless twice: to the stage's start (its RAM, VRAM and colours, for
// the blocks and the background), then sweeping Mickey across the whole layout (--poke-file)
// while --sprite-capture saves each set of sprites an object shows. The sprites' frames are
// drawn from the ROM's mapping tables as the game's DrawObject ($001AC6) does; Mickey's
// graphics are streamed per frame, from the table at $016B80.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "artpack.h"

namespace fs = std::filesystem;

namespace {

// The game's stage index ($FFFF42) for each stage in play order (stages.h).
constexpr int kStageIndex[28] = {0, 2, 3, 1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                 21, 14, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 26, 27};
constexpr int kStages = 28;
constexpr int kStartFrames = 420;           // --go-to-stage 60:N, settled at its start
constexpr int kSweepStart = 300;            // the sweep begins (play starts ~100-150 frames in)
constexpr uint32_t kMickeyArt = 0x016B80;   // Mickey's graphics per frame (32 tiles each)

struct ObjectName {
    int id;
    const char* name;
};
const ObjectName kObjectNames[] = {
#include "object_names.inc"
};

std::string object_name(int id)
{
    for (const ObjectName& o : kObjectNames)
        if (o.id == id)
            return o.name;
    char name[16];
    std::snprintf(name, sizeof(name), "Object%02X", id);
    return name;
}

bool read_file(const fs::path& path, std::vector<uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

uint16_t be16(const std::vector<uint8_t>& d, uint32_t a)
{
    return a + 1 < d.size() ? (uint16_t)(d[a] << 8 | d[a + 1]) : 0;
}
uint32_t be32(const std::vector<uint8_t>& d, uint32_t a) { return (uint32_t)be16(d, a) << 16 | be16(d, a + 2); }
int s8(int v) { return v & 0x80 ? v - 256 : v; }

uint32_t argb(uint16_t c)
{
    auto level = [](int v) { return (uint32_t)(v * 255 / 7); };
    return 0xFF000000 | level((c >> 1) & 7) << 16 | level((c >> 5) & 7) << 8 | level((c >> 9) & 7);
}

// A picture with its tiles, for its .tiles file.
struct Canvas {
    struct Cell {
        int x, y;
        uint64_t hash;
        int flags;      // 1: flipped across, 2: flipped up and down, 4: partly hidden, 8: overlapping another
        int palette;
    };
    int w = 0, h = 0;
    std::vector<uint32_t> px;               // 0xAARRGGBB, 0 = clear
    std::vector<int> owner;                 // the cell that drew each pixel, -1 none
    std::vector<std::vector<uint32_t>> palettes;
    std::vector<Cell> cells;
    std::vector<std::pair<int, int>> blocks;    // block sheets: each block's top left corner
    // Sprite pieces (one sprite of 1-4 x 1-4 tiles): corner, size in tiles, flips, and each tile's
    // cell (index in `cells`, -1 for a blank tile) in VRAM order (down the columns).
    struct PieceAt {
        int x, y, w, h, flags;
        std::vector<int> cells;
    };
    std::vector<PieceAt> pieces;

    void init(int width, int height)
    {
        w = width, h = height;
        px.assign((size_t)w * h, 0);
        owner.assign((size_t)w * h, -1);
        palettes.clear();
        cells.clear();
        blocks.clear();
        pieces.clear();
    }
    int palette_of(const uint32_t* colours)
    {
        std::vector<uint32_t> p(colours, colours + 16);
        for (size_t i = 0; i < palettes.size(); i++)
            if (palettes[i] == p)
                return (int)i;
        palettes.push_back(p);
        return (int)palettes.size() - 1;
    }
    // Draws an 8 x 8 tile's pattern at (x, y); `behind`: only where nothing is drawn yet (sprites,
    // drawn front first).
    void tile(const uint8_t* pattern, int x, int y, bool hflip, bool vflip, const uint32_t* colours, bool behind)
    {
        bool any = false;
        for (int i = 0; i < 32; i++)
            any |= pattern[i] != 0;
        if (!any)
            return;
        const int cell = (int)cells.size();
        cells.push_back({x, y, art::pattern_hash(pattern), (hflip ? 1 : 0) | (vflip ? 2 : 0), palette_of(colours)});
        for (int py = 0; py < 8; py++)
            for (int px_ = 0; px_ < 8; px_++) {
                const int fx = hflip ? 7 - px_ : px_, fy = vflip ? 7 - py : py;
                const int pair = pattern[fy * 4 + fx / 2], v = fx & 1 ? pair & 15 : pair >> 4;
                const int X = x + px_, Y = y + py;
                if (!v || X < 0 || Y < 0 || X >= w || Y >= h)
                    continue;
                const size_t at = (size_t)Y * w + X;
                if (behind && px[at]) {
                    cells[cell].flags |= 4;
                    continue;
                }
                px[at] = colours[v];
                owner[at] = cell;
            }
    }
    uint64_t content_hash() const
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) {
            for (int i = 0; i < 8; i++) {
                h ^= (v >> (i * 8)) & 255;
                h *= 1099511628211ull;
            }
        };
        mix((uint64_t)w << 32 | (uint32_t)h);
        for (uint32_t p : px)
            mix(p);
        for (const Cell& c : cells)
            mix(c.hash ^ ((uint64_t)c.x << 40) ^ ((uint64_t)c.y << 20) ^ (uint64_t)c.flags);
        return h;
    }
    bool save(const fs::path& png) const
    {
        fs::create_directories(png.parent_path());
        SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
        if (!s)
            return false;
        // See-through pixels keep alpha 0 but take the colour of the nearest pixel shown (up to
        // 8 away), so an upscaler that blends across an edge blends towards the shape's own
        // colour, not black: no dark fringe once its alpha is snapped back.
        std::vector<uint32_t> bled = px;
        std::vector<uint8_t> set(px.size());
        for (size_t i = 0; i < px.size(); i++)
            set[i] = px[i] != 0;
        for (int pass = 0; pass < 8; pass++) {
            std::vector<uint8_t> next = set;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const size_t i = (size_t)y * w + x;
                    if (set[i])
                        continue;
                    static const int kNear[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                    for (const auto& d : kNear) {
                        const int nx = x + d[0], ny = y + d[1];
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || !set[(size_t)ny * w + nx])
                            continue;
                        bled[i] = bled[(size_t)ny * w + nx] & 0x00FFFFFF;
                        next[i] = 1;
                        break;
                    }
                }
            set.swap(next);
        }
        for (int y = 0; y < h; y++)
            std::memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch, &bled[(size_t)y * w], (size_t)w * 4);
        const bool ok = SDL_SavePNG(s, png.string().c_str());
        SDL_DestroySurface(s);
        fs::path tiles = png;
        tiles.replace_extension(".tiles");
        std::ofstream out(tiles);
        out << "art-tiles 1\nsize " << w << " " << h << "\n";
        char line[256];
        for (size_t i = 0; i < palettes.size(); i++) {
            out << "palette " << i;
            for (uint32_t c : palettes[i]) {
                std::snprintf(line, sizeof(line), " %08X", c);
                out << line;
            }
            out << "\n";
        }
        // A block (block sheets): its 16 x 16 square, all four tiles, blank ones too.
        for (const auto& [x, y] : blocks)
            out << "block " << x << " " << y << "\n";
        // A sprite piece: x y width height flags, then its tiles' cells (counted from 0 in the
        // order of the cell lines, - for a blank tile), down each column, left to right.
        for (const PieceAt& p : pieces) {
            out << "piece " << p.x << " " << p.y << " " << p.w << " " << p.h << " " << p.flags;
            for (int i : p.cells) {
                if (i < 0)
                    out << " -";
                else
                    out << " " << i;
            }
            out << "\n";
        }
        // A cell: x y pattern flags palette, then the pixels of its square it drew itself (bit
        // y * 8 + x, as the picture shows them), for tiles that overlap others.
        for (size_t i = 0; i < cells.size(); i++) {
            const Cell& c = cells[i];
            uint64_t own = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    const int X = c.x + x, Y = c.y + y;
                    if (X >= 0 && Y >= 0 && X < w && Y < h && owner[(size_t)Y * w + X] == (int)i)
                        own |= 1ull << (y * 8 + x);
                }
            std::snprintf(line, sizeof(line), "cell %d %d %s %d %d %016llx\n", c.x, c.y, art::hash_name(c.hash).c_str(),
                          c.flags, c.palette, (unsigned long long)own);
            out << line;
        }
        return ok && (bool)out;
    }
};

// A stage's start as coi.exe saved it (--ram, --vram, --cram).
struct StageState {
    std::vector<uint8_t> ram, vram, cram;   // cram: 64 words, then the registers and VSRAM
    uint32_t colours[64];

    bool load(const fs::path& stem)
    {
        if (!read_file(stem.string() + ".ram", ram) || !read_file(stem.string() + ".vram", vram)
            || !read_file(stem.string() + ".cram", cram) || ram.size() < 0x10000 || vram.size() < 0x10000
            || cram.size() < 128)
            return false;
        for (int line = 0; line < 4; line++) {
            // A line fading (every colour at or below the colours the fade moves towards,
            // $FFF800, or every one at or above them, fading from white): those colours. A
            // picture dumped mid-fade has its colours run together, and an upscaler then
            // leaves it unsmoothed. Colour cycles (the Sega logo's) keep what CRAM shows.
            bool below = true, above = true, target = false;
            for (int i = 1; i < 16; i++) {
                const int c = be16(cram, (line * 16 + i) * 2), t = be16(ram, 0xF800 + (line * 16 + i) * 2);
                target |= t != 0;
                for (int shift = 1; shift <= 9; shift += 4) {
                    below &= ((c >> shift) & 7) <= ((t >> shift) & 7);
                    above &= ((c >> shift) & 7) >= ((t >> shift) & 7);
                }
            }
            const bool fading = target && (below || above);
            for (int i = 0; i < 16; i++)
                colours[line * 16 + i] = argb(fading ? be16(ram, 0xF800 + (line * 16 + i) * 2) : be16(cram, (line * 16 + i) * 2));
        }
        return true;
    }
    uint16_t w(uint32_t a) const { return be16(ram, a & 0xFFFF); }
    uint32_t l(uint32_t a) const { return (uint32_t)w(a) << 16 | w(a + 2); }
};

// A plane's layout (CameraA at $FFFA80, CameraB at $FFFAC0): blocks of 2 x 2 tiles.
struct Layout {
    uint32_t layout = 0, blocks = 0;
    int width = 0, height = 0, stride = 0;  // in blocks
    Layout(const StageState& s, uint32_t camera)
        : layout(s.l(camera + 8) & 0xFFFF), blocks(s.l(camera + 0x10) & 0xFFFF), width(s.w(camera + 0xE)),
          height(s.w(camera + 0xC)), stride(s.w(camera + 0xE))
    {
    }
};

// A block as drawn: its four tile words and what they show.
struct Block {
    uint16_t words[4];
    uint8_t tiles[4][32];
    uint32_t colours[4][16];
    std::string key() const
    {
        std::string k;
        for (int i = 0; i < 4; i++) {
            const uint16_t flags = words[i] & 0xF800;
            k.append((const char*)&flags, 2);
            k.append((const char*)tiles[i], 32);
            k.append((const char*)colours[i], 64);
        }
        return k;
    }
    void draw(Canvas& c, int x, int y) const
    {
        static const int kAt[4][2] = {{0, 0}, {8, 0}, {0, 8}, {8, 8}};
        for (int i = 0; i < 4; i++)
            c.tile(tiles[i], x + kAt[i][0], y + kAt[i][1], words[i] & 0x800, words[i] & 0x1000, colours[i], false);
    }
};

Block block_of(const StageState& s, uint32_t blocks, int b)
{
    Block k;
    for (int i = 0; i < 4; i++) {
        k.words[i] = s.w(blocks + b * 8 + i * 2);
        std::memcpy(k.tiles[i], &s.vram[(k.words[i] & 0x7FF) * 32], 32);
        std::memcpy(k.colours[i], &s.colours[((k.words[i] >> 13) & 3) * 16], 64);
    }
    return k;
}

void draw_layout(const StageState& s, const Layout& l, Canvas& c)
{
    c.init(l.width * 16, l.height * 16);
    for (int by = 0; by < l.height; by++)
        for (int bx = 0; bx < l.width; bx++) {
            block_of(s, l.blocks, s.ram[(l.layout + by * l.stride + bx) & 0xFFFF]).draw(c, bx * 16, by * 16);
            c.blocks.push_back({bx * 16, by * 16});
        }
}

// Sprites: the mapping table's animations, as render_sprites.py (outside the repository) did.
struct Piece {
    int x, y, w, h;
    uint16_t tile;
};
struct Frame {
    std::vector<Piece> pieces;
    int art = 0;
};

bool parse_mapping(const std::vector<uint8_t>& rom, uint32_t mapping, std::vector<std::vector<std::pair<uint32_t, bool>>>& anims)
{
    std::vector<uint32_t> offsets;
    uint32_t limit = 0;
    for (int i = 0;; i++) {
        const uint32_t a = mapping + i * 2;
        if (!offsets.empty() && a >= limit)
            break;
        const uint32_t off = be16(rom, a);
        if (off == 0 || (off & 1) || off >= 0x4000 || offsets.size() >= 64) {
            if (offsets.empty())
                return false;
            break;
        }
        offsets.push_back(off);
        limit = mapping + *std::min_element(offsets.begin(), offsets.end());
    }
    for (uint32_t off : offsets) {
        const uint32_t a = mapping + off;
        const int count = a < rom.size() ? rom[a] : 0;
        if (count < 1 || count > 64)
            return false;
        std::vector<std::pair<uint32_t, bool>> frames;
        for (int j = 0; j < count; j++) {
            const uint16_t fw = be16(rom, a + 2 + j * 2);
            const uint32_t frame = a + 2 + (fw & 0x7FFF) - 2;
            if (frame + 6 >= rom.size())
                return false;
            frames.push_back({frame, (fw & 0x8000) != 0});
        }
        anims.push_back(frames);
    }
    return true;
}

bool frame_pieces(const std::vector<uint8_t>& rom, uint32_t frame, bool mirrored, uint16_t base, Frame& out)
{
    const int n = rom[frame];
    out.art = rom[frame + 5];
    if (n & 0x80)
        return true;
    if (n > 40)
        return false;
    uint32_t p = frame + 6;
    for (int i = 0; i <= n; i++, p += 5) {
        if (p + 5 > rom.size())
            return false;
        const int size = rom[p + 1];
        Piece piece{0, s8(rom[p]), ((size >> 2) & 3) + 1, (size & 3) + 1, (uint16_t)(be16(rom, p + 2) + base)};
        if (mirrored) {
            piece.tile ^= 0x0800;
            piece.x = s8((-(rom[p + 4] + ((size * 2 + 8) & 0xF8))) & 0xFF);
        } else {
            piece.x = s8(rom[p + 4]);
        }
        out.pieces.push_back(piece);
    }
    return true;
}

void draw_frame(const Frame& f, const std::vector<uint8_t>& vram, const uint32_t* colours, int left, int top, Canvas& c)
{
    for (const Piece& p : f.pieces) {                       // the first piece is in front
        const bool hflip = p.tile & 0x0800, vflip = p.tile & 0x1000;
        const uint32_t* line = colours + ((p.tile >> 13) & 3) * 16;
        Canvas::PieceAt at{p.x - left, p.y - top, p.w, p.h, (hflip ? 1 : 0) | (vflip ? 2 : 0), {}};
        for (int cx = 0; cx < p.w; cx++)
            for (int cy = 0; cy < p.h; cy++) {
                const int t = ((p.tile & 0x7FF) + cx * p.h + cy) & 0x7FF;
                const int sx = hflip ? p.w - 1 - cx : cx, sy = vflip ? p.h - 1 - cy : cy;
                const size_t before = c.cells.size();
                c.tile(&vram[t * 32], p.x + sx * 8 - left, p.y + sy * 8 - top, hflip, vflip, line, true);
                at.cells.push_back(c.cells.size() > before ? (int)before : -1);
            }
        c.pieces.push_back(std::move(at));
    }
    // Tiles that overlap: each one's square shows some of the other (flag 8), so a pack takes
    // them only where no picture shows the tile clear of others.
    for (size_t i = 0; i < c.cells.size(); i++)
        for (size_t j = i + 1; j < c.cells.size(); j++)
            if (std::abs(c.cells[i].x - c.cells[j].x) < 8 && std::abs(c.cells[i].y - c.cells[j].y) < 8)
                c.cells[i].flags |= 8, c.cells[j].flags |= 8;
}

// A sprite set's frames, drawn.
struct SpriteSet {
    std::string name;
    int stage = 0;
    std::vector<std::pair<std::string, Canvas>> frames;    // file name, picture
    uint64_t key = 0;
    int frame = 0;                                          // when it was captured
};

std::vector<SpriteSet> stage_sprites(const std::vector<uint8_t>& rom, const fs::path& dir, int stage)
{
    std::vector<SpriteSet> sets;
    std::map<std::tuple<int, uint32_t, int>, std::set<int>> played;    // (object, mapping, base) -> animations
    {
        std::ifstream in(dir / "anims.txt");
        int st, a;
        std::string obj, mapping, base;
        while (in >> st >> obj >> mapping >> base >> a)
            played[{std::stoi(obj, nullptr, 16), (uint32_t)std::stoul(mapping, nullptr, 16), std::stoi(base, nullptr, 16)}]
                .insert(a / 2);
    }
    // A set captured again (its first capture's colours weren't loaded yet: coi.exe's
    // capture_sprites) is taken from its last capture.
    struct Capture { int n, st, frame; std::string obj_s, mapping_s, base_s; };
    std::vector<Capture> captures;
    {
        std::ifstream in(dir / "captures.txt");
        Capture c;
        std::string slot;
        while (in >> c.n >> c.st >> c.obj_s >> c.mapping_s >> c.base_s >> slot >> c.frame)
            captures.push_back(c);
    }
    auto key_of = [](const Capture& c) {   // (what coi.exe captures by)
        return std::make_tuple(c.st, std::stoi(c.obj_s, nullptr, 16), (uint32_t)std::stoul(c.mapping_s, nullptr, 16),
                               std::stoi(c.base_s, nullptr, 16));
    };
    std::map<std::tuple<int, int, uint32_t, int>, int> last;
    for (const Capture& c : captures)
        last[key_of(c)] = c.n;
    std::set<std::tuple<int, uint32_t, int>> done;
    std::set<std::string> names;
    for (const Capture& c : captures) {
        const int n = c.n, st = c.st, frame = c.frame;
        if (stage > 0 && st != kStageIndex[stage - 1])  // (the sweep got into another stage)
            continue;
        const int obj = std::stoi(c.obj_s, nullptr, 16), base = std::stoi(c.base_s, nullptr, 16);
        const uint32_t mapping = (uint32_t)std::stoul(c.mapping_s, nullptr, 16);
        const bool mickey = obj == 0x01;
        const auto key = std::make_tuple(obj, mapping, mickey ? 0 : base);
        if (last[key_of(c)] != n || !done.insert(key).second)
            continue;
        std::vector<std::vector<std::pair<uint32_t, bool>>> anims;
        if (!parse_mapping(rom, mapping, anims))
            continue;
        std::vector<uint8_t> vram, pal;
        const std::string cap = (dir / ("cap" + std::to_string(n))).string();
        if (!read_file(cap + ".vram", vram) || vram.size() < 0x10000)
            continue;
        if (!read_file(cap + ".pal", pal) || pal.size() < 128)
            read_file(cap + ".cram", pal);
        if (pal.size() < 128)
            continue;
        uint32_t colours[64];
        for (int i = 0; i < 64; i++)
            colours[i] = argb(be16(pal, i * 2));
        std::vector<std::vector<Frame>> frames(anims.size());
        bool bad = false;
        for (size_t a = 0; a < anims.size() && !bad; a++)
            for (const auto& [addr, mirrored] : anims[a]) {
                Frame f;
                bad |= !frame_pieces(rom, addr, mirrored, (uint16_t)base, f);
                frames[a].push_back(f);
            }
        if (bad)
            continue;
        // The animations seen played with these tiles (several kinds of object share a table,
        // each with its own tiles: the others would come out scrambled); for Mickey, those
        // whose frames all name their graphics.
        std::vector<int> chosen;
        for (size_t a = 0; a < frames.size(); a++) {
            bool ok;
            if (mickey) {
                ok = !frames[a].empty();
                for (const Frame& f : frames[a])
                    ok &= f.art != 0;
            } else {
                auto p = played.find(key);
                ok = p != played.end() && p->second.count((int)a);
            }
            if (ok)
                chosen.push_back((int)a);
        }
        if (chosen.empty())
            continue;
        int left = 1 << 20, top = 1 << 20, right = -(1 << 20), bottom = -(1 << 20);
        for (int a : chosen)
            for (const Frame& f : frames[a])
                for (const Piece& p : f.pieces) {
                    left = std::min(left, p.x), top = std::min(top, p.y);
                    right = std::max(right, p.x + p.w * 8), bottom = std::max(bottom, p.y + p.h * 8);
                }
        if (right <= left || right - left > 512 || bottom - top > 512)
            continue;
        SpriteSet set;
        set.stage = stage;
        set.frame = frame;
        set.name = mickey ? "Mickey" : object_name(obj);
        if (!mickey && !names.insert(set.name).second) {
            char more[32];
            std::snprintf(more, sizeof(more), "_%06X_%04X", mapping, base);
            set.name += more;
        }
        for (int a : chosen)
            for (size_t j = 0; j < frames[a].size(); j++) {
                const Frame& f = frames[a][j];
                if (mickey && f.art)                // his graphics for this frame, at tile $40
                    std::memcpy(&vram[0x800], &rom[std::min<size_t>(be32(rom, kMickeyArt + (f.art - 1) * 4), rom.size() - 0x400)],
                                0x400);
                Canvas c;
                c.init(right - left, bottom - top);
                draw_frame(f, vram, colours, left, top, c);
                char file[32];
                std::snprintf(file, sizeof(file), "anim%02d_frame%02zu.png", a, j);
                set.frames.push_back({file, std::move(c)});
            }
        uint64_t h = 1469598103934665603ull;
        for (const auto& [file, c] : set.frames)
            h = (h ^ c.content_hash()) * 1099511628211ull;
        set.key = h;
        sets.push_back(std::move(set));
    }
    return sets;
}

// Runs coi.exe with these arguments, waiting for it; true when it ran to the end.
bool run_engine(const fs::path& engine, const std::vector<std::string>& args)
{
    std::vector<std::string> all = {engine.string()};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<const char*> argv;
    for (const std::string& a : all)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process* p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p)
        return false;
    int code = -1;
    SDL_WaitProcess(p, true, &code);
    SDL_DestroyProcess(p);
    return code == 0;
}

// Everything one stage gives.
struct StageArt {
    bool ok = false;
    std::vector<Block> blocks;              // plane A's, in order of first use
    Canvas background;
    std::vector<SpriteSet> sprites;
    std::vector<Canvas> others;             // sprites no object's animations show (platforms...)
    std::vector<Canvas> animated;           // planes' squares the start doesn't show (animated tiles), among their neighbours
};

// A square's tiles and flips, colours aside.
std::string tiles_key(const Block& b)
{
    std::string k;
    for (int q = 0; q < 4; q++) {
        const uint16_t flags = b.words[q] & 0x1800;
        k.append((const char*)&flags, 2);
        k.append((const char*)b.tiles[q], 32);
    }
    return k;
}

// The squares planes A and B show during the sweep that the stage's start doesn't (`known`):
// tiles the game animates by rewriting their pixels (gears, the sea, waterfalls), each frame
// once.
template <typename Shown>
bool draw_table(const StageState& s, int base, int w, int h, Canvas& c, Shown shown, int rx = 0, int ry = 0);

// Each look's new squares go out as the part of the plane round them (a square more on each
// side), so an upscaler smooths their edges into their neighbours: alone on a sheet, the
// animated gears' squares showed seams.
void animated_squares(const fs::path& looks, int stage, std::set<std::string>& known, std::vector<Canvas>& out)
{
    std::vector<fs::path> stems;
    for (const auto& e : fs::directory_iterator(looks))
        if (e.path().extension() == ".ram")
            stems.push_back(e.path().parent_path() / e.path().stem());
    std::sort(stems.begin(), stems.end());
    for (const fs::path& stem : stems) {
        StageState s;
        if (!s.load(stem) || s.w(0xF300) != 0x0E || s.ram[0xFF43] != kStageIndex[stage - 1])
            continue;
        const uint8_t* reg = &s.cram[128];
        auto size = [](int v) { return v == 1 ? 64 : v == 3 ? 128 : 32; };
        const int w = size(reg[16] & 3), h = size((reg[16] >> 4) & 3);
        if (w * h > 4096)
            continue;
        for (const int base : {(reg[2] & 0x38) << 10, (reg[4] & 7) << 13}) {
            std::vector<uint8_t> fresh((size_t)(w / 2) * (h / 2));
            bool any_fresh = false;
            for (int cy = 0; cy < h; cy += 2)
                for (int cx = 0; cx < w; cx += 2) {
                    Block b;
                    bool any = false;
                    for (int q = 0; q < 4; q++) {
                        const int x = cx + (q & 1), y = cy + (q >> 1);
                        b.words[q] = be16(s.vram, (uint32_t)(base + (y * w + x) * 2) & 0xFFFF);
                        std::memcpy(b.tiles[q], &s.vram[(b.words[q] & 0x7FF) * 32], 32);
                        for (int i = 0; i < 32; i++)
                            any |= b.tiles[q][i] != 0;
                    }
                    if (any && known.insert(tiles_key(b)).second)
                        fresh[(size_t)(cy / 2) * (w / 2) + cx / 2] = 1, any_fresh = true;
                }
            if (!any_fresh)
                continue;
            // The new squares and those beside them (wrapping round the table).
            auto shown = [&](int cx, int cy) {
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const int sx = ((cx / 2 + dx) % (w / 2) + w / 2) % (w / 2), sy = ((cy / 2 + dy) % (h / 2) + h / 2) % (h / 2);
                        if (fresh[(size_t)sy * (w / 2) + sx])
                            return true;
                    }
                return false;
            };
            Canvas c;
            if (draw_table(s, base, w, h, c, shown))
                out.push_back(std::move(c));
        }
    }
}

// The sprites on screen, from the sprite table, front first.
std::vector<Piece> sat_pieces(const StageState& s)
{
    const uint8_t* reg = &s.cram[128];
    const bool h40 = reg[12] & 1;
    const int table = (reg[5] & (h40 ? 0x7E : 0x7F)) << 9, width = h40 ? 320 : 256;
    std::vector<Piece> out;
    for (int n = 0, link = 0; n < 80; n++) {
        const uint32_t a = (uint32_t)(table + link * 8) & 0xFFFF;
        const int y = (be16(s.vram, a) & 0x3FF) - 128, size = s.vram[a + 2], x = (be16(s.vram, a + 6) & 0x1FF) - 128;
        if (x > -32 && x < width && y > -32 && y < 224 && x != -128)
            out.push_back({x, y, ((size >> 2) & 3) + 1, (size & 3) + 1, be16(s.vram, a + 4)});
        link = s.vram[a + 3] & 0x7F;
        if (link == 0)
            break;
    }
    return out;
}

uint64_t piece_key_in(const std::vector<uint8_t>& vram, const Piece& p)
{
    uint64_t pattern[16];
    for (int k = 0; k < p.w * p.h; k++)
        pattern[k] = art::pattern_hash(&vram[((p.tile + k) & 0x7FF) * 32]);
    return art::piece_key(p.w, p.h, pattern);
}

// The pieces the objects' frames show (their keys).
void known_pieces(const std::vector<SpriteSet>& sets, std::set<uint64_t>& known)
{
    for (const SpriteSet& set : sets)
        for (const auto& [file, c] : set.frames)
            for (const Canvas::PieceAt& p : c.pieces) {
                uint64_t pattern[16];
                for (size_t k = 0; k < p.cells.size() && k < 16; k++)
                    pattern[k] = p.cells[k] < 0 ? art::empty_pattern() : c.cells[p.cells[k]].hash;
                known.insert(art::piece_key(p.w, p.h, pattern));
            }
}

// Sprites the game draws without an object's mapping table (the leaf platforms of stage 3,
// and the like), from looks at the sprite table during the sweep: touching pieces no object's
// frames show, as one picture each, once.
void other_sprites(const fs::path& looks, int stage, const std::set<uint64_t>& known, std::vector<Canvas>& out)
{
    std::vector<fs::path> stems;
    for (const auto& e : fs::directory_iterator(looks))
        if (e.path().extension() == ".ram")
            stems.push_back(e.path().parent_path() / e.path().stem());
    std::sort(stems.begin(), stems.end());
    std::set<uint64_t> done;
    struct Found {
        Canvas picture;
        std::vector<uint64_t> pieces;
    };
    std::vector<Found> found;
    for (const fs::path& stem : stems) {
        StageState s;
        if (!s.load(stem) || s.w(0xF300) != 0x0E || s.ram[0xFF43] != kStageIndex[stage - 1])   // (this stage's play only)
            continue;
        std::vector<Piece> loose;
        for (const Piece& p : sat_pieces(s))
            if (!known.count(piece_key_in(s.vram, p)))
                loose.push_back(p);
        // Group the pieces that touch.
        std::vector<int> group(loose.size());
        for (size_t i = 0; i < loose.size(); i++)
            group[i] = (int)i;
        std::function<int(int)> root = [&](int i) { return group[i] == i ? i : group[i] = root(group[i]); };
        for (size_t i = 0; i < loose.size(); i++)
            for (size_t j = i + 1; j < loose.size(); j++) {
                const Piece &a = loose[i], &b = loose[j];
                if (a.x <= b.x + b.w * 8 && b.x <= a.x + a.w * 8 && a.y <= b.y + b.h * 8 && b.y <= a.y + a.h * 8)
                    group[root((int)i)] = root((int)j);
            }
        std::map<int, Frame> frames;
        for (size_t i = 0; i < loose.size(); i++)
            frames[root((int)i)].pieces.push_back(loose[i]);
        for (auto& [g, f] : frames) {
            std::vector<uint64_t> keys;
            for (const Piece& p : f.pieces)
                keys.push_back(piece_key_in(s.vram, p) ^ (uint64_t)(p.tile & 0x1800) << 40);
            std::sort(keys.begin(), keys.end());
            uint64_t key = 1469598103934665603ull;
            for (uint64_t k : keys)
                key = (key ^ k) * 1099511628211ull;
            if (!done.insert(key).second)
                continue;
            int left = 1 << 20, top = 1 << 20, right = -(1 << 20), bottom = -(1 << 20);
            for (const Piece& p : f.pieces) {
                left = std::min(left, p.x), top = std::min(top, p.y);
                right = std::max(right, p.x + p.w * 8), bottom = std::max(bottom, p.y + p.h * 8);
            }
            Canvas c;
            c.init(right - left, bottom - top);
            draw_frame(f, s.vram, s.colours, left, top, c);
            if (c.cells.empty())
                continue;
            std::vector<uint64_t> pieces;
            for (const Piece& p : f.pieces)
                pieces.push_back(piece_key_in(s.vram, p));
            found.push_back({std::move(c), std::move(pieces)});
        }
    }
    // The biggest first; a picture whose pieces are all in pictures kept already (a platform cut
    // by the screen's edge, or seen again) is left out.
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.pieces.size() > b.pieces.size(); });
    std::set<uint64_t> covered;
    for (Found& f : found) {
        bool adds = false;
        for (uint64_t k : f.pieces)
            adds |= !covered.count(k);
        if (!adds)
            continue;
        covered.insert(f.pieces.begin(), f.pieces.end());
        out.push_back(std::move(f.picture));
    }
}

StageArt dump_stage(const fs::path& engine, const fs::path& rom_path, const std::vector<uint8_t>& rom,
                    const fs::path& work, int stage)
{
    StageArt art;
    char tag[8];
    std::snprintf(tag, sizeof(tag), "s%02d", stage);
    const fs::path dir = work / tag;
    fs::remove_all(dir);
    fs::create_directories(dir / "sprites");
    fs::create_directories(dir / "looks");
    const std::string go = "60:" + std::to_string(stage);
    const fs::path stem = dir / "start";
    if (!run_engine(engine, {rom_path.string(), "--no-mdplus", "--no-widescreen", "--no-gpu-vdp", "--go-to-stage", go,
                             "--frames", std::to_string(kStartFrames), "--cheat", "invincibility", "--ram",
                             stem.string() + ".ram", "--vram", stem.string() + ".vram", "--cram", stem.string() + ".cram"}))
        return art;
    StageState s;
    if (!s.load(stem))
        return art;
    // Plane A's blocks, plane B whole.
    const Layout a(s, 0xFA80);
    std::set<int> used;
    for (int y = 0; y < a.height; y++)
        for (int x = 0; x < a.width; x++) {
            const int b = s.ram[(a.layout + y * a.stride + x) & 0xFFFF];
            if (used.insert(b).second)
                art.blocks.push_back(block_of(s, a.blocks, b));
        }
    Layout b(s, 0xFAC0);
    if (s.ram[0x9800] & 1)                  // a static plane B: 32 blocks across
        b.width = b.stride = 32, b.height = std::min(b.height, 32);
    if (b.width > 0 && b.height > 0 && b.width <= 256 && b.height <= 256)
        draw_layout(s, b, art.background);
    // The sweep: Mickey across the layout, 8 pixels a frame, in rows 112 pixels apart.
    const fs::path route = dir / "route.txt";
    int frame = kSweepStart;
    {
        std::ofstream out(route);
        bool right = true;
        char line[64];
        for (int y = 48; y < std::max(a.height * 16, 64); y += 112, right = !right)
            for (int i = 0, n = std::max(1, (a.width * 16 - 32) / 8); i <= n; i++, frame++) {
                const int x = 16 + (right ? i : n - i) * 8;
                // (and the exit flag $FFF412 kept clear: passing the exit would end the stage and
                // sweep the next one)
                std::snprintf(line, sizeof(line), "%d:FFDC10=%04X\n%d:FFDC14=%04X\n%d:FFF412.b=00\n", frame, x & 0xFFFF,
                              frame, y & 0xFFFF, frame);
                out << line;
            }
        for (int f = frame; f < frame + 60; f++)
            out << f << ":FFF412.b=00\n";
    }
    if (!run_engine(engine, {rom_path.string(), "--no-mdplus", "--no-widescreen", "--no-gpu-vdp", "--go-to-stage", go,
                             "--cheat", "invincibility", "--cheat", "infinite-tries", "--poke-file", route.string(),
                             "--frames", std::to_string(frame + 60), "--sprite-capture", (dir / "sprites").string(),
                             "--vdp-every", "7", (dir / "looks").string()}))
        return art;
    art.sprites = stage_sprites(rom, dir / "sprites", stage);
    // A boss stage: Mickey stays where he starts (invincible) while the boss goes through its
    // attacks, for the poses the sweep never sees (stage 27's faces, Mizrabel's first ones).
    const bool boss = stage == 5 || stage == 9 || stage == 13 || stage == 23 || stage == 27 || stage == 28;
    if (boss) {
        fs::create_directories(dir / "sprites_boss");
        fs::create_directories(dir / "looks_boss");
        if (run_engine(engine, {rom_path.string(), "--no-mdplus", "--no-widescreen", "--no-gpu-vdp", "--go-to-stage", go,
                                "--cheat", "invincibility", "--cheat", "infinite-tries", "--frames", "4200",
                                "--sprite-capture", (dir / "sprites_boss").string(), "--vdp-every", "7",
                                (dir / "looks_boss").string()})) {
            std::set<uint64_t> have;
            for (const SpriteSet& set : art.sprites)
                have.insert(set.key);
            for (SpriteSet& set : stage_sprites(rom, dir / "sprites_boss", stage))
                if (have.insert(set.key).second) {
                    for (const SpriteSet& other : art.sprites)
                        if (other.name == set.name)
                            set.name += "_fight";
                    art.sprites.push_back(std::move(set));
                }
        }
    }
    std::set<uint64_t> known;
    known_pieces(art.sprites, known);
    other_sprites(dir / "looks", stage, known, art.others);
    if (boss)
        other_sprites(dir / "looks_boss", stage, known, art.others);
    // (7 frames between looks: animations of 2, 4 or 8 frames a step aren't always caught on
    // the same step.)
    std::set<std::string> start_squares;
    for (const Layout& l : {a, b})
        for (int y = 0; y < l.height; y++)
            for (int x = 0; x < l.width; x++)
                start_squares.insert(tiles_key(block_of(s, l.blocks, s.ram[(l.layout + y * l.stride + x) & 0xFFFF])));
    animated_squares(dir / "looks", stage, start_squares, art.animated);
    std::error_code e;
    fs::remove_all(dir / "looks", e);                   // (about 130 KB a look)
    fs::remove_all(dir / "looks_boss", e);
    art.ok = true;
    return art;
}

fs::path find_rom(const fs::path& base)
{
    for (const fs::path& dir : {base / ".." / "rom", base / "rom", fs::path("rom"), fs::path("..") / "rom"}) {
        std::error_code e;
        for (fs::directory_iterator it(dir, e), end; !e && it != end; it.increment(e)) {
            const std::string ext = it->path().extension().string();
            if ((ext == ".bin" || ext == ".md" || ext == ".gen" || ext == ".smd") && fs::file_size(it->path(), e) == 512 * 1024)
                return fs::absolute(it->path());
        }
    }
    return {};
}

void write_readme(const fs::path& out)
{
    std::ofstream f(out / "README.txt");
    f << "Castle of Illusion art dump (made by coi-art-dump from your own ROM: these are the game's\n"
         "graphics, so share packs made from them only if you accept the copyright risk).\n\n"
         "shared/              what two or more stages use\n"
         "  blocks.png         stage building blocks (16 x 16, made of four 8 x 8 tiles), on one sheet\n"
         "                     with an 8 pixel gap between blocks\n"
         "  backgrounds/       backgrounds (plane B) used by several stages\n"
         "  sprites/<object>/  animation frames: animAA_frameFF.png\n"
         "stageNN/             what only stage NN uses: blocks.png, background.png, sprites/\n\n"
         "Every picture has a .tiles file beside it: which tile sits where. Keep it.\n\n"
         "Making a pack: copy this folder to artpack/ beside rom/ (or start coi with --art-pack DIR)\n"
         "and redraw pictures at 2x or 4x their size (all at the same scale), keeping each picture's\n"
         "layout. Transparent pixels show what lies behind. Pictures left at their dumped size are\n"
         "not used.\n\n"
         "Blocks are replaced whole: a block's picture is used wherever the stage shows that block,\n"
         "so draw across its four tiles freely; only its outer edges meet other blocks. Keep each\n"
         "block inside its own 16 x 16 square (the gaps are not used). Backgrounds are replaced by\n"
         "the same 16 x 16 blocks: a block repeated in a background uses its first picture.\n"
         "Sprites are replaced piece by piece (the game builds each frame from a few pieces of up\n"
         "to 32 x 32), so draw across a piece freely. The HUD and anything else are replaced tile\n"
         "by tile. A stage's own picture wins over shared/, and sprites over blocks over\n"
         "backgrounds.\n\n"
         "screens/             everything outside stage play, by what the game is doing: the Sega\n"
         "                     logo, the story, the title and its menu, the options, the scenes\n"
         "                     between worlds, the score screens, Game Over, the ending; hud/ is the\n"
         "                     bar at the bottom in play (window.png, and squares.png: the squares\n"
         "                     that change, such as digits). Each screen is dumped as its planes whole\n"
         "                     (planeA, planeB, window: the game's full tables, so parts may be off\n"
         "                     screen) and its sprites. Planes are replaced in 16 x 16 squares (each\n"
         "                     square counts as a block), sprites piece by piece. Stage art wins\n"
         "                     over screens/ where both show the same tiles.\n";
}

// ---- Screens: everything outside stage play, and the HUD -------------------------------------

// What the game is doing (GameState, $FFF300), as a folder name.
std::string scene_name(const StageState& s)
{
    switch (s.w(0xF300)) {
    case 0x06: return s.ram[0xF40C] < 0x1E ? "boot_story" : "title";
    case 0x08: case 0x0A: return "title_menu";
    case 0x12: return "options";
    case 0x2A: return "opening_story";
    case 0x22: return "between_worlds";
    case 0x24: case 0x26: return "score";
    case 0x2C: case 0x2E: return "ending";
    case 0x16: return "after_ending";
    case 0x0C: case 0x0E: case 0x10: return "hud";      // level play: only its window (the HUD)
    case 0x02: case 0x04: return "sega_logo";
    case 0x1A: return "game_over";
    case 0x1E: return "copyright";
    case 0x20: return "between_scenes";
    default: {
        char name[16];
        std::snprintf(name, sizeof(name), "state_%02X", s.w(0xF300));
        return name;
    }
    }
}

// A table of tiles (a plane or the window) whole, with a block line on each 2 x 2 square from
// even cells that shows anything: the GPU renderer replaces such squares whole.
// `shown` (the window): only squares the screen shows.
// `rx`, `ry` (multiples of 16): the table turned round so this point is the picture's top left,
// as the screen's scroll shows it. A plane that scrolls is written a column (or row) at a
// time round and round its table, so the newest meets the oldest somewhere; turned this way,
// that break is at the picture's edges instead of across what the screen shows, and an
// upscaler doesn't run the two together.
template <typename Shown>
bool draw_table(const StageState& s, int base, int w, int h, Canvas& c, Shown shown, int rx, int ry)
{
    c.init(w * 8, h * 8);
    bool any = false;
    auto wrap = [](int v, int n) { return ((v % n) + n) % n; };
    for (int cy = 0; cy < h; cy += 2)
        for (int cx = 0; cx < w; cx += 2) {
            if (!shown(cx, cy))
                continue;
            const int px = wrap(cx * 8 - rx, w * 8), py = wrap(cy * 8 - ry, h * 8);
            const size_t before = c.cells.size();
            for (int q = 0; q < 4; q++) {
                const int x = cx + (q & 1), y = cy + (q >> 1);
                const int e = be16(s.vram, (uint32_t)(base + (y * w + x) * 2) & 0xFFFF);
                c.tile(&s.vram[(e & 0x7FF) * 32], px + (q & 1) * 8, py + (q >> 1) * 8, e & 0x800, e & 0x1000,
                       &s.colours[((e >> 13) & 3) * 16], false);
            }
            if (c.cells.size() > before) {
                c.blocks.push_back({px, py});
                any = true;
            }
        }
    return any;
}

// The sprites on screen (the sprite table, front first), as one picture with a piece line each.
// `key`: the pieces' tiles, wherever they sit (a sprite set moving is the same set).
bool draw_sprites(const StageState& s, Canvas& c, uint64_t& key)
{
    const uint8_t* reg = &s.cram[128];
    const bool h40 = reg[12] & 1;
    const int table = (reg[5] & (h40 ? 0x7E : 0x7F)) << 9, width = h40 ? 320 : 256;
    Frame f;
    std::vector<uint64_t> keys;
    for (int n = 0, link = 0; n < 80; n++) {
        const uint32_t a = (uint32_t)(table + link * 8) & 0xFFFF;
        const int y = (be16(s.vram, a) & 0x3FF) - 128, size = s.vram[a + 2], x = (be16(s.vram, a + 6) & 0x1FF) - 128;
        const Piece p{x, y, ((size >> 2) & 3) + 1, (size & 3) + 1, be16(s.vram, a + 4)};
        if (x > -32 && x < width && y > -32 && y < 224 && x != -128) {
            f.pieces.push_back(p);
            std::vector<uint64_t> pattern(p.w * p.h);
            for (int k = 0; k < p.w * p.h; k++)
                pattern[k] = art::pattern_hash(&s.vram[((p.tile + k) & 0x7FF) * 32]);
            keys.push_back(art::piece_key(p.w, p.h, pattern.data()) ^ (uint64_t)(p.tile & 0x1800) << 40);
        }
        link = s.vram[a + 3] & 0x7F;
        if (link == 0)
            break;
    }
    if (f.pieces.empty())
        return false;
    std::sort(keys.begin(), keys.end());
    key = 1469598103934665603ull;
    for (uint64_t k : keys)
        key = (key ^ k) * 1099511628211ull;
    int left = 1 << 20, top = 1 << 20, right = -(1 << 20), bottom = -(1 << 20);
    for (const Piece& p : f.pieces) {
        left = std::min(left, p.x), top = std::min(top, p.y);
        right = std::max(right, p.x + p.w * 8), bottom = std::max(bottom, p.y + p.h * 8);
    }
    c.init(right - left, bottom - top);
    draw_frame(f, s.vram, s.colours, left, top, c);
    return !c.cells.empty();
}

// A picture's layout (its tiles and where they sit), colours aside: the same screen mid-fade
// is the same picture.
uint64_t layout_hash(const Canvas& c)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix((uint64_t)c.w << 32 | (uint32_t)c.h);
    for (const Canvas::Cell& cell : c.cells)
        mix(cell.hash ^ ((uint64_t)cell.x << 40) ^ ((uint64_t)cell.y << 20) ^ (uint64_t)cell.flags);
    return h;
}

// How bright a picture's colours are: of several copies, the brightest (not mid-fade) is kept.
// (The most distinct colours shown first: a picture caught while its colours run together, as
// the Sega logo's colour sweep does, gives an upscaler no edges between them. Then the
// brightest.)
int brightness(const Canvas& c)
{
    int sum = 0;
    for (const auto& p : c.palettes)
        for (uint32_t v : p)
            sum += (v >> 16 & 255) + (v >> 8 & 255) + (v & 255);
    std::set<uint32_t> shown;
    for (uint32_t v : c.px)
        if (v)
            shown.insert(v);
    return (int)shown.size() * 1000000 + std::min(sum, 999999);
}

struct ScreenRun {
    std::string name;
    std::vector<std::string> args;
    int frames;
};

std::vector<ScreenRun> screen_runs()
{
    // Boot -> (Start) the title -> (Start) its menu, as tools/coverage_sweep.py did.
    const std::vector<std::string> new_game = {"--press", "start@700-705", "--press", "start@900-905"};
    std::vector<ScreenRun> runs;
    runs.push_back({"boot", {}, 6000});                 // the Sega logo, the story, the title
    std::vector<std::string> opt = new_game;            // the options, every row's settings
    int f = 1000;
    auto press = [&](const char* button, int n, int gap = 50) {
        for (int i = 0; i < n; i++, f += gap)
            opt.insert(opt.end(), {"--press", std::string(button) + "@" + std::to_string(f) + "-" + std::to_string(f + 3)});
    };
    press("c", 3);
    press("down", 1);
    press("c", 25, 40);
    press("b", 2);
    press("down", 1);
    press("c", 3);
    press("down", 1);
    press("c", 2);
    press("up", 3);
    runs.push_back({"options", opt, f + 200});
    std::vector<std::string> story = new_game;          // the opening story, the hallway, stage 1's HUD
    story.insert(story.end(), {"--press", "start@1300-1305"});
    runs.push_back({"new_game", story, 7500});
    runs.push_back({"ending", {"--go-to-stage", "200:28", "--cheat", "invincibility", "--poke", "600:FFD402=000F",
                               "--poke", "600:FFF420=0001"}, 16000});
    const std::vector<std::string> die = {"--go-to-stage", "200:1", "--poke", "400:FFF322=0001", "--poke",
                                          "400:FFF324=0001", "--press", "right@400-1500"};
    runs.push_back({"game_over", die, 5000});
    std::vector<std::string> cont = die;
    cont.insert(cont.end(), {"--press", "start@2200-2205", "--press", "start@2400-2405"});
    runs.push_back({"continue", cont, 4000});
    for (int stage : {5, 9, 13, 23, 27})               // a boss's gem: the score screen, the scene after
        runs.push_back({"gem" + std::to_string(stage),
                        {"--go-to-stage", "200:" + std::to_string(stage), "--cheat", "invincibility", "--poke", "800:FFF44E=0001"},
                        6500});
    runs.push_back({"exit1", {"--go-to-stage", "200:1", "--cheat", "invincibility", "--poke", "800:FFF412=0100"}, 3500});
    runs.push_back({"practice", {"--go-to-stage", "200:12", "--poke", "150:FFFF0E=0002", "--cheat", "invincibility",
                                 "--poke", "800:FFF412=0100"}, 7500});
    runs.push_back({"pause", {"--go-to-stage", "200:2", "--press", "start@800-803"}, 1000});
    return runs;
}

constexpr int kScreenEvery = 7;                         // frames between looks (odd: animation steps aren't always caught alike)

struct ScreenArt {
    std::string scene, kind;                            // folder, planeA / planeB / window / sprites
    Canvas picture;
    int bright = 0;
    int order = 0;                                      // run, then frame: first seen
};

// The HUD: the first picture of it, and every other square it shows (digits, power pips),
// each once.
// Of several copies, the one in the colours seen most often (not mid-fade or flashing) is kept;
// squares are told apart by their tiles, colours aside.
struct Hud {
    struct Version {
        int count = 0;
        Canvas picture;
        std::set<std::string> squares;
    };
    std::map<uint64_t, Version> pictures;                     // by colours
    std::map<std::string, std::map<uint64_t, std::pair<int, Block>>> squares;  // tiles -> colours -> count, square
    std::vector<std::string> order;                           // squares, first seen first
} g_hud;

uint64_t colours_hash(const uint32_t* c, int n)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++)
        h = (h ^ c[i]) * 1099511628211ull;
    return h;
}

template <typename Shown>
void add_hud(const StageState& s, int base, int w, Canvas& win, Shown shown, int)
{
    Hud::Version& v = g_hud.pictures[colours_hash(s.colours, 64)];
    const bool keep = v.count++ == 0;
    if (keep)
        v.picture = std::move(win);
    for (int cy = 0; cy < 32; cy += 2)
        for (int cx = 0; cx < w; cx += 2) {
            if (!shown(cx, cy))
                continue;
            Block b;
            bool any = false;
            for (int q = 0; q < 4; q++) {
                const int x = cx + (q & 1), y = cy + (q >> 1);
                b.words[q] = be16(s.vram, (uint32_t)(base + (y * w + x) * 2) & 0xFFFF);
                std::memcpy(b.tiles[q], &s.vram[(b.words[q] & 0x7FF) * 32], 32);
                std::memcpy(b.colours[q], &s.colours[((b.words[q] >> 13) & 3) * 16], 64);
                for (int i = 0; i < 32; i++)
                    any |= b.tiles[q][i] != 0;
            }
            if (!any)
                continue;
            std::string k;
            for (int q = 0; q < 4; q++) {
                const uint16_t flags = b.words[q] & 0x1800;
                k.append((const char*)&flags, 2);
                k.append((const char*)b.tiles[q], 32);
            }
            if (keep)
                v.squares.insert(k);
            if (!g_hud.squares.count(k))
                g_hud.order.push_back(k);
            auto& copy = g_hud.squares[k][colours_hash(&b.colours[0][0], 64)];
            if (copy.first++ == 0)
                copy.second = b;
        }
}

// The 16 x 16 squares a table's picture has (its block lines): their four tiles and flips.
// A cell in a palette line without colours yet (all 15 the same): the game hasn't loaded them
// (the ending's castle and credits' text show so first). Such squares and pieces don't count as
// dumped, so a later look in colour adds them.
bool blank_cell(const Canvas& c, const Canvas::Cell& cell)
{
    if (cell.palette < 0 || cell.palette >= (int)c.palettes.size() || c.palettes[cell.palette].size() < 16)
        return false;
    const std::vector<uint32_t>& p = c.palettes[cell.palette];
    for (int i = 2; i < 16; i++)
        if (p[i] != p[1])
            return false;
    return true;
}

std::vector<uint64_t> square_keys(const Canvas& c)
{
    std::map<std::pair<int, int>, const Canvas::Cell*> at;
    for (const Canvas::Cell& cell : c.cells)
        at[{cell.x, cell.y}] = &cell;
    std::vector<uint64_t> out;
    for (const auto& [bx, by] : c.blocks) {
        uint64_t pattern[4];
        int flips[4];
        bool blank = false;
        for (int q = 0; q < 4; q++) {
            auto it = at.find({bx + (q & 1) * 8, by + (q >> 1) * 8});
            pattern[q] = it == at.end() ? art::empty_pattern() : it->second->hash;
            flips[q] = it == at.end() ? 0 : it->second->flags & 3;
            blank |= it != at.end() && blank_cell(c, *it->second);
        }
        if (!blank)
            out.push_back(art::block_key(pattern, flips));
    }
    return out;
}

std::map<std::string, std::set<uint64_t>> g_squares;   // per scene and kind: the squares (or pieces) dumped

// A sprite picture's pieces (their tiles, flips aside).
std::vector<uint64_t> piece_keys(const Canvas& c)
{
    std::vector<uint64_t> out;
    for (const Canvas::PieceAt& p : c.pieces) {
        uint64_t pattern[16];
        bool blank = false;
        for (size_t k = 0; k < p.cells.size() && k < 16; k++) {
            pattern[k] = p.cells[k] < 0 ? art::empty_pattern() : c.cells[p.cells[k]].hash;
            blank |= p.cells[k] >= 0 && blank_cell(c, c.cells[p.cells[k]]);
        }
        if (!blank)
            out.push_back(art::piece_key(p.w, p.h, pattern));
    }
    return out;
}

// Runs one screen run and adds its distinct pictures to `found` (by layout), and the objects'
// sprite sets it shows outside level play to `sets` (every frame of each animation played, from
// the mapping tables, as for the stages), with the scene each was seen in.
bool dump_screens(const fs::path& engine, const fs::path& rom_path, const std::vector<uint8_t>& rom, const fs::path& work,
                  const ScreenRun& run, int run_index, std::map<uint64_t, ScreenArt>& found,
                  std::vector<std::pair<std::string, SpriteSet>>& sets, std::mutex& lock)
{
    const fs::path dir = work / ("screens_" + run.name);
    fs::remove_all(dir);
    fs::create_directories(dir / "sprites");
    // (Widescreen: scenes it widens, such as the theatre, repeat their own columns, which makes
    // squares 4:3 never shows; the middle 320 pixels are the same either way.)
    std::vector<std::string> args = {rom_path.string(), "--no-mdplus", "--no-gpu-vdp"};
    args.insert(args.end(), run.args.begin(), run.args.end());
    args.insert(args.end(), {"--frames", std::to_string(run.frames), "--vdp-every", std::to_string(kScreenEvery), dir.string(),
                             "--sprite-capture", (dir / "sprites").string()});
    if (!run_engine(engine, args))
        return false;
    for (SpriteSet& set : stage_sprites(rom, dir / "sprites", 0)) {
        char stem[32];
        std::snprintf(stem, sizeof(stem), "f%06d", set.frame / kScreenEvery * kScreenEvery);
        StageState s;
        if (!s.load(dir / stem))
            continue;
        const std::string scene = scene_name(s);
        if (scene == "hud")                             // (level play: the stages have those)
            continue;
        std::lock_guard<std::mutex> g(lock);
        sets.push_back({scene, std::move(set)});
    }
    std::vector<fs::path> stems;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".ram")
            stems.push_back(e.path().parent_path() / e.path().stem());
    std::sort(stems.begin(), stems.end());
    // Each scene's palette lines as seen. A look whose line has every colour at or below one
    // seen elsewhere in the scene was caught fading (in or out) and takes the brightest such
    // line instead: dumped mid-fade, a picture's colours run together and an upscaler leaves
    // it unsmoothed. ($FFF800, the fade's target, doesn't always hold them: the boot story.)
    using Line = std::array<uint16_t, 16>;
    std::map<std::pair<std::string, int>, std::set<Line>> lines_seen;
    auto line_of = [](const StageState& s, int line) {
        Line l;
        for (int i = 0; i < 16; i++)
            l[i] = be16(s.cram, (line * 16 + i) * 2);
        return l;
    };
    for (const fs::path& stem : stems) {
        StageState s;
        if (s.load(stem))
            for (int line = 0; line < 4; line++)
                lines_seen[{scene_name(s), line}].insert(line_of(s, line));
    }
    // `a` a fade of `b`: every channel brought down by the same step (max(0, b - k)) or up to
    // the same level (min(b, k)), as the game's fades step. (Merely darker isn't enough: other
    // pictures of the scene use the same line with other colours.)
    auto at_or_below = [](const Line& a, const Line& b) {
        for (int k = 0; k <= 7; k++) {
            bool down = true, up = true;
            for (int i = 1; i < 16 && (down || up); i++)
                for (int shift = 1; shift <= 9; shift += 4) {
                    const int av = (a[i] >> shift) & 7, bv = (b[i] >> shift) & 7;
                    down &= av == std::max(0, bv - k);
                    up &= av == std::min(bv, k);
                }
            if (down || up)
                return true;
        }
        return false;
    };
    auto sum = [](const Line& l) {
        int n = 0;
        for (int i = 1; i < 16; i++)
            n += ((l[i] >> 1) & 7) + ((l[i] >> 5) & 7) + ((l[i] >> 9) & 7);
        return n;
    };
    // A picture counts once it stays the same for two looks in a row (not mid-change).
    std::map<std::string, uint64_t> last;
    int look = 0;
    for (const fs::path& stem : stems) {
        const int order = run_index * 100000 + look++;
        StageState s;
        if (!s.load(stem))
            continue;
        const std::string scene = scene_name(s);
        for (int line = 0; line < 4; line++) {
            const Line own = line_of(s, line);
            const Line* best = nullptr;
            for (const Line& l : lines_seen[{scene, line}])
                if (at_or_below(own, l) && sum(l) > (best ? sum(*best) : sum(own)))
                    best = &l;
            if (best)
                for (int i = 0; i < 16; i++)
                    s.colours[line * 16 + i] = argb((*best)[i]);
        }
        const uint8_t* reg = &s.cram[128];
        const bool h40 = reg[12] & 1;
        auto size = [](int v) { return v == 1 ? 64 : v == 3 ? 128 : 32; };
        const int w = size(reg[16] & 3), h = size((reg[16] >> 4) & 3);
        std::vector<std::pair<std::string, Canvas>> pictures;
        uint64_t sprite_key = 0;
        auto all = [](int, int) { return true; };
        // The pause run, once paused (Start at 800): its sprites (the "PAUSE" text) as a scene
        // of their own; the rest is level play, which the stages have.
        const bool paused = run.name == "pause" && scene == "hud" && std::atoi(stem.filename().string().c_str() + 1) >= 820;
        if (paused) {
            Canvas sp;
            if (draw_sprites(s, sp, sprite_key))
                pictures.push_back({"sprites", std::move(sp)});
        }
        const std::string scene_or_pause = paused ? std::string("pause") : scene;
        if (scene != "hud") {
            Canvas a, b, sp;
            // Where the screen's top left is on each plane (the first line's scroll), to 16 pixels.
            const int hs = (reg[13] & 0x3F) << 10;
            auto left = [&](int plane) { return (-(int16_t)be16(s.vram, (uint32_t)(hs + plane * 2) & 0xFFFF)) & (w * 8 - 1) & ~15; };
            auto top = [&](int plane) {
                return (s.cram.size() >= 164 ? (int)(s.cram[160 + plane * 2] << 8 | s.cram[161 + plane * 2]) : 0) & (h * 8 - 1) & ~15;
            };
            if (w * h <= 4096 && draw_table(s, (reg[2] & 0x38) << 10, w, h, a, all, left(0), top(0)))
                pictures.push_back({"planeA", std::move(a)});
            if (w * h <= 4096 && draw_table(s, (reg[4] & 7) << 13, w, h, b, all, left(1), top(1)))
                pictures.push_back({"planeB", std::move(b)});
            if (draw_sprites(s, sp, sprite_key))
                pictures.push_back({"sprites", std::move(sp)});
        }
        // The window, where the screen shows it: rows from register 18 (down from or above a row),
        // columns from register 17 (right of or left of a column, in 2-cell steps).
        const int wrow = reg[18] & 0x1F, wcol = (reg[17] & 0x1F) * 2;
        auto shown = [&](int cx, int cy) {
            const bool rows = (reg[18] & 0x80) ? cy + 1 >= wrow && cy < 28 : cy < wrow;
            const bool cols = (reg[17] & 0x80) ? cx + 1 >= wcol && cx < (h40 ? 40 : 32) : cx < wcol;
            return rows || (cols && cy < 28);
        };
        if ((reg[17] || reg[18]) && scene != "ending") {      // (the ending's window: the HUD, fading to white)
            Canvas win;
            if (draw_table(s, (reg[3] & (h40 ? 0x3C : 0x3E)) << 10, h40 ? 64 : 32, 32, win, shown)) {
                if (scene == "hud") {
                    // The HUD: one picture, and its squares that change (digits, the power
                    // pips) on a sheet.
                    std::lock_guard<std::mutex> g(lock);
                    add_hud(s, (reg[3] & (h40 ? 0x3C : 0x3E)) << 10, h40 ? 64 : 32, win, shown, order);
                } else {
                    pictures.push_back({"window", std::move(win)});
                }
            }
        }
        for (auto& [kind, c] : pictures) {
            const uint64_t layout = kind == "sprites" ? sprite_key : layout_hash(c);
            const std::string slot = scene_or_pause + "/" + kind;
            // Sprites count once they stay the same for two looks (not mid-change). Planes and
            // the window count when they show a 16 x 16 square the scene's pictures don't have
            // yet: a scene that scrolls (the boot story's) is never still, and its squares
            // come in a strip at a time.
            // (Sprites too now: those that move all the time, as credits' letters do, were
            // never still; a look counts when it shows a piece the scene's pictures don't have.)
            last[slot] = layout;
            const int bright = brightness(c);
            std::lock_guard<std::mutex> g(lock);
            const uint64_t key = layout ^ std::hash<std::string>()(scene_or_pause + kind);
            auto it = found.find(key);
            if (it != found.end()) {
                if (bright > it->second.bright)
                    it->second.picture = std::move(c), it->second.bright = bright;
                continue;
            }
            {
                std::set<uint64_t>& have = g_squares[slot];
                bool adds = false;
                const std::vector<uint64_t> parts = kind == "sprites" ? piece_keys(c) : square_keys(c);
                for (uint64_t k : parts)
                    adds |= !have.count(k);
                if (!adds)
                    continue;
                have.insert(parts.begin(), parts.end());
            }
            found[key] = {scene_or_pause, kind, std::move(c), bright, order};
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    bool wait = true, keep_work = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-wait") == 0)
            wait = false;
        if (std::strcmp(argv[i], "--keep-work") == 0)   // (coi.exe's files, for checking)
            keep_work = true;
    }
    const char* base_c = SDL_GetBasePath();
    const fs::path base = base_c ? fs::path(base_c) : fs::current_path();
    auto finish = [&](int code) {
        if (wait) {
            std::printf("\nPress Enter to close.\n");
            std::getchar();
        }
        return code;
    };
    fs::path engine;
    for (const fs::path& p : {base / ".." / "coi.exe", base / "coi.exe", base / ".." / "coi", base / "coi"})
        if (fs::exists(p) && fs::is_regular_file(p)) {
            engine = fs::absolute(p);
            break;
        }
    const fs::path rom_path = find_rom(base);
    std::vector<uint8_t> rom;
    if (engine.empty() || rom_path.empty() || !read_file(rom_path, rom)) {
        std::printf("coi-art-dump: needs coi.exe in the folder above this one, and the game's ROM in its rom/\n"
                    "folder (%s%s).\n", engine.empty() ? "coi.exe not found" : "", rom_path.empty() ? " ROM not found" : "");
        return finish(1);
    }
    const fs::path out = base / "art-dump", work = base / "art-dump-work";
    std::printf("Dumping the game's art from %s\ninto %s\n", rom_path.string().c_str(), out.string().c_str());
    fs::remove_all(out);
    fs::create_directories(work);

    std::vector<StageArt> stages(kStages);
    std::atomic<int> next{0}, finished{0};
    std::mutex print;
    const int workers = std::clamp(SDL_GetNumLogicalCPUCores() - 1, 1, 8);
    std::vector<std::thread> threads;
    for (int t = 0; t < workers; t++)
        threads.emplace_back([&] {
            for (int i; (i = next++) < kStages;) {
                stages[i] = dump_stage(engine, rom_path, rom, work, i + 1);
                std::lock_guard<std::mutex> lock(print);
                std::printf("  stage %2d %s (%d of %d)\n", i + 1, stages[i].ok ? "done" : "FAILED", ++finished, kStages);
                std::fflush(stdout);
            }
        });
    for (std::thread& t : threads)
        t.join();

    // The screens: the runs through the boot, the menus, the story scenes and the ending.
    const std::vector<ScreenRun> runs = screen_runs();
    std::map<uint64_t, ScreenArt> screens;
    std::vector<std::pair<std::string, SpriteSet>> screen_sets;
    std::mutex screens_lock;
    std::atomic<int> next_run{0}, runs_done{0}, runs_failed{0};
    threads.clear();
    for (int t = 0; t < workers; t++)
        threads.emplace_back([&] {
            for (int i; (i = next_run++) < (int)runs.size();) {
                const bool ok = dump_screens(engine, rom_path, rom, work, runs[i], i, screens, screen_sets, screens_lock);
                runs_failed += !ok;
                std::lock_guard<std::mutex> lock(print);
                std::printf("  screens: %s %s (%d of %d)\n", runs[i].name.c_str(), ok ? "done" : "FAILED", ++runs_done,
                            (int)runs.size());
                std::fflush(stdout);
            }
        });
    for (std::thread& t : threads)
        t.join();

    // What two or more stages use goes to shared/.
    std::map<std::string, std::set<int>> block_stages;
    std::map<uint64_t, std::set<int>> background_stages, sprite_stages;
    for (int i = 0; i < kStages; i++) {
        for (const Block& b : stages[i].blocks)
            block_stages[b.key()].insert(i);
        if (stages[i].background.w)
            background_stages[stages[i].background.content_hash()].insert(i);
        for (const SpriteSet& s : stages[i].sprites)
            sprite_stages[s.key].insert(i);
    }
    auto sheet = [](const std::vector<const Block*>& blocks, const fs::path& path) {
        if (blocks.empty())
            return;
        // 8 clear pixels between blocks, so each block's edges are plain to see and an upscaler
        // can't run one block into the next.
        constexpr int kAcross = 16, kPitch = 24;
        Canvas c;
        c.init(kAcross * kPitch - 8, (int)((blocks.size() + kAcross - 1) / kAcross) * kPitch - 8);
        for (size_t i = 0; i < blocks.size(); i++) {
            const int x = (int)(i % kAcross) * kPitch, y = (int)(i / kAcross) * kPitch;
            blocks[i]->draw(c, x, y);
            c.blocks.push_back({x, y});
        }
        c.save(path);
    };
    std::vector<const Block*> shared_blocks;
    std::set<std::string> shared_done;
    std::set<uint64_t> backgrounds_done, sprites_done;
    int pictures = 0;
    for (int i = 0; i < kStages; i++) {
        char tag[16];
        std::snprintf(tag, sizeof(tag), "stage%02d", i + 1);
        const StageArt& st = stages[i];
        std::vector<const Block*> own;
        for (const Block& b : st.blocks) {
            const std::string k = b.key();
            if (block_stages[k].size() > 1) {
                if (shared_done.insert(k).second)
                    shared_blocks.push_back(&b);
            } else {
                own.push_back(&b);
            }
        }
        sheet(own, out / tag / "blocks.png");
        pictures += !own.empty();
        if (st.background.w) {
            const uint64_t k = st.background.content_hash();
            if (background_stages[k].size() > 1) {
                if (backgrounds_done.insert(k).second) {
                    std::string name = "background_stages";
                    for (int s : background_stages[k])
                        name += "_" + std::to_string(s + 1);
                    st.background.save(out / "shared" / "backgrounds" / (name + ".png"));
                    pictures++;
                }
            } else {
                st.background.save(out / tag / "background.png");
                pictures++;
            }
        }
        for (const SpriteSet& s : st.sprites) {
            const bool shared = sprite_stages[s.key].size() > 1;
            if (shared && !sprites_done.insert(s.key).second)
                continue;
            fs::path folder = shared ? out / "shared" / "sprites" / s.name : out / tag / "sprites" / s.name;
            if (shared && fs::exists(folder)) {         // the same name, other graphics: its stages too
                std::string name = s.name;
                for (int n : sprite_stages[s.key])
                    name += "_" + std::to_string(n + 1);
                folder = out / "shared" / "sprites" / name;
            }
            for (const auto& [file, c] : s.frames) {
                c.save(folder / file);
                pictures++;
            }
        }
        for (size_t k = 0; k < st.others.size(); k++) {
            char file[32];
            std::snprintf(file, sizeof(file), "other_%02zu.png", k + 1);
            st.others[k].save(out / tag / "sprites" / "Other" / file);
            pictures++;
        }
        for (size_t k = 0; k < st.animated.size(); k++) {   // animated tiles' other frames, among their neighbours
            char file[32];
            std::snprintf(file, sizeof(file), "animated_%02zu.png", k + 1);
            st.animated[k].save(out / tag / "animated" / file);
            pictures++;
        }
    }
    sheet(shared_blocks, out / "shared" / "blocks.png");
    // Screens, numbered per scene and kind in the order first seen.
    std::vector<const ScreenArt*> ordered;
    for (const auto& [key, art] : screens)
        ordered.push_back(&art);
    std::sort(ordered.begin(), ordered.end(), [](const ScreenArt* a, const ScreenArt* b) { return a->order < b->order; });
    std::map<std::string, int> numbers;
    for (const ScreenArt* art : ordered) {
        char file[48];
        std::snprintf(file, sizeof(file), "%s_%02d.png", art->kind.c_str(), ++numbers[art->scene + "/" + art->kind]);
        art->picture.save(out / "screens" / art->scene / file);
        pictures++;
    }
    // The screens' objects: each set once, unless a stage has it already.
    std::sort(screen_sets.begin(), screen_sets.end(),
              [](const auto& a, const auto& b) { return a.second.frame < b.second.frame; });
    for (const auto& [scene, set] : screen_sets) {
        if (sprite_stages.count(set.key) || !sprites_done.insert(set.key).second)
            continue;
        fs::path folder = out / "screens" / scene / "sprites" / set.name;
        for (int n = 2; fs::exists(folder); n++)
            folder = out / "screens" / scene / "sprites" / (set.name + "_" + std::to_string(n));
        for (const auto& [file, c] : set.frames) {
            c.save(folder / file);
            pictures++;
        }
    }
    if (!g_hud.pictures.empty()) {
        const Hud::Version* best = nullptr;
        for (const auto& [colours, v] : g_hud.pictures)
            if (!best || v.count > best->count)
                best = &v;
        best->picture.save(out / "screens" / "hud" / "window.png");
        std::vector<const Block*> squares;
        for (const std::string& k : g_hud.order) {
            if (best->squares.count(k))
                continue;
            const std::pair<int, Block>* most = nullptr;
            for (const auto& [colours, copy] : g_hud.squares[k])
                if (!most || copy.first > most->first)
                    most = &copy;
            squares.push_back(&most->second);
        }
        sheet(squares, out / "screens" / "hud" / "squares.png");
        pictures += 1 + !squares.empty();
    }
    write_readme(out);
    int failed = runs_failed;
    for (const StageArt& s : stages)
        failed += !s.ok;
    std::error_code e;
    if (!keep_work)
        fs::remove_all(work, e);
    std::printf("Done: %d pictures in %s%s\n", pictures + 1, out.string().c_str(),
                failed ? " (some stages failed: see above)" : "");
    return finish(failed ? 1 : 0);
}
