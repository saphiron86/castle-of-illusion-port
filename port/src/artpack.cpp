// Art packs and the tile dump (see artpack.h).

#include "artpack.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "vdp.h"

namespace art {

namespace {

uint32_t argb(uint16_t c)
{
    auto scale = [](int v) { return (uint32_t)(v * 255 / 7); };
    return 0xFF000000 | scale((c >> 1) & 7) << 16 | scale((c >> 5) & 7) << 8 | scale((c >> 9) & 7);
}

bool blank(const uint8_t* tile)
{
    for (int i = 0; i < 32; i++)
        if (tile[i])
            return false;
    return true;
}

// A palette line counts as settled (worth dumping its colours) after this many frames
// unchanged: fades and flashes move it every few frames.
constexpr int kSettled = 30;

}  // namespace

namespace {

// A picture for a pattern, and how much it counts when several pictures show the same tile.
struct Candidate {
    int priority = -1000;
    std::vector<uint32_t> rgba;             // (8 * scale)^2, R, G, B, A in memory
    uint32_t reference[16] = {};
    std::string source;
};

SDL_Surface* load_rgba(const std::filesystem::path& path)
{
    SDL_Surface* loaded = SDL_LoadPNG(path.string().c_str());
    if (!loaded)
        return nullptr;
    SDL_Surface* s = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ABGR8888);   // R, G, B, A in memory
    SDL_DestroySurface(loaded);
    return s;
}

}  // namespace

