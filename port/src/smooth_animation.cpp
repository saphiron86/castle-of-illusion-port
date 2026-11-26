// Smooth Animation: see smooth_animation.h and docs/notes/smooth-animation.md.
//
// For each object in kKinds, the sprites DrawObject wrote for it (FrameInterpolator's owners,
// by sprite table index) are replaced by one picture: frame 0 of the animation it is playing,
// built from that frame's sprite mapping in the ROM and the object's tile base, turned about
// its middle. Frame 0 of each animation is where the object's middle is, so the picture stays
// steady where the game's frames jump about (the big apple's slope frames 5-7 sit 16 pixels
// back and 8 up).
//
// The angle is the game's own: one frame of the animation is a turn / steps, plus the part of
// it shown so far (its timer), so it turns at the game's pace, in step with its frames. When
// the animation changes (the apple's roll to the slope's), the angle carries on from where it
// was and the difference fades out over the next frames, rather than jumping.

#include "smooth_animation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace smooth_animation {
namespace {

// The objects drawn turned: their ID, and the frames of their animation in one whole turn
// (clockwise; negative: anticlockwise). Stage 24's boulders aren't a turn: their light stays put.
struct Kind {
    uint16_t id;
    int steps;
    int anim = -1;                                  // the one animation (ObjAnim) that turns; -1: any
    // The way it turns follows its X speed (the game's frames turn one way only): `steps`'s way
    // moving right, the other moving left.
    bool follows_x = false;
    // Not a turn but a swing, back and forth about the middle of the frame's bottom edge:
    // `swing` degrees each way (0: a turn), anticlockwise first. Its animation is one swing:
    // upright, leaning anticlockwise, upright, leaning clockwise, so the angle is a sine of
    // the animation's progress, flattened towards its peaks (sin^0.6) since the game holds each
    // lean for two frames of the six.
    float swing = 0;
    // A swing's body and head (0: one piece): the frame's pixels on rows from body_row, or from
    // body_row2 at columns from body_col, are the body that swings; the rest is the head, carried
    // on the body's top middle (body_row2) and kept upright, as the game's frames keep it.
    int body_row = 0, body_row2 = 0, body_col = 0;
    // A swing's turning point, in pixels down from the frame's top (-1: its bottom edge), and
    // where its sine starts, in animation frames (0: the frame 0 start is upright).
    int swing_pivot = -1;
    float swing_phase = 0;
    // The frame of the animation drawn upright and turned (0 for all but the bell, whose frame
    // 0 leans).
    int upright = 0;
    // Draw only where one of the animation's own frames draws (the bell keeps inside its arch,
    // whose side pillars it would otherwise swing over).
    bool clip_to_frames = false;
    // Rolls: the angle follows how far it moves across (radians: pixels / roll_radius), so it
    // rolls without sliding, either way (0: the animation's angle).
    float roll_radius = 0;
    // Colours (a bit each) of a light that mustn't turn with it: those pixels show the picture's
    // pixel on the other side of its middle instead (the light left out altogether), and the
    // picture is widened a little past its edge (drawn only inside the frames' outline, with
    // clip_to_frames), so its edge stays whole as it turns.
    uint16_t unlit = 0;
};
constexpr Kind kKinds[] = {
    {0x54, 8},                                      // BigApple: stage 2's giant apple
    {0x57, 8},                                      // BigAppleLibrary: the same in stages 16-19
    {0x47, -4},                                     // Mace: thrown by stage 24's Knights (anticlockwise)
    {0xD9, 4, 0x00, true},                          // OldOakTree: stage 5's boss, rolling (anim 0 only)
    {0x8C, 24, 0x00},                               // Gear: stage 26's cogs (3 frames of 15 degrees)
    {0x8C, -24, 0x02},                              // (the other way: the same frames backwards)
    // SpringHead: stage 6's jack-in-the-box, swaying on its spring. Its frames lean the head
    // about 10 pixels at 22 up (24 degrees; 16 looked too small in play). Anims $2A and $30
    // sway; $28 stands still.
    // The spring swings; the head and tongue (rows 0-21, but for the spring's top beside the
    // tongue: rows 16-21 from column 10) stay upright on top of it, the tongue hanging down.
    {0x37, 0, 0x2A, false, 24, 22, 16, 10},
    {0x37, 0, 0x30, false, 24, 22, 16, 10},
    // Decoration subtype 1: stage 6 and 8's bells, hanging in the arches. Anim $02 is left,
    // left, middle, right, right, middle (13 game frames each), 3 drawings of a 16 x 16 bell; a
    // pendulum about its top. The side drawings lean about 24 to 46 degrees by fit
    // (loosely: they're redrawn, not turned); 30 each way looks closest, clockwise (left) first.
    {0x93, 0, 0x02, false, -30, 0, 0, 0, 0, 0.5f, 2, true},
    // RollingBall: stage 24's boulders, anim $44 (3 frames: the surface moves under a blue light
    // at the top). Drawn without the light (colours 12 and 13, the light and its grey; the
    // surface is 14 and 15), frame 0 rolling with its travel, 24 pixels round.
    {0x4C, 0, 0x44, false, 0, 0, 0, 0, -1, 0, 0, true, 24.0f, (1 << 12) | (1 << 13)},
};

struct State {
    double shown = 0, offset = 0;                   // the angle drawn, and its difference from the game's
    int anim = -1, frame = -1, way = 1;
    long turns = 0;                                 // times the animation came round
    uint32_t seen = 0;
    int last_x = 0, last_y = 0;                     // (a roll: where it was the last frame)
    int steady_y = 0;                               // (a roll: its height drawn, the game's 2-pixel hops left out)
};
std::unordered_map<uint32_t, State> g_states;      // by owner key (slot and ID)
uint32_t g_frame = 0;

// Plane A pictures the game spins by rewriting their 16 tiles (Vdp::Spinner): the stage (its
// index, $FFFF43), the first tile, each tile's place in the 32 x 32 block (x | y << 2), the
// disc's middle and radius, and the turn each rewrite makes (radians). Each rewrite is taken as
// one step, either way (see update_spinners).
struct SpinKind {
    int stage;
    int first;
    uint8_t cell[16];
    float cx, cy, radius;
    double step;
    // Colours shaded where the frame casts its shadow: {plain, shaded} pairs (0, 0: none).
    uint8_t shades[4][2];
    int plane = 0;                                  // 0: plane A, 1: plane B
    // The way it always turns (1 clockwise, -1 anticlockwise; 0: found from each rewrite).
    int way = 0;
    uint16_t solid = 0;                             // Vdp::Spinner::solid
};
constexpr SpinKind kSpinKinds[] = {
    // Stage 6's pinwheels: tiles $27D-$28C, 2 x 2 tiles at a time across then down; 22.5
    // degrees a rewrite (8 blades, two colours). They rest, spin one way speeding up and
    // slowing down, then the other way, over and over.
    {5, 0x27D, {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15}, 16, 16, 13, 3.14159265358979323846 / 8,
     {{5, 6}, {13, 14}}},                           // (the blades' purple and blue, in shadow)
    // Stage 7's (at least one, found by the user in play): the same tiles, layout and colours.
    {6, 0x27D, {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15}, 16, 16, 13, 3.14159265358979323846 / 8,
     {{5, 6}, {13, 14}}},
    // Stage 8's pinwheels: the same tiles, layout and colour indices, in the stage's own colours.
    {7, 0x27D, {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15}, 16, 16, 13, 3.14159265358979323846 / 8,
     {{5, 6}, {13, 14}}},
    // Stage 26's background gears (plane B): tiles $2A9-$2B8, laid out as the pinwheels' (the
    // whole block turns: its teeth reach its corners); 8
    // teeth, 3 pictures 15 degrees apart, a rewrite every 8 frames, always anticlockwise. Their
    // light turns with them (a 15 degree tilt at most).
    // Its upright picture's middle is at (16.25, 15.5) (its teeth sit a little off the block's
    // middle; turning about that keeps it from swaying).
    {25, 0x2A9, {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15}, 16.25f, 15.5f, 23, 3.14159265358979323846 / 12,
     {}, 1, -1, 0x78},                              // (colours 3-6 the gear, 1, 2, 9 the wall)
    // Stage 27 (the boss's arena) shows the same gears behind it.
    {26, 0x2A9, {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15}, 16.25f, 15.5f, 23, 3.14159265358979323846 / 12,
     {}, 1, -1, 0x78},
};
struct SpinState {
    uint64_t hash = 0;
    bool known = false;                             // a picture seen before this one
    uint8_t block[32 * 32] = {};                    // that picture (colour indices)
    uint32_t moving[32] = {};                       // pixels any rewrite changed (Vdp::Spinner::moving)
    uint8_t behind[32 * 32] = {};                   // what the pictures showed behind
    uint8_t filled[32 * 32] = {};                   // that, gaps filled (Vdp::Spinner::behind)
    bool has_base = false;                          // Vdp::Spinner::base, and its angle
    uint8_t base[32 * 32] = {};
    double base_angle = 0;
    int base_skew = 0;                              // how far from upright it is
    uint32_t changed = 0;                           // the game frame of the last rewrite
    double target = 0;                              // the angle of the picture VRAM holds
    double from = 0, shown = 0;                     // where this step's turn started; the angle drawn
    float duration = 1;                             // frames the step's turn takes
};
SpinState g_spin[sizeof(kSpinKinds) / sizeof(kSpinKinds[0])];

// The 16 tiles as the 32 x 32 block they make.
void spin_block(const Vdp& vdp, const SpinKind& kind, uint8_t* out)
{
    for (int i = 0; i < 16; i++) {
        const int bx = (kind.cell[i] & 3) * 8, by = (kind.cell[i] >> 2) * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                const uint8_t pair = vdp.vram[((kind.first + i) * 32 + y * 4 + x / 2) & 0xFFFF];
                out[(by + y) * 32 + bx + x] = x & 1 ? pair & 15 : pair >> 4;
            }
    }
}

