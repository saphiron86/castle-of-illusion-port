// Art packs: replacement art for the game's 8x8 tiles, drawn by the GPU renderer.
//
// A tile is known by its pattern: a hash of its 32 bytes of pixels (TilePattern::hash), the
// same whichever VRAM slot, palette or flip it is drawn with. The tile dump (--dump-tiles DIR)
// saves each pattern the game shows, in the colours it showed it with, as DIR/<hash>.png
// (8 x 8, colour 0 see-through) and lists those colours in DIR/palettes.txt. Nothing of the
// game's art is in the port: a dump comes from the player's own ROM.
//
// A pack is such a folder with the pictures redrawn larger: <hash>.png at 16 x 16 (2x) or
// 32 x 32 (4x), all at one size, see-through where the tile should show what lies behind.
// Pictures still 8 x 8 are left out. The GPU renderer draws the picture at 4x4 texels a game
// pixel with a pack, and takes a replaced tile's texels from its picture, scaled per channel
// by the colour its own pixel has now against the colour it had in the dump (palettes.txt),
// so fades, flashes and colour changes still show.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Vdp;

namespace art {

// FNV-1a over a tile's 32 bytes (the tile dump, the art dump tool and the GPU renderer).
inline uint64_t pattern_hash(const uint8_t* tile)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 32; i++) {
        h ^= tile[i];
        h *= 1099511628211ull;
    }
    return h;
}

inline std::string hash_name(uint64_t hash)  // 16 hex digits
{
    char name[17];
    std::snprintf(name, sizeof(name), "%016llx", (unsigned long long)hash);
    return name;
}

// A plane A block (2 x 2 tiles, as the art dump's blocks.png shows them): its four tiles'
// patterns and flips (bit 0 across, 1 up and down), top left, top right, bottom left, bottom
// right. A blank tile counts as the empty pattern unflipped.
inline uint64_t block_key(const uint64_t pattern[4], const int flips[4])
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 4; i++) {
        for (int b = 0; b < 8; b++) {
            h ^= (pattern[i] >> (b * 8)) & 255;
            h *= 1099511628211ull;
        }
        h ^= (uint64_t)(flips[i] & 3);
        h *= 1099511628211ull;
    }
    return h;
}

// A block with the blocks round it (left, right, above, below; 0 where a picture has none):
// the same square drawn beside other squares can be drawn to meet each of them.
inline uint64_t context_key(uint64_t square, const uint64_t around[4])
{
    uint64_t k = square * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 4; i++)
        k = (k ^ (around[i] + 0x632BE59BD9B4E019ull * (i + 1))) * 1099511628211ull;
    return k;
}

// The colours a block was drawn in: its four tiles' palette lines (16 colours each, as the
// art dump writes them, 0xAARRGGBB; 0 for a blank tile). Mixed into a key, it finds the
// block's picture dumped in the colours the game shows now (the same mountains by day in the
// boot story and by night after the ending).
inline uint64_t colours_key(const uint32_t* const lines[4])
{
    uint64_t h = 1469598103934665603ull;
    for (int q = 0; q < 4; q++)
        for (int i = 0; i < 16; i++) {
            h ^= lines[q] ? lines[q][i] : 0;
            h *= 1099511628211ull;
        }
    return h;
}

inline uint64_t with_colours(uint64_t key, uint64_t colours) { return (key ^ 0x5851F42D4C957F2Dull) * 6364136223846793005ull ^ colours; }

// A sprite piece (one hardware sprite, 1-4 x 1-4 tiles): its size and its tiles' patterns in
// VRAM order (down each column), flips aside.
inline uint64_t piece_key(int w, int h, const uint64_t* pattern)
{
    uint64_t k = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int b = 0; b < 8; b++) {
            k ^= (v >> (b * 8)) & 255;
            k *= 1099511628211ull;
        }
    };
    mix((uint64_t)(w * 8 + h));
    for (int i = 0; i < w * h; i++)
        mix(pattern[i]);
    return k;
}

inline uint64_t empty_pattern()
{
    static const uint8_t zero[32] = {};
    static const uint64_t h = pattern_hash(zero);
    return h;
}

struct Pack {
    int scale = 0;                          // picture texels per game pixel (2 or 4)
    int slots = 0;
    // Plane A blocks: key (block_key) -> the first of four slots, one per tile (unflipped like
    // a tile's picture), so a tile repeated in several blocks can look right in each.
    std::unordered_map<uint64_t, int> block;
    // The same with the blocks round it (context_key): taken first, where the picture of the
    // block among these neighbours differs from its usual one.
    std::unordered_map<uint64_t, int> block_context;
    // Sprite pieces: key (piece_key) -> the first of its w * h slots, in VRAM order, unflipped.
    std::unordered_map<uint64_t, int> piece;
    static constexpr int kPerRow = 64;      // pictures per atlas row
    int atlas_width = 0, atlas_height = 0;
    std::vector<uint32_t> atlas;            // RGBA bytes in memory order (R first)
    // Per atlas texel, red: a texel an upscaler blended from two dumped colours: the second
    // colour (1-15) * 16 + its share (0-15, of 15); 0 for an unblended texel.
    std::vector<uint32_t> atlas_blend;
    std::vector<uint32_t> reference;        // per slot, 16 colours (0xAARRGGBB) as dumped
    std::vector<uint32_t> slot_atlas;       // per slot, its picture's place in the atlas (shared by equal pictures)
    std::unordered_map<uint64_t, int> slot; // pattern -> slot

    // Loads DIR's pictures; false (with a reason) when none could be used.
    // `progress` (if given) goes from 0 to 1 as the pictures are read (another thread may read it).
    bool load(const std::filesystem::path& dir, std::string& why, std::atomic<float>* progress = nullptr);
    bool empty() const { return slots == 0; }
};

// Saves every tile pattern the planes and sprites show, once each.
class Dumper {
public:
    explicit Dumper(std::filesystem::path dir);
    void frame(const Vdp& vdp);             // once per game frame, with the frame's VRAM and CRAM
    int saved() const { return (int)done.size(); }

private:
    void save(const Vdp& vdp, int tile, int line);
    std::filesystem::path dir;
    std::unordered_set<uint64_t> done;
    uint16_t last_cram[64] = {};
    int stable[4] = {};                     // frames each palette line has kept its colours
};

}  // namespace art