bool Pack::load(const std::filesystem::path& dir, std::string& why, std::atomic<float>* progress)
{
    namespace fs = std::filesystem;
    *this = Pack();
    std::error_code error;
    if (!fs::is_directory(dir, error)) {
        why = "no folder " + dir.string();
        return false;
    }
    std::unordered_map<uint64_t, Candidate> best;
    // Plane A blocks (block sheets' "block" lines): four pictures each, top left to bottom right.
    struct BlockCandidate {
        int priority = -1000;
        Candidate quarter[4];
    };
    std::unordered_map<uint64_t, BlockCandidate> best_block, best_context, best_block_colours;
    // Sprite pieces ("piece" lines): one picture per tile, in VRAM order.
    struct PieceCandidate {
        int priority = -1000;
        std::vector<Candidate> tiles;           // (each with its own priority, as a tile)
        std::vector<uint64_t> pattern;
    };
    std::unordered_map<uint64_t, PieceCandidate> best_piece;
    int other_sizes = 0, conflicts = 0;
    // Offers one tile's picture: a higher priority replaces what is there; the same priority
    // with other pixels is a conflict (the first stays).
    // A palette line without colours yet (all 15 the same: dumped before the game loaded them,
    // as the ending's castle and credits were): its pictures come last, after any copy in colour.
    auto blank = [](const uint32_t* reference) {
        if (!reference || !(reference[1] >> 24))
            return false;
        for (int i = 2; i < 16; i++)
            if (reference[i] != reference[1])
                return false;
        return true;
    };
    constexpr int kBlank = 100;                     // (priority taken off)
    auto offer = [&](uint64_t hash, int priority, std::vector<uint32_t>&& rgba, const uint32_t* reference,
                     const std::string& source) {
        if (blank(reference))
            priority -= kBlank;
        Candidate& c = best[hash];
        if (priority < c.priority)
            return;
        if (priority == c.priority) {
            // (The same pattern in other colours is no conflict: its colours are scaled anyway.)
            const bool same_colours = reference && std::memcmp(reference, c.reference, sizeof(c.reference)) == 0;
            // (Nor are copies that overlap other tiles: they always differ.)
            if (same_colours && priority >= 0 && rgba != c.rgba && ++conflicts <= 10)
                std::printf("art pack: tile %s differs between %s and %s (the first is used)\n", hash_name(hash).c_str(),
                            c.source.c_str(), source.c_str());
            return;
        }
        c.priority = priority;
        c.rgba = std::move(rgba);
        if (reference)
            std::memcpy(c.reference, reference, sizeof(c.reference));
        else
            std::memset(c.reference, 0, sizeof(c.reference));
        c.source = source;
    };
    auto usable_scale = [&](int s) {
        if (s < 2)
            return false;
        if (scale == 0)
            scale = s;
        if (s != scale) {
            other_sizes++;
            return false;
        }
        return true;
    };

    // The tile dump's pictures (--dump-tiles): <hash>.png, one tile each, with palettes.txt.
    std::unordered_map<uint64_t, std::vector<uint32_t>> dumped;
    if (std::ifstream in{dir / "palettes.txt"}) {
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream words(line);
            std::string name;
            if (!(words >> name) || name.size() != 16)
                continue;
            std::vector<uint32_t> colours;
            std::string c;
            while (colours.size() < 16 && words >> c)
                colours.push_back((uint32_t)std::strtoul(c.c_str(), nullptr, 16));
            if (colours.size() == 16)
                dumped[std::strtoull(name.c_str(), nullptr, 16)] = colours;
        }
    }
    // (Progress: the files read of all there are, up to 0.95; the atlas after.)
    size_t files = 0, seen = 0;
    if (progress) {
        for (fs::recursive_directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
            files++;
        error.clear();
        progress->store(0.0f);
    }
    for (fs::recursive_directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        const fs::path& path = it->path();
        if (progress && files && (++seen & 15) == 0)
            progress->store(0.95f * (float)seen / (float)files);
        const std::string stem = path.stem().string();
        if (path.extension() == ".png" && stem.size() == 16 && stem.find_first_not_of("0123456789abcdef") == std::string::npos) {
            SDL_Surface* s = load_rgba(path);
            if (!s)
                continue;
            const int w = s->w;
            if (w == s->h && w % 8 == 0 && w > 8 && usable_scale(w / 8)) {
                std::vector<uint32_t> rgba((size_t)w * w);
                for (int y = 0; y < w; y++)
                    std::memcpy(&rgba[(size_t)y * w], (const uint8_t*)s->pixels + (size_t)y * s->pitch, (size_t)w * 4);
                const uint64_t hash = std::strtoull(stem.c_str(), nullptr, 16);
                auto found = dumped.find(hash);
                offer(hash, 30, std::move(rgba), found != dumped.end() ? found->second.data() : nullptr, path.string());
            }
            SDL_DestroySurface(s);
            continue;
        }
        // The art dump's pictures (coi-art-dump): a picture with a .tiles file beside it.
        if (path.extension() != ".tiles")
            continue;
        std::ifstream in(path);
        std::string word;
        int version = 0, size_w = 0, size_h = 0;
        if (!(in >> word >> version) || word != "art-tiles" || !(in >> word >> size_w >> size_h) || word != "size"
            || size_w <= 0 || size_h <= 0)
            continue;
        fs::path png = path;
        png.replace_extension(".png");
        SDL_Surface* s = load_rgba(png);
        if (!s)
            continue;
        const int sc = s->w / size_w;
        if (s->w != sc * size_w || s->h != sc * size_h || !usable_scale(sc)) {
            if (s->w != size_w)
                other_sizes++;
            SDL_DestroySurface(s);
            continue;
        }
        // Most specific first: a stage's own pictures over shared/; sprites over blocks over
        // backgrounds; a tile partly hidden or overlapping another in a sprite only when
        // nothing else shows it.
        const fs::path rel = fs::relative(path, dir, error);
        bool shared = false, sprite = false, screen = false;
        for (const fs::path& part : rel) {
            shared |= part == "shared";
            sprite |= part == "sprites";
            screen |= part == "screens";
        }
        const int kind = sprite ? 3 : stem == "blocks" ? 2 : 1;
        // (screens/: the menus, story and HUD, under all stage art; their objects' animations
        // over the screens' own sprite pictures)
        const int priority = screen ? (sprite ? 1 : 0) : kind + (shared ? 0 : 10);
        std::vector<std::vector<uint32_t>> palettes;
        // A tile's square of the picture at (x, y), turned back to the tile's own way.
        auto cut = [&](int x, int y, int flags) {
            const int n = 8 * sc;
            std::vector<uint32_t> rgba((size_t)n * n);
            for (int ty = 0; ty < n; ty++)
                for (int tx = 0; tx < n; tx++) {
                    const int px = (flags & 1) ? n - 1 - tx : tx, py = (flags & 2) ? n - 1 - ty : ty;
                    rgba[(size_t)ty * n + tx] = ((const uint32_t*)((const uint8_t*)s->pixels + (size_t)(y * sc + py) * s->pitch))[x * sc + px];
                }
            return rgba;
        };
        struct Placed {
            uint64_t hash;
            int flags, palette;
        };
        std::vector<std::pair<int, int>> block_at;
        std::map<std::pair<int, int>, Placed> placed;   // whole tiles by position (block sheets)
        struct CellLine {
            int x, y, flags, palette;
            uint64_t hash, own;
        };
        std::vector<CellLine> cell_lines;               // every cell line, in order (sprite pieces)
        std::vector<std::string> piece_lines;
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::istringstream words(line);
            words >> word;
            if (word == "palette") {
                int n;
                words >> n;
                std::vector<uint32_t> p;
                std::string c;
                while (p.size() < 16 && words >> c)
                    p.push_back((uint32_t)std::strtoul(c.c_str(), nullptr, 16));
                if (n >= 0 && p.size() == 16) {
                    if ((int)palettes.size() <= n)
                        palettes.resize(n + 1);
                    palettes[n] = p;
                }
            } else if (word == "block") {
                int x, y;
                if (words >> x >> y && x >= 0 && y >= 0 && x + 16 <= size_w && y + 16 <= size_h)
                    block_at.push_back({x, y});
            } else if (word == "piece") {
                piece_lines.push_back(line);
            } else if (word == "cell") {
                int x, y, flags, pal;
                std::string hash;
                cell_lines.push_back({-1, -1, 0, -1, 0, 0});
                if (!(words >> x >> y >> hash >> flags >> pal) || x < 0 || y < 0 || x + 8 > size_w || y + 8 > size_h)
                    continue;
                // A tile overlapping others (8), not hidden by them (4): only the pixels it drew
                // itself are its own; the rest of its square shows the tile behind.
                std::string own_s;
                const bool masked = (flags & 12) == 8 && (words >> own_s);
                const uint64_t own = masked ? std::strtoull(own_s.c_str(), nullptr, 16) : ~0ull;
                const int n = 8 * sc;
                std::vector<uint32_t> rgba((size_t)n * n);
                for (int ty = 0; ty < n; ty++)
                    for (int tx = 0; tx < n; tx++) {
                        // The picture shows the tile flipped as it was drawn: back to the tile's own way.
                        const int px = (flags & 1) ? n - 1 - tx : tx, py = (flags & 2) ? n - 1 - ty : ty;
                        const uint32_t t = ((const uint32_t*)((const uint8_t*)s->pixels + (size_t)(y * sc + py) * s->pitch))[x * sc + px];
                        rgba[(size_t)ty * n + tx] = own >> ((py / sc) * 8 + px / sc) & 1 ? t : 0;
                    }
                const int rank = !(flags & 12) ? priority : masked ? priority - 10 : priority - 20;
                const uint64_t h = std::strtoull(hash.c_str(), nullptr, 16);
                if (!(flags & 12))
                    placed[{x, y}] = {h, flags, pal};
                cell_lines.back() = {x, y, flags, pal, h, own};
                offer(h, rank, std::move(rgba),
                      pal >= 0 && pal < (int)palettes.size() && palettes[pal].size() == 16 ? palettes[pal].data() : nullptr,
                      rel.string());
            }
        }
        // Blocks: all four squares, blank tiles too (art may cover them), known by their tiles.
        // Each block's key first, then the blocks round each (a table's picture wraps round;
        // a block sheet's blocks have none).
        static const int kAt[4][2] = {{0, 0}, {8, 0}, {0, 8}, {8, 8}};
        std::map<std::pair<int, int>, uint64_t> key_at;
        std::vector<uint64_t> keys(block_at.size());
        std::vector<std::array<int, 4>> block_flips(block_at.size());
        for (size_t i = 0; i < block_at.size(); i++) {
            const auto [bx, by] = block_at[i];
            uint64_t pattern[4];
            int flips[4];
            for (int q = 0; q < 4; q++) {
                auto found = placed.find({bx + kAt[q][0], by + kAt[q][1]});
                const bool blank = found == placed.end();
                pattern[q] = blank ? empty_pattern() : found->second.hash;
                flips[q] = blank ? 0 : found->second.flags & 3;
                block_flips[i][q] = flips[q];
            }
            keys[i] = key_at[{bx, by}] = block_key(pattern, flips);
        }
        auto key_near = [&](int x, int y) -> uint64_t {
            auto it = key_at.find({(x % size_w + size_w) % size_w, (y % size_h + size_h) % size_h});
            return it == key_at.end() ? 0 : it->second;
        };
        for (size_t i = 0; i < block_at.size(); i++) {
            const auto [bx, by] = block_at[i];
            BlockCandidate b;
            b.priority = priority;
            for (int q = 0; q < 4; q++) {
                auto found = placed.find({bx + kAt[q][0], by + kAt[q][1]});
                Candidate& c = b.quarter[q];
                c.rgba = cut(bx + kAt[q][0], by + kAt[q][1], block_flips[i][q]);
                const int pal = found == placed.end() ? -1 : found->second.palette;
                if (pal >= 0 && pal < (int)palettes.size() && palettes[pal].size() == 16)
                    std::memcpy(c.reference, palettes[pal].data(), sizeof(c.reference));
            }
            for (int q = 0; q < 4; q++)
                if (blank(b.quarter[q].reference)) {
                    b.priority -= kBlank;
                    break;
                }
            // Among these neighbours, kept only where it differs from the block's usual
            // picture (an upscaler ran this one into other neighbours).
            const uint64_t around[4] = {key_near(bx - 16, by), key_near(bx + 16, by), key_near(bx, by - 16),
                                        key_near(bx, by + 16)};
            BlockCandidate& have = best_block[keys[i]];
            const bool differs = have.priority > -1000 && [&] {
                for (int q = 0; q < 4; q++)
                    if (b.quarter[q].rgba != have.quarter[q].rgba)
                        return true;
                return false;
            }();
            // And in these colours: the same block dumped in other colours elsewhere (by night)
            // would otherwise be recoloured, and an upscaler's blended edges with it.
            const uint32_t* lines[4];
            for (int q = 0; q < 4; q++)
                lines[q] = (b.quarter[q].reference[1] >> 24) ? b.quarter[q].reference : nullptr;
            const uint64_t colours = colours_key(lines);
            const bool has_around = around[0] || around[1] || around[2] || around[3];
            // (Lookups go: among these neighbours in these colours, in these colours, among
            // these neighbours, alone.)
            auto keep = [&](std::unordered_map<uint64_t, BlockCandidate>& map, uint64_t k) {
                BlockCandidate& slot = map[k];
                if (b.priority > slot.priority)
                    slot = b;
            };
            keep(best_block_colours, with_colours(keys[i], colours));
            if (differs && has_around) {
                keep(best_context, context_key(keys[i], around));
                keep(best_context, with_colours(context_key(keys[i], around), colours));
            }
            if (b.priority > have.priority)          // (the same block again: the first stays)
                have = std::move(b);
        }
        // Sprite pieces: every tile of the piece, turned back to the tiles' own way, known by
        // the size and the tiles. A piece partly hidden by another counts only when no frame
        // shows it whole; where it overlaps another, it keeps only its own pixels.
        for (const std::string& pl : piece_lines) {
            std::istringstream words(pl);
            int x, y, w, h, flags;
            words >> word;
            if (!(words >> x >> y >> w >> h >> flags) || w < 1 || w > 4 || h < 1 || h > 4 || x < 0 || y < 0
                || x + w * 8 > size_w || y + h * 8 > size_h)
                continue;
            std::vector<int> index(w * h, -1);
            bool ok = true;
            for (int k = 0; k < w * h && ok; k++) {
                std::string c;
                ok = (bool)(words >> c);
                if (ok && c != "-") {
                    index[k] = std::atoi(c.c_str());
                    ok = index[k] >= 0 && index[k] < (int)cell_lines.size() && cell_lines[index[k]].x >= 0;
                }
            }
            if (!ok)
                continue;
            bool hidden = false, overlapping = false;
            for (int i : index)
                if (i >= 0) {
                    hidden |= (cell_lines[i].flags & 4) != 0;
                    overlapping |= (cell_lines[i].flags & 8) != 0;
                }
            PieceCandidate p;
            p.priority = hidden ? priority - 20 : priority;
            std::vector<uint64_t> pattern(w * h);
            p.tiles.resize(w * h);
            for (int cx = 0; cx < w; cx++)
                for (int cy = 0; cy < h; cy++) {
                    const int k = cx * h + cy, i = index[k];
                    const int sx = (flags & 1) ? w - 1 - cx : cx, sy = (flags & 2) ? h - 1 - cy : cy;
                    const int tx = x + sx * 8, ty = y + sy * 8;
                    pattern[k] = i >= 0 ? cell_lines[i].hash : empty_pattern();
                    Candidate& c = p.tiles[k];
                    c.priority = i < 0 ? priority : !(cell_lines[i].flags & 12) ? priority
                                                    : (cell_lines[i].flags & 4) ? priority - 20 : priority - 10;
                    c.rgba = cut(tx, ty, flags);
                    // Overlapping others: its own pixels only (a blank tile: none of them).
                    const uint64_t own = i >= 0 ? ((cell_lines[i].flags & 8) ? cell_lines[i].own : ~0ull) : overlapping ? 0 : ~0ull;
                    if (own != ~0ull) {
                        const int n = 8 * sc;
                        for (int py = 0; py < n; py++)
                            for (int px = 0; px < n; px++) {
                                // (own is as the picture shows the tile; rgba is turned back)
                                const int fx = (flags & 1) ? n - 1 - px : px, fy = (flags & 2) ? n - 1 - py : py;
                                if (!(own >> ((fy / sc) * 8 + fx / sc) & 1))
                                    c.rgba[(size_t)py * n + px] = 0;
                            }
                    }
                    const int pal = i >= 0 ? cell_lines[i].palette : -1;
                    if (pal >= 0 && pal < (int)palettes.size() && palettes[pal].size() == 16)
                        std::memcpy(c.reference, palettes[pal].data(), sizeof(c.reference));
                }
            p.pattern = pattern;
            PieceCandidate& have = best_piece[piece_key(w, h, pattern.data())];
            if (p.priority > have.priority)
                have = std::move(p);
        }
        SDL_DestroySurface(s);
    }
    if (best.empty() && best_block.empty() && best_piece.empty()) {
        why = "no pictures redrawn larger in " + dir.string();
        return false;
    }
    const int size = 8 * scale;
    // Blocks and pieces need their tiles' slots in a row; the pictures themselves are kept
    // once each (most of a block's or a piece's tiles look as they do alone).
    std::vector<const Candidate*> pictures;                 // per slot
    std::unordered_map<uint64_t, int> picture_slot;         // picture content -> its own slot
    std::vector<int> picture_of;                            // per slot: the slot holding its picture
    auto content = [](const Candidate& c) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            for (size_t k = 0; k < n; k++) {
                h ^= ((const uint8_t*)p)[k];
                h *= 1099511628211ull;
            }
        };
        mix(c.rgba.data(), c.rgba.size() * 4);
        mix(c.reference, sizeof(c.reference));      // (each texel is tagged with its nearest dumped colour)
        return h;
    };
    int stored = 0;
    auto put = [&](const Candidate& c) {
        const int at = (int)pictures.size();
        pictures.push_back(&c);
        auto [it, fresh] = picture_slot.try_emplace(content(c), at);
        picture_of.push_back(it->second);
        stored += fresh;
        return at;
    };
    for (const auto& [hash, c] : best)
        slot[hash] = put(c);
    for (const auto& [key, b] : best_block) {
        block[key] = (int)pictures.size();
        for (const Candidate& c : b.quarter)
            put(c);
    }
    for (const auto& [key, b] : best_context) {
        block_context[key] = (int)pictures.size();
        for (const Candidate& c : b.quarter)
            put(c);
    }
    // Each block in each set of colours it was dumped in (equal pictures share the atlas).
    for (const auto& [key, b] : best_block_colours) {
        block[key] = (int)pictures.size();
        for (const Candidate& c : b.quarter)
            put(c);
    }
    for (const auto& [key, p] : best_piece) {
        piece[key] = (int)pictures.size();
        // A tile the piece's picture shows partly hidden: the tile's own best picture, if that
        // shows it more whole.
        for (size_t k = 0; k < p.tiles.size(); k++) {
            auto alone = best.find(p.pattern[k]);
            put(alone != best.end() && alone->second.priority > p.tiles[k].priority
                    ? alone->second
                    : p.tiles[k]);
        }
    }
    // The atlas: each distinct picture once; slot_atlas maps a slot to its place there.
    if (progress)
        progress->store(0.95f);
    slots = (int)pictures.size();
    atlas_width = kPerRow * size;
    atlas_height = (stored + kPerRow - 1) / kPerRow * size;
    atlas.assign((size_t)atlas_width * atlas_height, 0);
    atlas_blend.assign(atlas.size(), 0);
    reference.assign((size_t)slots * 16, 0);
    slot_atlas.assign(slots, 0);
    std::vector<int> place(slots, -1);
    int placed_count = 0;
    for (int k = 0; k < slots; k++) {
        const int owner = picture_of[k];
        if (place[owner] < 0) {
            place[owner] = placed_count++;
            const int ax = place[owner] % kPerRow * size, ay = place[owner] / kPerRow * size;
            const Candidate& c = *pictures[owner];
            // Alpha: 0 see-through; otherwise 128 + the dumped colour (1-15) the texel is closest
            // to, so the shader recolours it as that colour changes (fades, night, flashes);
            // 128 when the picture's colours aren't known (not recoloured).
            const bool known = (c.reference[1] >> 24) != 0;
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++) {
                    uint32_t t = c.rgba[(size_t)y * size + x];
                    if ((t >> 24) < 128) {
                        t = 0;
                    } else {
                        int nearest = 0, best_d = 1 << 30;
                        for (int k = 1; known && k < 16; k++) {
                            const uint32_t r = c.reference[k];
                            const int dr = (int)(t & 255) - (int)(r >> 16 & 255), dg = (int)(t >> 8 & 255) - (int)(r >> 8 & 255),
                                      db = (int)(t >> 16 & 255) - (int)(r & 255);
                            const int d = dr * dr + dg * dg + db * db;
                            if (d < best_d)
                                best_d = d, nearest = k;
                        }
                        // Not one of the colours: the blend of it with another that comes closest.
                        // (Any two of the colours: the nearest single one can be a third colour
                        // that happens to lie close to the blend.)
                        if (nearest && best_d > 12) {
                            const float tr = (float)(t & 255), tg = (float)(t >> 8 & 255), tb = (float)(t >> 16 & 255);
                            float best_e = (float)best_d * 0.5f;
                            int first = 0, other = 0, share_q = 0;
                            for (int i = 1; i < 16; i++) {
                                const uint32_t A = c.reference[i];
                                if (!(A >> 24))
                                    continue;
                                const float ar = (float)(A >> 16 & 255), ag = (float)(A >> 8 & 255), ab = (float)(A & 255);
                                for (int j = i + 1; j < 16; j++) {
                                    const uint32_t B = c.reference[j];
                                    if (!(B >> 24) || B == A)
                                        continue;
                                    const float br = (float)(B >> 16 & 255) - ar, bg = (float)(B >> 8 & 255) - ag, bb = (float)(B & 255) - ab;
                                    const float len = br * br + bg * bg + bb * bb;
                                    float share = ((tr - ar) * br + (tg - ag) * bg + (tb - ab) * bb) / len;
                                    share = share < 0 ? 0 : share > 1 ? 1 : share;
                                    const int q = (int)(share * 15.0f + 0.5f);
                                    if (q == 0 || q == 15)
                                        continue;
                                    const float s = (float)q / 15.0f;
                                    const float er = tr - (ar + s * br), eg = tg - (ag + s * bg), eb = tb - (ab + s * bb);
                                    const float e = er * er + eg * eg + eb * eb;
                                    if (e < best_e)
                                        best_e = e, first = i, other = j, share_q = q;
                                }
                            }
                            if (other) {
                                if (share_q > 7)                // (tagged with the nearer one, the share of the other)
                                    std::swap(first, other), share_q = 15 - share_q;
                                nearest = first;
                                atlas_blend[(size_t)(ay + y) * atlas_width + ax + x] = (uint32_t)(other << 4 | share_q);
                            }
                        }
                        t = (t & 0x00FFFFFF) | (uint32_t)(128 + nearest) << 24;
                    }
                    atlas[(size_t)(ay + y) * atlas_width + ax + x] = t;
                }
        }
        slot_atlas[k] = place[owner];
        std::memcpy(&reference[(size_t)k * 16], pictures[k]->reference, sizeof(pictures[k]->reference));
    }
    std::printf("art pack: %d tiles, %d blocks (%d more beside other blocks) and %d sprite pieces at %dx (%d pictures, atlas %dx%d) from %s%s",
                (int)best.size(), (int)best_block.size(), (int)best_context.size(), (int)best_piece.size(), scale, stored,
                atlas_width, atlas_height, dir.string().c_str(),
                other_sizes ? " (pictures of other sizes left out)" : "");
    if (conflicts)
        std::printf("; %d tiles differ between pictures", conflicts);
    std::printf("\n");
    return true;
}