// How many disc pixels of `now` differ from `before` turned by `angle` (clockwise).
int spin_mismatch(const SpinKind& kind, const uint8_t* before, const uint8_t* now, double angle)
{
    const double c = std::cos(angle), s = std::sin(angle), r = kind.radius - 1;
    int bad = 0;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            const double dx = x + 0.5 - kind.cx, dy = y + 0.5 - kind.cy;
            if (dx * dx + dy * dy > r * r)
                continue;
            const int u = (int)std::floor(c * dx + s * dy + kind.cx), v = (int)std::floor(c * dy - s * dx + kind.cy);
            if (u >= 0 && v >= 0 && u < 32 && v < 32 && before[v * 32 + u] != now[y * 32 + x])
                bad++;
        }
    return bad;
}

void update_spinners(Vdp& vdp, const uint8_t* ram)
{
    for (size_t k = 0; k < sizeof(kSpinKinds) / sizeof(kSpinKinds[0]); k++) {
        const SpinKind& kind = kSpinKinds[k];
        SpinState& st = g_spin[k];
        if (ram[0xFF43] != kind.stage) {
            st = SpinState();
            continue;
        }
        uint64_t h = 1469598103934665603ull;        // (FNV-1a of the 16 tiles)
        for (int i = 0; i < 16 * 32; i++)
            h = (h ^ vdp.vram[(kind.first * 32 + i) & 0xFFFF]) * 1099511628211ull;
        if (h != st.hash) {
            // A rewrite: which way it turned (the game spins them both ways, speeding up and
            // slowing down), from the last picture turned a step either way. Its turn is drawn
            // over the frames the game took for it, from the angle drawn now: a step behind the
            // game, but with no jump when the way changes.
            uint8_t block[32 * 32];
            spin_block(vdp, kind, block);
            // Only a picture a step from the last counts (about 12% of the disc's pixels differ
            // from a pure turn; 30% allowed). Anything else, such as the stage's art arriving, is
            // a new start: no turn, and it marks nothing as moving.
            int way = 0;
            if (st.known) {
                const int cw = spin_mismatch(kind, st.block, block, kind.step);
                const int ccw = spin_mismatch(kind, st.block, block, -kind.step);
                const double disc = 3.14159265358979323846 * (kind.radius - 1) * (kind.radius - 1);
                // (One that always turns the same way, lit from a fixed side so its colours match
                // less well (stage 26's gears: about 25%), up to 50%.)
                if (std::min(cw, ccw) < (kind.way ? 0.5 : 0.3) * disc)
                    way = kind.way ? kind.way : cw <= ccw ? 1 : -1;
            }
            const double shown = way ? st.shown : st.target;
            st.target += way * kind.step;
            st.from = shown;
            st.duration = way ? (float)std::clamp<uint32_t>(g_frame - st.changed, 1, 30) : 1.0f;
            if (way)
                for (int i = 0; i < 32 * 32; i++)
                    if (block[i] != st.block[i])
                        st.moving[i / 32] |= 1u << (i % 32);
            if (kind.solid)
                for (int i = 0; i < 32 * 32; i++)
                    if (!(kind.solid >> block[i] & 1) && block[i])
                        st.behind[i] = block[i];
            if (kind.solid) {
                // Where no picture has shown the wall (a tooth in all of them), the nearest wall
                // colour.
                std::copy(st.behind, st.behind + 32 * 32, st.filled);
                for (int i = 0; i < 32 * 32; i++) {
                    if (st.behind[i])
                        continue;
                    int best = 1 << 30;
                    for (int j = 0; j < 32 * 32; j++)
                        if (st.behind[j]) {
                            const int dx = j % 32 - i % 32, dy = j / 32 - i / 32, d = dx * dx + dy * dy;
                            if (d < best)
                                best = d, st.filled[i] = st.behind[j];
                        }
                }
            }
            if (kind.solid && way) {
                // The most upright of the game's pictures (the most nearly mirrored left to
                // right and top to bottom: teeth straight up, down and across), which keeps its
                // shape best turned. The first a true step brings until a better one comes.
                int skew = 0;
                for (int y = 0; y < 32; y++)
                    for (int x = 0; x < 32; x++) {
                        const bool g = kind.solid >> block[y * 32 + x] & 1;
                        skew += g != (bool)(kind.solid >> block[y * 32 + 31 - x] & 1);
                        skew += g != (bool)(kind.solid >> block[(31 - y) * 32 + x] & 1);
                    }
                if (!st.has_base || skew < st.base_skew) {
                    std::copy(block, block + 32 * 32, st.base);
                    st.base_angle = st.target;
                    st.base_skew = skew;
                    st.has_base = true;
                }
            }
            std::copy(block, block + 32 * 32, st.block);
            st.known = true;
            st.hash = h;
            st.changed = g_frame;
        }
        const float elapsed = (float)(g_frame - st.changed);
        st.shown = st.from + (st.target - st.from) * std::min(1.0f, elapsed / st.duration);
        Vdp::Spinner sp;
        sp.first = kind.first;
        sp.plane = kind.plane;
        sp.solid = kind.solid;
        std::copy(st.filled, st.filled + 32 * 32, sp.behind);
        if (st.has_base) {
            sp.single = true;
            sp.offset = (float)(st.target - st.base_angle);
            std::copy(st.base, st.base + 32 * 32, sp.base);
        }
        std::copy(kind.cell, kind.cell + 16, sp.cell);
        sp.cx = kind.cx, sp.cy = kind.cy, sp.radius = kind.radius;
        sp.start = (float)(st.from - st.target);
        sp.elapsed = elapsed;
        sp.interval = st.duration;
        sp.angle = (float)(st.shown - st.target) + sp.offset;   // the picture VRAM holds (or base), turned to the angle drawn
        std::copy(st.moving, st.moving + 32, sp.moving);
        for (int c = 0; c < 16; c++)
            sp.shade[c] = sp.unshade[c] = (uint8_t)c;
        for (const auto& pair : kind.shades)
            if (pair[0] || pair[1])
                sp.shade[pair[0]] = pair[1], sp.unshade[pair[1]] = pair[0];
        vdp.spinners.push_back(sp);
    }
}

// Stage 6-8's white flowers (Vdp::Glider): 2 x 3 plane A cells the game rewrites so the flower
// bobs 4 pixels at a time between three places, held 32, 16, 32 and 16 frames (low, middle,
// high, middle). Its tiles: $2E9 blank, $2EA-$2ED the flower, $2EE-$2F3 the flower 4 pixels on
// and the stem. The middle and low layouts are exactly the high one moved down 4 and 8 pixels,
// so the flower is drawn as the high layout moved down by a smooth bob: a sine through the
// middle of each hold (one cycle, 96 frames, low at its middle), its phase from the game's
// rewrites. Stage 8 upside down: the same, mirrored top to bottom.
constexpr int kFlowerStages[] = {5, 6, 7};
constexpr int kFlowerBlank = 0x2E9;
struct GlideState {
    int place = -1;                                 // 0, 4, 8: pixels up from the low layout
    uint32_t seen = 0;
    bool phased = false;                            // a rewrite seen: the cycle's phase known
    double phase = 0;                               // at frame `at` (0-1, 0: the step to low)
    uint32_t at = 0;
    double period = 96;
    uint32_t stepped = 0;                           // the last rewrite's frame and phase
    double step_phase = 0;
    double fade = 0;                                // a jump at a resync, fading out
    float shown = 0;
};
std::unordered_map<uint32_t, GlideState> g_glide;  // by the area's top left cell

// The bob, pixels up from low, at phase f (0: the step to low; low held until 1/3, middle to
// 1/2, high to 5/6, middle to 1).
double bob(double f)
{
    return 4.0 - 4.0 * std::cos(2.0 * 3.14159265358979323846 * (f - 1.0 / 6.0));
}

void update_gliders(Vdp& vdp, const uint8_t* ram)
{
    if (std::find(std::begin(kFlowerStages), std::end(kFlowerStages), (int)ram[0xFF43]) == std::end(kFlowerStages)) {
        g_glide.clear();
        return;
    }
    static const int kSizes[4] = {32, 64, 32, 128};
    const int w = kSizes[vdp.reg[16] & 3], h = kSizes[(vdp.reg[16] >> 4) & 3];
    const int base = (vdp.reg[2] & 0x38) << 10;
    auto at = [&](int cx, int cy) {
        const int a = (base + (((cy % h + h) % h) * w + ((cx % w + w) % w)) * 2) & 0xFFFF;
        return (uint16_t)(vdp.vram[a] << 8 | vdp.vram[a + 1]);
    };
    auto is = [](uint16_t e, int tile, bool vf) { return (e & 0x7FF) == tile && ((e & 0x1000) != 0) == vf; };
    for (int cy = 0; cy < h; cy++)
        for (int cx = 0; cx < w; cx++) {
            // Found from the area's bottom row (upside down: its top row): the stem's foot
            // ($2F3: high), the middle layout's ($2F2), or the low layout's flower bottom
            // ($2EC $2ED, the flower top $2EA above).
            const uint16_t e = at(cx + 1, cy);
            for (int vf = 0; vf < 2; vf++) {
                const int in = vf ? 1 : -1;         // towards the area's far end
                int place = -1;
                if (is(e, 0x2F3, vf))
                    place = 8;
                else if (is(e, 0x2F2, vf))
                    place = 4;
                else if (is(e, 0x2ED, vf) && is(at(cx, cy), 0x2EC, vf) && is(at(cx, cy + in), 0x2EA, vf)
                         && !is(at(cx + 1, cy - in), 0x2F3, vf))
                    place = 0;
                if (place < 0)
                    continue;
                const int top = vf ? cy : cy - 2;
                const uint32_t key = (uint32_t)((top % h + h) % h) << 16 | (uint32_t)cx;
                GlideState& s = g_glide[key];
                if (s.place < 0)
                    s.shown = (float)place;
                if (s.place >= 0 && place != s.place && s.seen == g_frame - 1) {
                    // A rewrite: where it falls in the cycle (4 pixels either way only).
                    const int d = place - s.place;
                    double f = -1;
                    if (d == 4 && place == 4) f = 1.0 / 3.0;
                    else if (d == 4 && place == 8) f = 0.5;
                    else if (d == -4 && place == 4) f = 5.0 / 6.0;
                    else if (d == -4 && place == 0) f = 0.0;
                    if (f < 0) {
                        s.phased = false;
                    } else {
                        if (s.phased) {             // the cycle's length, from the last step
                            double df = f - s.step_phase;
                            if (df <= 0) df += 1;
                            const double p = (g_frame - s.stepped) / df;
                            if (p > 24 && p < 400)
                                s.period = p;
                        }
                        // (no jump: the difference from the bob drawn so far fades out)
                        if (s.phased)
                            s.fade += bob(s.phase + (g_frame - s.at) / s.period) - bob(f);
                        else
                            s.fade = s.shown - bob(f);
                        s.phase = f, s.at = g_frame;
                        s.phased = true;
                        s.stepped = g_frame, s.step_phase = f;
                    }
                }
                s.place = place;
                s.seen = g_frame;
                double up = place;
                if (s.phased) {
                    up = bob(s.phase + (g_frame - s.at) / s.period) + s.fade;
                    if (std::fabs(up - s.fade - place) > 3.0)   // out of step with the game
                        s.phased = false, up = place, s.fade = 0;
                }
                s.fade *= 0.9;
                if (std::fabs(s.fade) < 0.01)
                    s.fade = 0;
                const float last = s.shown;
                s.shown = (float)up;
                Vdp::Glider g;
                g.x = cx * 8, g.y = ((top % h + h) % h) * 8;
                // The high layout, top to bottom (upside down: mirrored), moved towards the ground.
                const uint16_t attr = e & 0xE000;
                const uint16_t high[6] = {0x2EA, 0x2EB, 0x2EC, 0x2ED, (uint16_t)kFlowerBlank, 0x2F3};
                for (int i = 0; i < 6; i++) {
                    const int row = vf ? 2 - i / 2 : i / 2;
                    g.cell[row * 2 + i % 2] = (uint16_t)(attr | high[i] | (vf ? 0x1000 : 0));
                }
                g.offset = (float)(vf ? -(8.0 - up) : 8.0 - up);
                g.rate = vf ? (s.shown - last) : -(s.shown - last);
                vdp.gliders.push_back(g);
            }
        }
    for (auto it = g_glide.begin(); it != g_glide.end();)
        it = g_frame - it->second.seen > 600 ? g_glide.erase(it) : std::next(it);
}

// Stage 26's big wheels: their spokes are stripes in palette 2's colours 10, 11, 12 (16 of
// each, 22.5 degrees a set), and the game turns them by cycling those colours a step every 11
// frames: each step shows the stripes 7.5 degrees on. Drawn turning smoothly (Vdp::Cycler),
// a step behind. The rim's dots (104 segments, 66.5-72 pixels out) are in only two of the
// three colours, so they have no way round to turn: the rim is drawn as 105 segments (so the
// three colours go round evenly) in the three colours in turn, turned with the spokes, a
// segment a step. Found by the wheel's top middle (tile $219, then $219 mirrored), 64 pixels
// above its middle, or the bottom middle (the same, flipped), or the left or right middle
// (tile $24A over it flipped, 80 pixels from the middle).
struct CycleKind {
    int stage, anchor, dx, dy, palette;
    int side, side_dx;                              // the left middle piece, and the middle from its left edge
    int colour[3];                                  // in the order the stripes go round (clockwise)
    float inner, outer;
    double step;                                    // radians from one colour's stripe to the next
    float rim_inner, rim_outer, rim_segments, rim_start;   // (Vdp::Cycler)
};
constexpr CycleKind kCycleKinds[] = {
    {25, 0x219, 8, 64, 2, 0x24A, 80, {10, 11, 12}, 15.5f, 31.5f, 7.5 * 3.14159265358979323846 / 180,
     66.5f, 72.0f, 105.0f, (float)(1.7 * 3.14159265358979323846 / 180)},
    {26, 0x219, 8, 64, 2, 0x24A, 80, {10, 11, 12}, 15.5f, 31.5f, 7.5 * 3.14159265358979323846 / 180,   // (stage 27: the same)
     66.5f, 72.0f, 105.0f, (float)(1.7 * 3.14159265358979323846 / 180)},
};
struct CycleState {
    uint16_t colours[3] = {};
    bool known = false;
    uint32_t changed = 0;
    float interval = 11;
    int way = 0;                                    // the last step: 1 the colours moved on (clockwise), -1 back
};
CycleState g_cycle[sizeof(kCycleKinds) / sizeof(kCycleKinds[0])];
// Each wheel's rim's middle, by where its top middle tiles are (x | y << 16, plane pixels): the
// game builds some wheels from overlapping pieces (stage 27's right one has its rim 16 pixels
// above where its middle tiles, and its spokes, say), so the rim's middle is where the most
// rim pixels lie at the rim's radius, within 24 pixels. Kept while the wheel is in the plane's table (with the frame it was last
// seen).
struct WheelMiddle { uint32_t at, seen; };
std::unordered_map<uint32_t, WheelMiddle> g_wheel_middle;

void update_cyclers(Vdp& vdp, const uint8_t* ram)
{
    static const int kSizes[4] = {32, 64, 32, 128};
    const int w = kSizes[vdp.reg[16] & 3], h = kSizes[(vdp.reg[16] >> 4) & 3];
    const int base = (vdp.reg[2] & 0x38) << 10;
    auto at = [&](int x, int y) {
        const int a = (base + (((y % h + h) % h) * w + ((x % w + w) % w)) * 2) & 0xFFFF;
        return (uint16_t)(vdp.vram[a] << 8 | vdp.vram[a + 1]);
    };
    for (size_t k = 0; k < sizeof(kCycleKinds) / sizeof(kCycleKinds[0]); k++) {
        const CycleKind& kind = kCycleKinds[k];
        CycleState& st = g_cycle[k];
        if (ram[0xFF43] != kind.stage) {
            st = CycleState();
            continue;
        }
        uint16_t now[3];
        for (int i = 0; i < 3; i++)
            now[i] = vdp.cram[kind.palette * 16 + kind.colour[i]];
        if (!st.known || !std::equal(now, now + 3, st.colours)) {
            // A step: each colour's look moved on to the next colour's stripes, or back.
            int way = 0;
            if (st.known) {
                if (now[1] == st.colours[0] && now[2] == st.colours[1] && now[0] == st.colours[2])
                    way = 1;
                else if (now[0] == st.colours[1] && now[1] == st.colours[2] && now[2] == st.colours[0])
                    way = -1;
            }
            if (way)
                st.interval = (float)std::clamp<uint32_t>(g_frame - st.changed, 1, 30);
            st.way = way;
            st.changed = g_frame;
            std::copy(now, now + 3, st.colours);
            st.known = true;
        }
        const float elapsed = (float)(g_frame - st.changed);
        // The wheels, by any of their four middle edge pieces (only part of a wheel may be in the
        // plane's table: one coming in at widescreen's edges), each once (the first found).
        std::vector<std::pair<int, int>> middles;
        auto add = [&](int x, int y) {
            x = (x % (w * 8) + w * 8) % (w * 8), y = (y % (h * 8) + h * 8) % (h * 8);
            for (const auto& q : middles) {
                const int ax = std::abs(q.first - x), ay = std::abs(q.second - y);
                if (std::min(ax, w * 8 - ax) <= 24 && std::min(ay, h * 8 - ay) <= 24)
                    return;
            }
            middles.push_back({x, y});
        };
        auto is = [&](uint16_t e, int tile, int flips) {
            return (e & 0x7FF) == tile && (e & 0x1800) == flips && ((e >> 13) & 3) == kind.palette;
        };
        for (int edge = 0; edge < 4; edge++)        // top, bottom, left, right
            for (int cy = 0; cy < h; cy++)
                for (int cx = 0; cx < w; cx++) {
                    const uint16_t e = at(cx, cy);
                    if (edge == 0 && is(e, kind.anchor, 0) && is(at(cx + 1, cy), kind.anchor, 0x0800))
                        add(cx * 8 + kind.dx, cy * 8 + kind.dy);
                    else if (edge == 1 && is(e, kind.anchor, 0x1000) && is(at(cx + 1, cy), kind.anchor, 0x1800))
                        add(cx * 8 + kind.dx, cy * 8 + 8 - kind.dy);
                    else if (edge == 2 && is(e, kind.side, 0) && is(at(cx, cy + 1), kind.side, 0x1000))
                        add(cx * 8 + kind.side_dx, cy * 8 + 8);
                    else if (edge == 3 && is(e, kind.side, 0x0800) && is(at(cx, cy + 1), kind.side, 0x1800))
                        add(cx * 8 + 8 - kind.side_dx, cy * 8 + 8);
                }
        for (const auto& mid : middles) {
                Vdp::Cycler c;
                c.x = mid.first, c.y = mid.second;
                const uint32_t key = (uint32_t)c.x | (uint32_t)c.y << 16;
                auto found = g_wheel_middle.find(key);
                // (found again at each step of the colours: the stage may still be drawing it
                // when it is first seen)
                if (found == g_wheel_middle.end() || elapsed == 0) {
                    std::vector<std::pair<int, int>> rim;     // rim colours, from the middle given
                    const int reach = (int)kind.rim_outer + 24;
                    for (int y = -reach; y <= reach; y++)
                        for (int x = -reach; x <= reach; x++) {
                            const int px = c.x + x, py = c.y + y;
                            const uint16_t t = at(px >> 3, py >> 3);
                            if (((t >> 13) & 3) != kind.palette)
                                continue;
                            const int tx = t & 0x0800 ? 7 - (px & 7) : px & 7, ty = t & 0x1000 ? 7 - (py & 7) : py & 7;
                            const uint8_t pair = vdp.vram[((t & 0x7FF) * 32 + ty * 4 + tx / 2) & 0xFFFF];
                            const int colour = tx & 1 ? pair & 15 : pair >> 4;
                            if (colour == kind.colour[1] || colour == kind.colour[2])
                                rim.push_back({x, y});
                        }
                    auto count = [&](int dx, int dy) {
                        int n = 0;
                        for (const auto& q : rim) {
                            const float rx = q.first + 0.5f - dx, ry = q.second + 0.5f - dy;
                            const float r2 = rx * rx + ry * ry;
                            n += r2 >= kind.rim_inner * kind.rim_inner && r2 < kind.rim_outer * kind.rim_outer;
                        }
                        return n;
                    };
                    int best = -1, bx = 0, by = 0;
                    for (int dy = -24; dy <= 24; dy += 4)       // (every 4 pixels, then each near the best)
                        for (int dx = -24; dx <= 24; dx += 4) {
                            const int n = count(dx, dy);
                            if (n > best || (n == best && dx * dx + dy * dy < bx * bx + by * by))
                                best = n, bx = dx, by = dy;
                        }
                    const int cx0 = bx, cy0 = by;
                    for (int dy = cy0 - 3; dy <= cy0 + 3; dy++)
                        for (int dx = cx0 - 3; dx <= cx0 + 3; dx++) {
                            const int n = count(dx, dy);
                            if (n > best || (n == best && dx * dx + dy * dy < bx * bx + by * by))
                                best = n, bx = dx, by = dy;
                        }
                    const uint32_t middle = (uint32_t)((c.x + bx + w * 8) % (w * 8)) | (uint32_t)((c.y + by + h * 8) % (h * 8)) << 16;
                    found = g_wheel_middle.insert_or_assign(key, WheelMiddle{middle, g_frame}).first;
                }
                found->second.seen = g_frame;
                c.rim_x = (int)(found->second.at & 0xFFFF), c.rim_y = (int)(found->second.at >> 16);
                c.inner = kind.inner, c.outer = kind.outer;
                c.start = (float)(st.way * kind.step);
                c.elapsed = elapsed, c.interval = st.interval;
                c.angle = c.start * (1.0f - std::min(1.0f, elapsed / st.interval));
                c.palette = kind.palette;
                for (int i = 0; i < 3; i++) {
                    c.colours |= (uint16_t)(1u << kind.colour[i]);
                    c.order[i] = (uint8_t)kind.colour[i];
                }
                c.step = (float)kind.step;
                c.rim_inner = kind.rim_inner, c.rim_outer = kind.rim_outer;
                c.rim_segments = kind.rim_segments, c.rim_start = kind.rim_start;
                c.spoke_start = 0;                  // (a stripe of the first colour starts straight right)
                vdp.cyclers.push_back(c);
            }
    }
    for (auto it = g_wheel_middle.begin(); it != g_wheel_middle.end();)
        it = it->second.seen == g_frame ? std::next(it) : g_wheel_middle.erase(it);
}