Dumper::Dumper(std::filesystem::path d) : dir(std::move(d))
{
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    // Patterns dumped before (a dump can be added to over several runs).
    for (std::filesystem::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
        if (it->path().extension() == ".png" && it->path().stem().string().size() == 16)
            done.insert(std::strtoull(it->path().stem().string().c_str(), nullptr, 16));
}

void Dumper::save(const Vdp& vdp, int tile, int line)
{
    const uint8_t* bytes = &vdp.vram[(tile & 0x7FF) * 32];
    if (blank(bytes) || stable[line] < kSettled)
        return;
    const uint64_t hash = pattern_hash(bytes);
    if (!done.insert(hash).second)
        return;
    SDL_Surface* s = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_ARGB8888);
    if (!s)
        return;
    uint32_t colours[16];
    for (int i = 0; i < 16; i++)
        colours[i] = argb(vdp.cram[line * 16 + i]);
    // A line of one colour (all black while a stage loads, before its fade in) shows nothing
    // of the tile: wait for its real colours.
    bool varied = false;
    for (int i = 2; i < 16; i++)
        varied |= colours[i] != colours[1];
    if (!varied) {
        done.erase(hash);
        return;
    }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            const int pair = bytes[y * 4 + x / 2], index = x & 1 ? pair & 15 : pair >> 4;
            ((uint32_t*)((uint8_t*)s->pixels + y * s->pitch))[x] = index ? colours[index] : 0;
        }
    const std::string name = hash_name(hash);
    SDL_SavePNG(s, (dir / (name + ".png")).string().c_str());
    SDL_DestroySurface(s);
    if (FILE* f = std::fopen((dir / "palettes.txt").string().c_str(), "a")) {
        std::fprintf(f, "%s", name.c_str());
        for (uint32_t c : colours)
            std::fprintf(f, " %08X", c);
        std::fprintf(f, "\n");
        std::fclose(f);
    }
}