// Stage 26's big horizontal gears: drums seen edge on, 20 x 4 cells (the bottom half the top
// mirrored), tiles $271-$298, rewritten every 8 frames: 3 pictures, each with the gaps between
// the teeth about 7 degrees further round (moving right), lit brightest in the middle. Each
// place's face colour is the lightest (lowest) its pictures show there; anything else is a gap
// or its edge. Found by their top left cell (tile $271).
struct DrumKind {
    int stage, first, tiles, cells_w, cells_h;
    double step;                                    // radians a rewrite turns
};
constexpr DrumKind kDrumKinds[] = {
    {25, 0x271, 40, 20, 4, 7.0 * 3.14159265358979323846 / 180},
    {26, 0x271, 40, 20, 4, 7.0 * 3.14159265358979323846 / 180},   // (stage 27, if it shows one)
};
struct DrumState {
    uint64_t hash = 0;
    bool known = false;
    uint32_t changed = 0;
    float interval = 8;
    int way = 0;
    std::vector<uint8_t> look;
};
DrumState g_drum[sizeof(kDrumKinds) / sizeof(kDrumKinds[0])];

void update_drums(Vdp& vdp, const uint8_t* ram)
{
    static const int kSizes[4] = {32, 64, 32, 128};
    const int w = kSizes[vdp.reg[16] & 3], h = kSizes[(vdp.reg[16] >> 4) & 3];
    const int base = (vdp.reg[2] & 0x38) << 10;
    auto at = [&](int x, int y) {
        const int a = (base + (((y % h + h) % h) * w + ((x % w + w) % w)) * 2) & 0xFFFF;
        return (uint16_t)(vdp.vram[a] << 8 | vdp.vram[a + 1]);
    };
    for (size_t k = 0; k < sizeof(kDrumKinds) / sizeof(kDrumKinds[0]); k++) {
        const DrumKind& kind = kDrumKinds[k];
        DrumState& st = g_drum[k];
        if (ram[0xFF43] != kind.stage) {
            st = DrumState();
            continue;
        }
        const int aw = kind.cells_w * 8, ah = kind.cells_h * 8;
        if (st.look.size() != (size_t)aw * ah)
            st.look.assign((size_t)aw * ah, 0);
        uint64_t hsh = 1469598103934665603ull;
        for (int i = 0; i < kind.tiles * 32; i++)
            hsh = (hsh ^ vdp.vram[(kind.first * 32 + i) & 0xFFFF]) * 1099511628211ull;
        const bool step = hsh != st.hash;
        if (step) {
            const int way = st.known ? 1 : 0;
            if (way)
                st.interval = (float)std::clamp<uint32_t>(g_frame - st.changed, 1, 30);
            st.way = way;
            st.changed = g_frame;
            st.hash = hsh;
            st.known = true;
        }
        const float elapsed = (float)(g_frame - st.changed);
        for (int cy = 0; cy < h; cy++)
            for (int cx = 0; cx < w; cx++) {
                const uint16_t e = at(cx, cy);
                if ((e & 0x7FF) != kind.first || (e & 0x1800))
                    continue;
                // The faces' colours: the lightest each place shows.
                if (step)
                    for (int y = 0; y < ah; y++)
                        for (int x = 0; x < aw; x++) {
                            const uint16_t c = at(cx + x / 8, cy + y / 8);
                            if ((c & 0x7FF) < kind.first || (c & 0x7FF) >= kind.first + kind.tiles)
                                continue;
                            const int tx = c & 0x0800 ? 7 - (x & 7) : x & 7, ty = c & 0x1000 ? 7 - (y & 7) : y & 7;
                            const uint8_t pair = vdp.vram[((c & 0x7FF) * 32 + ty * 4 + tx / 2) & 0xFFFF];
                            const int p = tx & 1 ? pair & 15 : pair >> 4;
                            if (!p)
                                continue;
                            uint8_t& l = st.look[(size_t)y * aw + x];
                            if (!l || p < l)
                                l = (uint8_t)p;
                        }
                Vdp::Drum d;
                d.x = cx * 8, d.y = cy * 8, d.w = aw, d.h = ah;
                d.first = kind.first, d.tiles = kind.tiles;
                d.start = (float)(st.way * kind.step);
                d.elapsed = elapsed, d.interval = st.interval;
                d.angle = d.start * (1.0f - std::min(1.0f, elapsed / st.interval));
                vdp.drums.push_back(d);
            }
        vdp.drum_look = st.look;
        vdp.drum_look_w = aw, vdp.drum_look_h = ah;
    }
}

// Stage 6-8's window shutters: tiles $26D-$27C (laid out as the pinwheels'), plane A, 3
// pictures: shut, half open (the doors a little ajar, leaning out at the top) and open (folded
// back past a right angle: their backs a 3-pixel strip outside the frame). The game holds shut
// 42 frames, half open 12, open 36, half open 6. Drawn as the shut picture's two doors swinging
// on their outer hinges in front of the open picture's window: opening over twice the half
// open picture's hold from when it shows, closing the same (each hold as last seen). Told
// apart by the window's colours (5, 6, 11, 12): none shut, most open.
struct ShutterKind {
    int stage, first;
    uint8_t cell[16];
    uint16_t window;                                // the window's colours (a bit each)
    int wall, shadow, back, edge;
    int hinge_l, split, hinge_r, sill;
    float distance, middle, shadow_k;
    double open;                                    // the open picture's angle (radians)
};
constexpr ShutterKind kShutterKinds[] = {
    {5, 0x26D, {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15}, (1 << 5) | (1 << 6) | (1 << 11) | (1 << 12),
     14, 15, 8, 9, 5, 16, 26, 26, 80.0f, 25.5f, 0.3f, 100.0 * 3.14159265358979323846 / 180},
    {7, 0x26D, {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15}, (1 << 5) | (1 << 6) | (1 << 11) | (1 << 12),
     14, 15, 8, 9, 5, 16, 26, 26, 80.0f, 25.5f, 0.3f, 100.0 * 3.14159265358979323846 / 180},
    // Stage 7's (later in the stage, found by the user in play): the same tiles, colour
    // indices and timing, in blue.
    {6, 0x26D, {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15}, (1 << 5) | (1 << 6) | (1 << 11) | (1 << 12),
     14, 15, 8, 9, 5, 16, 26, 26, 80.0f, 25.5f, 0.3f, 100.0 * 3.14159265358979323846 / 180},
};
struct ShutterState {
    uint64_t hash = 0;
    int shows = -1;                                 // 0 shut, 1 half open, 2 open (-1 not known)
    bool has_shut = false, has_open = false;
    uint8_t shut[32 * 32] = {}, open[32 * 32] = {};
    uint32_t changed = 0;                           // the frame the picture last changed
    float half_opening = 12, half_closing = 6;      // the half open picture's holds, as last seen
    int way = 0;                                    // the swing: 1 opening, -1 closing, 0 none
    uint32_t swing_at = 0;
    float swing_frames = 1;
};
ShutterState g_shutter[sizeof(kShutterKinds) / sizeof(kShutterKinds[0])];

// The doors' angle `elapsed` frames into a swing (eased in and out).
float shutter_angle(const Vdp::Shutter& s, float elapsed)
{
    if (s.start == 0)
        return s.angle;
    const float t = std::clamp(elapsed / s.interval, 0.0f, 1.0f);
    const float eased = 0.5f - 0.5f * std::cos(3.14159265f * t);
    return s.open_angle * (s.start > 0 ? eased : 1.0f - eased);
}

void update_shutters(Vdp& vdp, const uint8_t* ram)
{
    for (size_t k = 0; k < sizeof(kShutterKinds) / sizeof(kShutterKinds[0]); k++) {
        const ShutterKind& kind = kShutterKinds[k];
        ShutterState& st = g_shutter[k];
        if (ram[0xFF43] != kind.stage) {
            st = ShutterState();
            continue;
        }
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < 16 * 32; i++)
            h = (h ^ vdp.vram[(kind.first * 32 + i) & 0xFFFF]) * 1099511628211ull;
        if (h != st.hash) {
            uint8_t block[32 * 32];
            for (int i = 0; i < 16; i++) {
                const int bx = (kind.cell[i] & 3) * 8, by = (kind.cell[i] >> 2) * 8;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        const uint8_t pair = vdp.vram[((kind.first + i) * 32 + y * 4 + x / 2) & 0xFFFF];
                        block[(by + y) * 32 + bx + x] = x & 1 ? pair & 15 : pair >> 4;
                    }
            }
            int window = 0;
            for (int i = 0; i < 32 * 32; i++)
                window += kind.window >> block[i] & 1;
            const int shows = window == 0 ? 0 : window > 250 ? 2 : 1;
            const float held = (float)(g_frame - st.changed);
            if (st.shows == 0 && shows == 1) {
                st.way = 1, st.swing_at = g_frame, st.swing_frames = 2 * st.half_opening;
            } else if (st.shows == 2 && shows == 1) {
                st.way = -1, st.swing_at = g_frame, st.swing_frames = 2 * st.half_closing;
            } else if (st.shows == 1 && shows == 2 && st.way == 1) {
                st.half_opening = std::clamp(held, 2.0f, 60.0f);
            } else if (st.shows == 1 && shows == 0 && st.way == -1) {
                st.half_closing = std::clamp(held, 2.0f, 60.0f);
            } else if (shows != st.shows) {
                st.way = 0;                         // (out of the usual order: as the game draws it)
            }
            if (shows == 0)
                std::copy(block, block + 32 * 32, st.shut), st.has_shut = true;
            if (shows == 2)
                std::copy(block, block + 32 * 32, st.open), st.has_open = true;
            st.shows = shows;
            st.changed = g_frame;
            st.hash = h;
        }
        Vdp::Shutter s;
        s.open_angle = (float)kind.open;
        if (st.way != 0 && g_frame - st.swing_at < st.swing_frames + 1) {
            s.start = (float)st.way;
            s.elapsed = (float)(g_frame - st.swing_at);
            s.interval = st.swing_frames;
            s.angle = shutter_angle(s, s.elapsed);
        } else if (st.shows == 2 || (st.way == 1 && st.shows != 0)) {
            s.angle = s.open_angle;                 // (at rest open)
        } else {
            continue;                               // shut, or not known: the game's picture
        }
        if (!st.has_shut || !st.has_open)
            continue;
        s.first = kind.first;
        std::copy(kind.cell, kind.cell + 16, s.cell);
        s.hinge_l = kind.hinge_l, s.split = kind.split, s.hinge_r = kind.hinge_r, s.sill = kind.sill;
        s.distance = kind.distance, s.middle = kind.middle, s.shadow_k = kind.shadow_k;
        s.wall = kind.wall, s.shadow = kind.shadow, s.back = kind.back, s.edge = kind.edge;
        std::copy(st.shut, st.shut + 32 * 32, s.door);
        // Behind: the open picture's window, the doors' strips beside the frame and their
        // shadows left out (the wall there), its sill from the shut picture.
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++)
                s.behind[y * 32 + x] = y >= kind.sill ? st.shut[y * 32 + x]
                                     : x < kind.hinge_l || x >= kind.hinge_r - 1 ? (uint8_t)kind.wall
                                     : st.open[y * 32 + x];
        vdp.shutters.push_back(s);
    }
}

// Stage 6-8's flower buds: tiles $28D-$290 (2 x 2, row by row), plane A, 3 pictures:
// closed (a round bud), half open, open (a wide cup, its cream middle showing). The game plays
// them in a 40-frame cycle: open 12 frames, half 4, closed 4, half 4, closed 8, half 8. Told
// apart by the middle's colours (1, 2): none closed, about 12 pixels half open, 22 open. Drawn
// as a morph: each half of the bud (the right mirrors the left) row by row, its rows' spans
// stretched from one picture to the next by how open it is, each pixel the colour at the same
// place in the nearer picture. How open follows a smooth curve through each picture at the
// middle of its hold, its phase from the step from open to half open (seen once a cycle).
constexpr int kBudStages[] = {5, 6, 7};   // (stage 7's in its own colours)
constexpr int kBudFirst = 0x28D;
constexpr int kBudHolds[6] = {12, 4, 4, 4, 8, 8};
constexpr int kBudLevels[6] = {2, 1, 0, 1, 0, 1};
struct BudState {
    uint64_t hash = 0;
    int shows = -1;                                 // 0 closed, 1 half open, 2 open
    uint32_t changed = 0;
    int seen = 0;                                   // the pictures seen (a bit each)
    uint8_t pictures[3][16 * 16] = {};
    bool phased = false;
    uint32_t phase_at = 0;                          // the last step from open to half open
    float period = 40;
};
BudState g_bud;

// How open the bud is at `f` frames into the game's cycle (0: the open picture's start).
float bud_openness(double f)
{
    double centre[6];
    double t = 0;
    for (int i = 0; i < 6; i++)
        centre[i] = t + kBudHolds[i] / 2.0, t += kBudHolds[i];
    f = std::fmod(std::fmod(f, 40.0) + 40.0, 40.0);
    if (f < centre[0])
        f += 40;
    int i = 0;
    while (i < 5 && f >= centre[i + 1])
        i++;
    auto key = [&](int k, double& at) {
        const int j = ((k % 6) + 6) % 6;
        at = centre[j] + 40.0 * std::floor(k / 6.0);
        return (double)kBudLevels[j];
    };
    double a0, a1, a2, a3;
    const double p0 = key(i - 1, a0), p1 = key(i, a1), p2 = key(i + 1, a2), p3 = key(i + 2, a3);
    const double s = (f - a1) / (a2 - a1);
    const double v = 0.5 * (2 * p1 + (-p0 + p2) * s + (2 * p0 - 5 * p1 + 4 * p2 - p3) * s * s
                            + (-p0 + 3 * p1 - 3 * p2 + p3) * s * s * s);
    return (float)std::clamp(v, 0.0, 2.0);
}

// The morph (see above) into bud.look.
void bud_look(Vdp::Bud& bud)
{
    struct Spans { int top = 16, bottom = -1; int left[16]; };
    Spans sp[3];
    for (int p = 0; p < 3; p++)
        for (int y = 0; y < 16; y++) {
            sp[p].left[y] = 8;
            for (int x = 0; x < 8; x++)
                if (bud.pictures[p][y * 16 + x]) {
                    sp[p].left[y] = x;
                    break;
                }
            if (sp[p].left[y] < 8)
                sp[p].top = std::min(sp[p].top, y), sp[p].bottom = y;
        }
    const float o = std::clamp(bud.openness, 0.0f, 2.0f);
    const int a = o < 1 ? 0 : 1, b = a + 1;
    const float t = o - a;
    const Spans& A = sp[a];
    const Spans& B = sp[b];
    std::fill(std::begin(bud.look), std::end(bud.look), (uint8_t)0);
    if (A.bottom < 0 || B.bottom < 0)
        return;
    const float top = A.top + (B.top - A.top) * t;
    const float bottom = A.bottom + 1 + (B.bottom - A.bottom) * t;
    const int n = 16 * Vdp::kBudScale;
    for (int Y = 0; Y < n; Y++) {
        const float y = (Y + 0.5f) / Vdp::kBudScale;
        const float s = (y - top) / (bottom - top);
        if (s < 0 || s >= 1)
            continue;
        const int ra = std::min(A.bottom, (int)(A.top + s * (A.bottom + 1 - A.top)));
        const int rb = std::min(B.bottom, (int)(B.top + s * (B.bottom + 1 - B.top)));
        const int la = A.left[ra], lb = B.left[rb];
        const float left = la + (lb - la) * t;
        const bool near_a = t < 0.5f;
        const int row = near_a ? ra : rb, from = near_a ? la : lb;
        const uint8_t* pic = bud.pictures[near_a ? a : b];
        for (int X = 0; X < n; X++) {
            const float x = (X + 0.5f) / Vdp::kBudScale;
            const float hx = x < 8 ? x : 16 - x;    // (the right half mirrors the left)
            if (hx < left || hx >= 8)
                continue;
            const float u = (hx - left) / (8 - left);
            const int sx = std::min(7, (int)(from + u * (8 - from)));
            bud.look[Y * n + X] = pic[row * 16 + (x < 8 ? sx : 15 - sx)];
        }
    }
}

void update_buds(Vdp& vdp, const uint8_t* ram)
{
    BudState& st = g_bud;
    if (std::find(std::begin(kBudStages), std::end(kBudStages), (int)ram[0xFF43]) == std::end(kBudStages)) {
        st = BudState();
        return;
    }
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 4 * 32; i++)
        h = (h ^ vdp.vram[(kBudFirst * 32 + i) & 0xFFFF]) * 1099511628211ull;
    if (h != st.hash) {
        uint8_t pic[16 * 16];
        int middle = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                const int tile = kBudFirst + (y >> 3) * 2 + (x >> 3);
                const uint8_t pair = vdp.vram[(tile * 32 + (y & 7) * 4 + ((x & 7) >> 1)) & 0xFFFF];
                const int c = x & 1 ? pair & 15 : pair >> 4;
                pic[y * 16 + x] = (uint8_t)c;
                middle += c == 1 || c == 2;
            }
        const int shows = middle == 0 ? 0 : middle > 16 ? 2 : 1;
        if (st.shows == 2 && shows == 1) {
            if (st.phased)
                st.period = std::clamp((float)(g_frame - st.phase_at), 20.0f, 120.0f);
            st.phase_at = g_frame;
            st.phased = true;
        }
        std::copy(pic, pic + 256, st.pictures[shows]);
        st.seen |= 1 << shows;
        st.shows = shows;
        st.changed = g_frame;
        st.hash = h;
    }
    if (!st.phased || st.seen != 7)
        return;
    // Frames into the cycle (the step from open to half open is 12 in).
    auto cycle = [&](double frame) { return (frame - st.phase_at) * 40.0 / st.period + kBudHolds[0]; };
    const double f = cycle(g_frame);
    // Out of step with the game (its picture isn't the one the cycle says, a frame either way
    // allowed): the game's pictures until the next step from open.
    auto expected = [&](double at) {
        at = std::fmod(std::fmod(at, 40.0) + 40.0, 40.0);
        for (int i = 0; i < 6; i++) {
            if (at < kBudHolds[i])
                return kBudLevels[i];
            at -= kBudHolds[i];
        }
        return kBudLevels[0];
    };
    if (expected(f) != st.shows && expected(f - 1) != st.shows && expected(f + 1) != st.shows) {
        st.phased = false;
        return;
    }
    Vdp::Bud bud;
    bud.first = kBudFirst;
    bud.openness = bud_openness(f);
    bud.rate = bud.openness - bud_openness(cycle(g_frame - 1.0));
    for (int p = 0; p < 3; p++)
        std::copy(st.pictures[p], st.pictures[p] + 256, bud.pictures[p]);
    bud_look(bud);
    vdp.buds.push_back(bud);
}