void Dumper::frame(const Vdp& vdp)
{
    for (int line = 0; line < 4; line++) {
        const bool same = std::memcmp(&last_cram[line * 16], &vdp.cram[line * 16], 32) == 0;
        stable[line] = same ? stable[line] + 1 : 0;
    }
    std::memcpy(last_cram, vdp.cram, sizeof(last_cram));
    const uint8_t* reg = vdp.reg;
    if (!(reg[1] & 0x40))
        return;
    const bool h40 = reg[12] & 1;
    auto size = [](int s) { return s == 1 ? 64 : s == 3 ? 128 : 32; };
    const int plane_w = size(reg[16] & 3), plane_h = size((reg[16] >> 4) & 3);
    auto table = [&](int base, int w, int h) {
        for (int i = 0; i < w * h; i++) {
            const int at = (base + i * 2) & 0xFFFF;
            const int entry = vdp.vram[at] << 8 | vdp.vram[at | 1];
            save(vdp, entry & 0x7FF, (entry >> 13) & 3);
        }
    };
    table((reg[2] & 0x38) << 10, plane_w, plane_h);
    table((reg[4] & 7) << 13, plane_w, plane_h);
    table(h40 ? (reg[3] & 0x3C) << 10 : (reg[3] & 0x3E) << 10, h40 ? 64 : 32, 32);
    const int base_sprites = h40 ? (reg[5] & 0x7E) << 9 : (reg[5] & 0x7F) << 9;
    int link = 0;
    for (int n = 0; n < 80; n++) {
        const uint8_t* s = &vdp.vram[(base_sprites + link * 8) & 0xFFFF];
        const int cells = (((s[2] >> 2) & 3) + 1) * ((s[2] & 3) + 1);
        const int attr = s[4] << 8 | s[5];
        for (int c = 0; c < cells; c++)
            save(vdp, (attr & 0x7FF) + c, (attr >> 13) & 3);
        link = s[3] & 0x7F;
        if (!link)
            break;
    }
}

}  // namespace art