// Stage 24's hanging chains: plane A tiles the game never moves ($168/$169 the links' left
// cells, $16A/$16B the right, rows alternating; the anchor $16C-$171, 4 x 2 cells under the
// last links). They swing when Mickey walks or jumps into one, as a damped pendulum about the
// top (a period of about 1.8 s for a 160-pixel chain, shorter ones faster), drawn bent: each
// row turned by the top's angle a little while ago (the lag growing down the chain, 14 frames
// at its foot), so the bottom trails and whips. Only chains whose top hangs from something (a
// drawn cell above it) count: the plane's table holds stale rows away from the screen.
constexpr int kChainStage = 23;
constexpr float kChainLag = 14;
struct ChainState {
    float angle = 0, speed = 0;
    float history[Vdp::kChainHistory] = {};
    uint32_t seen = 0;
    bool touching = false;                          // (Mickey against it the last frame)
};
std::unordered_map<uint64_t, ChainState> g_chains; // by the top's world x | y << 32
struct Pusher { int x = 0, y = 0; uint32_t seen = 0; };
Pusher g_pusher;

// Rows' angles (each the top's, its lag earlier, `back` of a frame further back) and middles.
void chain_rows(Vdp::Chain& c, float back)
{
    c.offset.assign((size_t)c.length, 0.0f);
    c.angle.assign((size_t)c.length, 0.0f);
    float across = 0;
    for (int r = 0; r < c.length; r++) {
        const float lag = std::clamp(r / (float)c.length * kChainLag + back, 0.0f, Vdp::kChainHistory - 1.001f);
        const int i = (int)lag;
        const float a = c.history[i] + (c.history[i + 1] - c.history[i]) * (lag - i);
        c.offset[(size_t)r] = across;
        c.angle[(size_t)r] = a;
        across += std::sin(a);
    }
}

void update_chains(Vdp& vdp, const uint8_t* ram)
{
    if (ram[0xFF43] != kChainStage) {
        g_chains.clear();
        return;
    }
    static const int kSizes[4] = {32, 64, 32, 128};
    const int w = kSizes[vdp.reg[16] & 3], h = kSizes[(vdp.reg[16] >> 4) & 3];
    auto rw = [&](int a) { return (int)(uint16_t)(ram[a & 0xFFFF] << 8 | ram[(a + 1) & 0xFFFF]); };
    // The whole stage's plane A from its layout (CameraA, $FFFA80: +8 the layout's address, +$C
    // its height and +$E its width in 16 x 16 blocks, +$10 the blocks' table: 4 tile words
    // each, across then down), so chains are found whole, their tops above the screen too (the
    // plane's table only holds the rows around the screen).
    const int layout = rw(0xFA8A), blocks = rw(0xFA92), lh = rw(0xFA8C), lw = rw(0xFA8E);
    auto tile = [&](int cx, int cy) {
        if (cx < 0 || cy < 0 || cx >= lw * 2 || cy >= lh * 2)
            return 0;
        const int b = ram[(layout + (cy >> 1) * lw + (cx >> 1)) & 0xFFFF];
        return rw(blocks + b * 8 + (cy & 1) * 4 + (cx & 1) * 2) & 0x7FF;
    };
    auto blank = [&](int t) {
        for (int i = 0; i < 32; i++)
            if (vdp.vram[(t * 32 + i) & 0xFFFF])
                return false;
        return true;
    };
    // A row of links: its left cell, and its right cell or something drawn over it (some hang
    // beside a wall that covers their right halves at the top).
    auto links = [&](int cx, int cy) {
        const int l = tile(cx, cy), r = tile(cx + 1, cy);
        return (l == 0x168 || l == 0x169)
               && (r == 0x16A || r == 0x16B || (!blank(r) && (r < 0x168 || r > 0x171)));
    };
    auto sw = [&](int a) { return (int)(int16_t)rw(a); };
    const int cam_x = sw(0xFA84), cam_y = sw(0xFA80);
    // Mickey: his box (feet at his X, Y) and how far he moved since the last frame.
    const int mx = sw(0xDC10), my = sw(0xDC14);
    const int vx = g_pusher.seen + 1 == g_frame ? std::clamp(mx - g_pusher.x, -8, 8) : 0;
    g_pusher = {mx, my, g_frame};
    const float kTwoPi = 6.2831853f;
    if (lw <= 0 || lh <= 0 || lw * lh > 0x8000)
        return;
    // The chains whose links reach the screen (widescreen's sides and a swing's reach too).
    const int x0 = std::max(0, (cam_x - 40 - 160) / 8), x1 = std::min(lw * 2 - 1, (cam_x + 360 + 160) / 8);
    const int y1 = std::min(lh * 2 - 1, (cam_y + 224) / 8);
    for (int cx = x0; cx <= x1; cx++)
        for (int cy = 0; cy <= y1; cy++) {
            if (!links(cx, cy) || links(cx, cy - 1))
                continue;
            const int above = tile(cx, cy - 1);
            if (blank(above) || (above >= 0x168 && above <= 0x171))
                continue;                           // (hanging from nothing)
            int n = 0;
            while (cy + n < lh * 2 && links(cx, cy + n))
                n++;
            if (n < 2 || (cy + n) * 8 < cam_y)
                continue;                           // (a single link; or all above the screen)
            const int anchor = n * 8;
            if (tile(cx, cy + n) == 0x16D && tile(cx - 1, cy + n) == 0x16C)
                n += 2;
            Vdp::Chain c;
            const int wx = cx * 8 + 8, wy = cy * 8;   // its top middle in the world
            c.x = wx % (w * 8), c.y = wy % (h * 8);   // (and in the plane's table)
            c.length = std::min(n * 8, 316), c.anchor = anchor;
            ChainState& s = g_chains[(uint64_t)(uint32_t)wx | (uint64_t)(uint32_t)wy << 32];
            if (s.seen + 1 != g_frame)
                s = ChainState();
            s.seen = g_frame;
            // A push: Mickey's box (16 across, 30 up from his feet) meeting the chain as drawn
            // while moving across: a swing his way, more the faster he goes. Once a touch: he
            // passes in front of it.
            bool touching = false;
            float across = 0;
            for (int r = 0; r < c.length && !touching; r++) {
                const float lag = std::min(r / (float)c.length * kChainLag, Vdp::kChainHistory - 1.001f);
                const float x = wx + across, y = (float)(wy + r);
                across += std::sin(s.history[(int)lag]);
                touching = y >= my - 30 && y <= my && std::fabs(x - mx) <= 8;
            }
            // (Its swing at most 0.08 radians per pixel a frame of his speed, whatever its length:
            // 9 degrees at a walk, 2 pixels a frame.)
            const float omega = kTwoPi / 110.0f * std::sqrt(160.0f / std::max(c.length, 24));
            if (touching && !s.touching)
                s.speed += std::clamp(vx, -6, 6) * 0.08f * omega;
            s.touching = touching;
            s.speed += -omega * omega * s.angle - 0.035f * s.speed;
            s.angle = std::clamp(s.angle + s.speed, -0.6f, 0.6f);
            std::memmove(s.history + 1, s.history, sizeof(float) * (Vdp::kChainHistory - 1));
            s.history[0] = s.angle;
            float most = 0;
            for (float a : s.history)
                most = std::max(most, std::fabs(a));
            if (most < 0.002f && std::fabs(s.speed) < 0.0005f) {
                s.angle = s.speed = 0;              // (at rest: the game's picture)
                std::fill(std::begin(s.history), std::end(s.history), 0.0f);
                continue;
            }
            std::copy(std::begin(s.history), std::end(s.history), c.history);
            c.speed = s.speed;
            chain_rows(c, 0);
            vdp.chains.push_back(std::move(c));
        }
    for (auto it = g_chains.begin(); it != g_chains.end();)
        it = g_frame - it->second.seen > 60 ? g_chains.erase(it) : std::next(it);
}

constexpr double kPi = 3.14159265358979323846;

// Stage 8 upside down: each turned picture mirrored top to bottom about screen line 96, as
// DrawObject mirrors the sprites: its rows reversed (each pixel names its tile pixel, so that
// mirrors the picture; toggling the tiles' flip as well flipped it back),
// its turning point and clip mask mirrored, and its angle (and rate, and a carried part's arm)
// reversed.
void flip_upside_down(Vdp& vdp)
{
    std::vector<bool> flipped(vdp.rotated_pictures.size(), false);
    auto flip = [&](int index) {
        if (index < 0 || (size_t)index >= vdp.rotated_pictures.size() || flipped[(size_t)index])
            return;
        flipped[(size_t)index] = true;
        Vdp::RotatedPicture& pic = vdp.rotated_pictures[(size_t)index];
        for (int y = 0; y < pic.h / 2; y++)
            std::swap_ranges(pic.words.begin() + (size_t)y * pic.w, pic.words.begin() + (size_t)(y + 1) * pic.w,
                             pic.words.begin() + (size_t)(pic.h - 1 - y) * pic.w);
        pic.pivot_y = pic.h - pic.pivot_y;
    };
    for (Vdp::Rotated& r : vdp.rotated) {
        flip(r.picture);
        if (r.clip >= 0) {
            const int h = vdp.rotated_pictures[(size_t)r.clip].h;
            flip(r.clip);
            r.clip_y = -(r.clip_y + h);
        }
        r.y = 192 - r.y;
        r.angle = -r.angle, r.rate = -r.rate;
        r.arm_y = -r.arm_y;
    }
}

// How far a picture reaches from its turning point (its farthest corner), plus a pixel.
int reach(const Vdp::RotatedPicture& pic)
{
    const float dx = std::max(pic.pivot_x, pic.w - pic.pivot_x), dy = std::max(pic.pivot_y, pic.h - pic.pivot_y);
    return (int)std::ceil(std::sqrt(dx * dx + dy * dy)) + 1;
}

// The pixels any frame of an animation draws (nonzero), in object coordinates from (*x0, *y0):
// the mask for Kind::clip_to_frames.
Vdp::RotatedPicture frames_mask(const Vdp& vdp, const uint8_t* rom, size_t rom_size, uint32_t anim, int frames,
                                uint16_t tile_base, int* x0, int* y0)
{
    auto rw = [&](uint32_t a) { return a + 1 < rom_size ? (int)(rom[a] << 8 | rom[a + 1]) : 0; };
    struct Piece { int x, y, w, h; uint16_t attr; };
    std::vector<Piece> all;
    int ax = 1 << 20, ay = 1 << 20, bx = -(1 << 20), by = -(1 << 20);
    for (int f = 0; f < frames && f < 16; f++) {
        const int offset = rw(anim + 2 + 2 * f);
        const uint32_t map = anim + (offset & 0x7FFF);
        const int pieces = map < rom_size && rom[map] < 0x80 ? rom[map] + 1 : 0;
        if (map + 6 + pieces * 5 > rom_size)
            continue;
        for (int k = 0; k < pieces && k < 16; k++) {
            const uint8_t* e = &rom[map + 6 + k * 5];
            Piece q;
            q.w = ((e[1] >> 2) & 3) + 1, q.h = (e[1] & 3) + 1, q.y = (int8_t)e[0], q.x = (int8_t)e[4];
            q.attr = (uint16_t)((e[2] << 8 | e[3]) + tile_base);
            if (offset & 0x8000)
                q.x = -(q.x + q.w * 8), q.attr ^= 0x0800;
            ax = std::min(ax, q.x), ay = std::min(ay, q.y), bx = std::max(bx, q.x + q.w * 8), by = std::max(by, q.y + q.h * 8);
            all.push_back(q);
        }
    }
    Vdp::RotatedPicture m;
    if (all.empty())
        return m;
    m.w = bx - ax, m.h = by - ay;
    m.words.assign((size_t)m.w * m.h, 0);
    for (const Piece& q : all)
        for (int py = 0; py < q.h * 8; py++)
            for (int px = 0; px < q.w * 8; px++) {
                const int col = q.attr & 0x0800 ? q.w * 8 - 1 - px : px, row = q.attr & 0x1000 ? q.h * 8 - 1 - py : py;
                const int tile = (q.attr & 0x7FF) + (col >> 3) * q.h + (row >> 3);
                const uint8_t pair = vdp.vram[(tile * 32 + (row & 7) * 4 + ((col & 7) >> 1)) & 0xFFFF];
                if ((col & 1 ? pair & 15 : pair >> 4) != 0)
                    m.words[(size_t)(q.y - ay + py) * m.w + (q.x - ax + px)] = 1;
            }
    *x0 = ax, *y0 = ay;
    return m;
}

// Parts carried by another (Vdp::Rotated::parent): placed at the end of their arm, turned with
// the parent.
void place_parts(std::vector<Vdp::Rotated>& rotated)
{
    for (Vdp::Rotated& r : rotated) {
        if (r.parent < 0 || r.parent >= (int)rotated.size())
            continue;
        const Vdp::Rotated& p = rotated[(size_t)r.parent];
        const float c = std::cos(p.angle), s = std::sin(p.angle);
        r.x = p.x + c * r.arm_x - s * r.arm_y;
        r.y = p.y + s * r.arm_x + c * r.arm_y;
        r.angle = p.angle * r.keep;
        r.rate = p.rate * r.keep;
    }
}

}  // namespace

void update(Vdp& vdp, const FrameInterpolator& interp, const uint8_t* ram, const uint8_t* rom, size_t rom_size, bool on)
{
    vdp.rotated.clear();
    vdp.rotated_pictures.clear();
    vdp.spinners.clear();
    vdp.gliders.clear();
    vdp.cyclers.clear();
    vdp.drums.clear();
    vdp.shutters.clear();
    vdp.buds.clear();
    vdp.chains.clear();
    if (!on) {
        g_states.clear();
        g_glide.clear();
        return;
    }
    g_frame++;
    update_spinners(vdp, ram);                      // (upside down too: stage 8's pinwheels)
    update_gliders(vdp, ram);
    update_cyclers(vdp, ram);
    update_drums(vdp, ram);
    update_shutters(vdp, ram);
    update_buds(vdp, ram);
    update_chains(vdp, ram);
    // $FFFF9801 (stage 8 upside down): DrawObject puts each sprite at 0xC0 - Y - its height on
    // screen and flips it top to bottom (sub_001C56), so the whole picture is mirrored about
    // screen line 96. The turned pictures are worked out as usual and mirrored the same way at
    // the end (`flip_upside_down`).
    const bool upside_down = ram[0x9801] != 0;
    auto rw = [&](uint32_t a) { return a + 1 < rom_size ? (int)(rom[a] << 8 | rom[a + 1]) : 0; };
    const auto& owners = interp.shown_owners();
    const bool h40 = vdp.reg[12] & 1;
    const int table = (vdp.reg[5] & (h40 ? 0x7E : 0x7F)) << 9;
    bool done[FrameInterpolator::kSprites] = {};
    for (int i = 0; i < FrameInterpolator::kSprites && vdp.rotated.size() < 8; i++) {   // (8: GpuVdp::kRotMax)
        const FrameInterpolator::Owner& o = owners[i];
        if (!o.valid || o.screen || done[i])
            continue;
        const Kind* kind = nullptr;
        for (const Kind& k : kKinds)
            if ((o.key & 0xFFFF) == k.id && (k.anim < 0 || k.anim == o.anim))
                kind = &k;
        if (!kind)
            continue;
        Vdp::Rotated r;
        for (int j = i; j < FrameInterpolator::kSprites; j++)
            if (owners[j].valid && owners[j].key == o.key)
                r.replaces[j] = 1, done[j] = true;
        // The upright picture: frame 0 of the animation playing (ObjAnimScript at $06).
        const uint32_t object = o.key >> 16;
        const uint32_t script = (uint32_t)(ram[(object + 7) & 0xFFFF] << 16 | ram[(object + 8) & 0xFFFF] << 8
                                           | ram[(object + 9) & 0xFFFF]);
        const uint32_t anim = script + rw(script + o.anim);
        if (anim + 4 >= rom_size)
            continue;
        const int frames = rom[anim], shows = rom[anim + 1] + 1;
        const int offset = rw(anim + 2 + 2 * std::min(kind->upright, std::max(frames - 1, 0)));
        const bool mirror = offset & 0x8000;
        const uint32_t map = anim + (offset & 0x7FFF);
        const int pieces = map < rom_size && rom[map] < 0x80 ? rom[map] + 1 : 0;
        if (!pieces || frames <= 0 || map + 6 + pieces * 5 > rom_size)
            continue;
        struct Piece { int x, y, w, h; uint16_t attr; };
        Piece p[16];
        int n = 0, x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
        for (int k = 0; k < pieces && n < 16; k++) {
            const uint8_t* e = &rom[map + 6 + k * 5];
            Piece& q = p[n++];
            q.w = ((e[1] >> 2) & 3) + 1, q.h = (e[1] & 3) + 1;
            q.y = (int8_t)e[0];
            q.x = (int8_t)e[4];
            q.attr = (uint16_t)((e[2] << 8 | e[3]) + o.tile_base);
            if (mirror)                             // (DrawObject's mirrored frames)
                q.x = -(q.x + q.w * 8), q.attr ^= 0x0800;
            x0 = std::min(x0, q.x), y0 = std::min(y0, q.y);
            x1 = std::max(x1, q.x + q.w * 8), y1 = std::max(y1, q.y + q.h * 8);
        }
        Vdp::RotatedPicture pic;
        pic.w = x1 - x0, pic.h = y1 - y0;
        pic.pivot_x = pic.w * 0.5f, pic.pivot_y = pic.h * 0.5f;
        pic.words.assign((size_t)pic.w * pic.h, 0);
        for (int k = 0; k < n; k++)
            for (int py = 0; py < p[k].h * 8; py++)
                for (int px = 0; px < p[k].w * 8; px++)
                    pic.words[(size_t)(p[k].y - y0 + py) * pic.w + (p[k].x - x0 + px)]
                        = p[k].attr | (uint32_t)px << 16 | (uint32_t)py << 21 | (uint32_t)(p[k].w - 1) << 26
                          | (uint32_t)(p[k].h - 1) << 28 | 1u << 31;

        if (kind->unlit) {
            // The light left out: each lit pixel shows the one opposite it through the middle (or
            // the nearest unlit one), then the picture widened 2 pixels past its edge.
            auto colour = [&](uint32_t word) {
                if (!word)
                    return 0;
                const int w = ((word >> 26) & 3) + 1, h = ((word >> 28) & 3) + 1;
                int col = (word >> 16) & 31, row = (word >> 21) & 31;
                if (word & 0x0800) col = w * 8 - 1 - col;
                if (word & 0x1000) row = h * 8 - 1 - row;
                const int tile = (word & 0x7FF) + (col >> 3) * h + (row >> 3);
                const uint8_t pair = vdp.vram[(tile * 32 + (row & 7) * 4 + ((col & 7) >> 1)) & 0xFFFF];
                return col & 1 ? pair & 15 : pair >> 4;
            };
            std::vector<uint32_t> words = pic.words;
            auto plain = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= pic.w || y >= pic.h)
                    return false;
                const int c = colour(pic.words[(size_t)y * pic.w + x]);
                return c != 0 && !(kind->unlit >> c & 1);
            };
            for (int y = 0; y < pic.h; y++)
                for (int x = 0; x < pic.w; x++) {
                    const int c = colour(pic.words[(size_t)y * pic.w + x]);
                    if (!c || !(kind->unlit >> c & 1))
                        continue;
                    int bx = pic.w - 1 - x, by = pic.h - 1 - y;
                    if (!plain(bx, by)) {
                        int best = 1 << 30;
                        for (int v = 0; v < pic.h; v++)
                            for (int u = 0; u < pic.w; u++)
                                if (plain(u, v)) {
                                    const int d = (u - bx) * (u - bx) + (v - by) * (v - by);
                                    if (d < best)
                                        best = d, bx = u, by = v;
                                }
                        if (best == 1 << 30)
                            continue;
                    }
                    words[(size_t)y * pic.w + x] = pic.words[(size_t)by * pic.w + bx];
                }
            for (int pass = 0; pass < 2; pass++) {
                std::vector<uint32_t> wider = words;
                for (int y = 0; y < pic.h; y++)
                    for (int x = 0; x < pic.w; x++) {
                        if (words[(size_t)y * pic.w + x])
                            continue;
                        for (int k = 0; k < 4; k++) {
                            const int u = x + (k == 0) - (k == 1), v = y + (k == 2) - (k == 3);
                            if (u >= 0 && v >= 0 && u < pic.w && v < pic.h && words[(size_t)v * pic.w + u]) {
                                wider[(size_t)y * pic.w + x] = words[(size_t)v * pic.w + u];
                                break;
                            }
                        }
                    }
                words.swap(wider);
            }
            pic.words.swap(words);
        }

        State& s = g_states[o.key];
        const bool held = o.flags & 0x02;           // (ObjFlags bit 1: the animation held)
        if (kind->roll_radius > 0) {
            // A roll: the angle carries on with its travel across (clockwise moving right).
            const bool fresh = s.seen + 1 != g_frame;
            const int dx = fresh ? 0 : o.x - s.last_x;
            if (fresh)
                s.shown = 0;
            s.shown = std::fmod(s.shown + dx / kind->roll_radius + 2 * kPi, 2 * kPi);
            r.rate = (float)(dx / kind->roll_radius);
            // The game hops it 2 pixels up for one frame in four as it rolls (a rumble): drawn at
            // the lower of this frame's and the last one's height, so it stays on the ground (a
            // fall shows at once, a climb a frame late).
            s.steady_y = fresh ? o.y : std::max(o.y, s.last_y);
            s.last_x = o.x, s.last_y = o.y, s.anim = o.anim, s.frame = o.anim_frame, s.seen = g_frame, s.offset = 0, s.turns = 0;
        } else if (kind->swing != 0) {
            // A swing: the angle is a sine of the animation's progress (its frame and timer), so
            // it is smooth through the animation's end; `rate` is the last game frame's change.
            pic.pivot_x = pic.w * 0.5f, pic.pivot_y = kind->swing_pivot < 0 ? (float)pic.h : (float)kind->swing_pivot;
            const double t = held ? o.anim_frame : o.anim_frame + std::clamp(shows - 1 - o.anim_timer, 0, shows - 1) / (double)shows;
            auto angle = [&](double at) {
                const double v = std::sin(2 * kPi * (at + kind->swing_phase) / frames);
                return -kind->swing * kPi / 180 * std::copysign(std::pow(std::fabs(v), 0.6), v);
            };
            s.shown = angle(t);
            r.rate = held ? 0.0f : (float)(s.shown - angle(t - 1.0 / shows));
            s.anim = o.anim, s.frame = o.anim_frame, s.seen = g_frame, s.offset = 0, s.turns = 0;
        } else {
            // The angle, along the game's animation.
            const double step = 2 * kPi / kind->steps;
            r.rate = held ? 0.0f : (float)(step / shows);
            // The game's own angle: its frame, plus the part of it shown so far (ObjAnimTimer counts
            // down from shows - 1).
            // Counted on past the animation's last frame (s.turns), as its frames may make less than a
            // whole turn (the gears: 3 frames of 15 degrees, their teeth repeating every 45).
            const int way = kind->follows_x && o.vel_x < 0 ? -1 : 1;
            const bool fresh = s.seen + 1 != g_frame || held;
            const bool jumped = !fresh && (o.anim != s.anim || way != s.way
                                           || (o.anim_frame != s.frame && o.anim_frame != (s.frame + 1) % frames));
            if (fresh || jumped)
                s.turns = 0;
            else if (o.anim_frame < s.frame)
                s.turns++;
            double game = (s.turns * frames + o.anim_frame) * step
                          + (held ? 0 : std::clamp(shows - 1 - o.anim_timer, 0, shows - 1) * (double)r.rate);
            game = std::fmod(game, 2 * kPi) * way;
            r.rate *= (float)way;
            if (fresh) {
                s.offset = 0;                           // new on screen, or standing still
            } else if (jumped) {
                // Another animation (or a frame skipped): carry on from the angle shown, and fade
                // the difference out over the next frames.
                s.offset = std::remainder(s.shown + r.rate - game, 2 * kPi);
            } else {
                s.offset *= 0.9;
            }
            s.anim = o.anim, s.frame = o.anim_frame, s.seen = g_frame, s.way = way;
            s.shown = std::fmod(game + s.offset + 2 * kPi, 2 * kPi);
        }

        r.angle = (float)s.shown;
        r.x = (float)(o.x - o.camera_x + x0) + pic.pivot_x;
        r.y = (float)((kind->roll_radius > 0 ? s.steady_y : o.y) - o.camera_y + y0) + pic.pivot_y;
        r.radius = reach(pic);
        // The first of its sprites in the link order.
        int link = 0;
        for (int k = 0; k < 80; k++) {
            if (r.replaces[link]) {
                r.first = link;
                break;
            }
            link = vdp.vram[(table + link * 8 + 3) & 0xFFFF] & 0x7F;
            if (link == 0 || link >= 80)
                break;
        }
        if (r.first < 0)
            continue;
        if (kind->swing != 0 && kind->body_row > 0 && vdp.rotated.size() + 2 <= 8) {
            // Head and body: the head first (drawn in front), carried by the body after it.
            Vdp::RotatedPicture head = pic;
            for (int y = 0; y < pic.h; y++)
                for (int x = 0; x < pic.w; x++) {
                    const bool body = y >= kind->body_row || (y >= kind->body_row2 && x >= kind->body_col);
                    (body ? head : pic).words[(size_t)y * pic.w + x] = 0;
                }
            head.pivot_y = (float)kind->body_row2;
            Vdp::Rotated h = r;
            h.parent = (int)vdp.rotated.size() + 1;
            h.arm_x = 0, h.arm_y = head.pivot_y - pic.pivot_y;
            h.keep = 0;
            h.radius = reach(head);
            h.picture = (int)vdp.rotated_pictures.size();
            vdp.rotated_pictures.push_back(std::move(head));
            vdp.rotated.push_back(h);
        }
        if (kind->clip_to_frames) {
            int mx = 0, my = 0;
            Vdp::RotatedPicture mask = frames_mask(vdp, rom, rom_size, anim, frames, o.tile_base, &mx, &my);
            if (mask.w > 0) {
                r.clip = (int)vdp.rotated_pictures.size();
                r.clip_x = (float)mx - (x0 + pic.pivot_x), r.clip_y = (float)my - (y0 + pic.pivot_y);
                vdp.rotated_pictures.push_back(std::move(mask));
            }
        }
        r.picture = (int)vdp.rotated_pictures.size();
        vdp.rotated_pictures.push_back(std::move(pic));
        vdp.rotated.push_back(r);
    }
    if (upside_down)
        flip_upside_down(vdp);
    place_parts(vdp.rotated);
    for (auto it = g_states.begin(); it != g_states.end();)
        it = it->second.seen == g_frame ? std::next(it) : g_states.erase(it);
}

void between(Vdp& v, const Vdp& now, double back)
{
    v.rotated = now.rotated;
    v.rotated_pictures = now.rotated_pictures;
    for (Vdp::Rotated& r : v.rotated)
        r.angle -= (float)(back * r.rate);
    place_parts(v.rotated);
    // Spinners: where the turn was `back` of a frame earlier (before the last rewrite, the new
    // picture turned back towards the old one's place).
    v.drums = now.drums;                            // drums: their turn then
    v.drum_look = now.drum_look, v.drum_look_w = now.drum_look_w, v.drum_look_h = now.drum_look_h;
    for (Vdp::Drum& d : v.drums)
        d.angle = d.start * (1.0f - std::min(1.0f, std::max(0.0f, d.elapsed - (float)back) / d.interval));
    v.cyclers = now.cyclers;                        // colour-cycled wheels: their turn then
    for (Vdp::Cycler& c : v.cyclers)
        c.angle = c.start * (1.0f - std::min(1.0f, std::max(0.0f, c.elapsed - (float)back) / c.interval));
    v.chains = now.chains;                          // chains: their swing then
    for (Vdp::Chain& c : v.chains)
        chain_rows(c, (float)back);
    v.buds = now.buds;                              // buds: how open then
    for (Vdp::Bud& b : v.buds) {
        b.openness -= (float)(back * b.rate);
        bud_look(b);
    }
    v.shutters = now.shutters;                      // shutters: their swing then
    for (Vdp::Shutter& s : v.shutters)
        s.angle = shutter_angle(s, std::max(0.0f, s.elapsed - (float)back));
    v.gliders = now.gliders;                        // gliders: where the bob was then
    for (Vdp::Glider& g : v.gliders)
        g.offset -= (float)(back * g.rate);
    v.spinners = now.spinners;
    for (Vdp::Spinner& sp : v.spinners)
        sp.angle = sp.start * (1.0f - std::min(1.0f, std::max(0.0f, sp.elapsed - (float)back) / sp.interval)) + sp.offset;
}

}  // namespace smooth_animation
