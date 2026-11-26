// Boss Rush: a BOSS RUSH tick box on the game's options screen.
//
// The options screen (GameState $08 draws it, $0A runs it) is laid out from 12-byte pieces in
// the table at $00A7B8 (source, VDP command, width - 1, height - 1), each placed by its own
// VDP command, and its cursor (Mickey on the swing, object $C4) moves between three rows
// with fixed swing heights. To fit a fourth row, every gap closes to one row: the Difficulty
// sign moves up a row, Sound Test two, Control three and the exit line down one, with the
// values the cursor redraws (difficulty name, sound number, control lines) moved to match.
// BOSS RUSH goes in at row 24 with a 3 x 3 sign built from the Difficulty sign's ends, an
// X (font tile $31) in it when ticked.
//
// The new code (boss_rush.asm, assembled into boss_rush_code.h) lives at $0D0000, past the end
// of the 512 KB ROM and clear of smooth swinging's $0C0000. The cursor's state dispatch goes
// through it: row 2 can now move down, state 7 is row 3, and B/C there tick or untick (the
// presses that change the other rows' values). The swing's chain grows to 21 links for row 3,
// so the two highlight objects that sat in slots $FFD900-$FFD9BF move to $FFDA00-$FFDABF.

#include "boss_rush.h"

#include <algorithm>

#include <cstdio>

#include "boss_rush_code.h"

namespace {

struct Patch {
    uint32_t address;
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
    const char* what;
};

constexpr uint32_t kDrawOptions = kBossRushOrigin, kCursorMain = kBossRushOrigin + 6,
                   kSetRowState = kBossRushOrigin + 12, kRedraw = kBossRushOrigin + 18,
                   kStartGame = kBossRushOrigin + 24, kLevelStart = kBossRushOrigin + 30,
                   kRushFrame = kBossRushOrigin + 36, kTryLimit = kBossRushOrigin + 42,
                   kMizrabelHit = kBossRushOrigin + 48, kResultsStart = kBossRushOrigin + 54,
                   kResultsFrame = kBossRushOrigin + 60, kMizrabelFade = kBossRushOrigin + 66,
                   kHudFrame = kBossRushOrigin + 72, kHudLevelStart = kBossRushOrigin + 78;
// Hardcore (hardcore.asm, at its own address past Boss Rush's code).
constexpr uint32_t kHardcore = 0x0D1000, kHardcoreRow = kHardcore, kHardcoreRedraw = kHardcore + 6,
                   kHardcoreStart = kHardcore + 12, kHardcoreLevel = kHardcore + 18, kHardcoreTryItem = kHardcore + 24,
                   kOptionsWrap = kHardcore + 30;
// Co-op (coop.asm, at its own address past Hardcore's).
constexpr uint32_t kCoopObjects = 0x0D2000, kCoopCameraTarget = 0x0D2006, kCoopTouchHurts = 0x0D200C,
                   kCoopTouchHurts2 = 0x0D2012, kCoopBouncedOn = 0x0D2018, kCoopTouches = 0x0D201E,
                   kCoopScrolling = 0x0D2024, kCoopVIntUpload = 0x0D202A,
                   kCoopTouchesSlot = 0x0D2030, kCoopAfterObject = 0x0D2036,
                   kCoopVineHeld = 0x0D203C,
                   kCoopHurts2Replay = 0x0D2042,
                   kCoopStandClear = 0x0D204E, kCoopStandSet = 0x0D2054,
                   kCoopBossPush = 0x0D205A, kCoopBossArm27 = 0x0D2060, kCoopBossBlockR = 0x0D2066,
                   kCoopBossBlockL = 0x0D206C, kCoopPlayerEntry = 0x0D2072, kCoopDieStart = 0x0D2078,
                   kCoopDyingTick = 0x0D207E, kCoopDeathEnd = 0x0D2084, kCoopDrawHud = 0x0D208A,
                   kCoopLevelInit = 0x0D2090, kCoopPlaySound = 0x0D2096, kCoopPlaySoundMd = 0x0D209C,
                   kCoopVineGrab1 = 0x0D20A2, kCoopBoulderR = 0x0D20A8, kCoopBoulderL = 0x0D20AE,
                   kCoopFinaleCheck = 0x0D20B4, kCoopFinaleX = 0x0D20BA, kCoopFinaleArrive = 0x0D20C0,
                   kCoopFinaleWaiting = 0x0D20C6, kCoopOrb = 0x0D20CC, kCoopOrbBurst = 0x0D20D2,
                   kCoopDrawFlip = 0x0D20D8, kCoopOrbsGone = 0x0D20DE,
                   kCoopOrbTiles = 0x0D20E4, kCoopSparkleD1 = 0x0D20EA, kCoopExitPower = 0x0D20EA,
                   kCoopSoundRoutine = 0x0D7FFC;
// Co-op's art, made from the ROM when the co-op image is built (coop.asm's CoopArt): the orb
// that traps Minnie (object $EA, stage 28; Nemesis-compressed at $05177C, 28 tiles), its
// colours (palette line 3 there: white and cyans) moved to palette line 0's white and blues,
// which every stage shares.
constexpr uint32_t kCoopArt = 0x0D8000, kOrbArt = 0x05177C;
constexpr uint32_t kFinaleArt = 0x0D8400, kFinaleSet = 0x06D010;  // graphics set $2B (VRAM $B000)
constexpr int kFinaleTiles = 49;                    // its first: the finale's orb and burst
constexpr int kOrbTiles = 28;

std::vector<uint8_t> jsr(uint32_t a) { return {0x4E, 0xB9, uint8_t(a >> 24), uint8_t(a >> 16), uint8_t(a >> 8), uint8_t(a)}; }
std::vector<uint8_t> jmp(uint32_t a) { return {0x4E, 0xF9, uint8_t(a >> 24), uint8_t(a >> 16), uint8_t(a >> 8), uint8_t(a)}; }

std::vector<uint8_t> operator+(std::vector<uint8_t> a, const std::vector<uint8_t>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

const std::vector<uint8_t> kNop = {0x4E, 0x71};

// The VDP command that writes plane A ($C000, 64 cells across) at a cell.
std::vector<uint8_t> cell(int row, int col)
{
    const uint32_t a = 0xC000 + (row * 64 + col) * 2;
    const uint32_t cmd = 0x40000000 | (a & 0x3FFF) << 16 | a >> 14;
    return {uint8_t(cmd >> 24), uint8_t(cmd >> 16), uint8_t(cmd >> 8), uint8_t(cmd)};
}

// A layout piece's VDP command (index 1-$21 in the table at $00A7B8).
uint32_t piece(int index) { return 0xA7B8 + (index - 1) * 12 + 4; }

const std::vector<Patch>& patches()
{
    static const std::vector<Patch> list = {
        // Layout pieces: signs, labels, the exit line and the difficulty names.
        {piece(0x01), cell(2, 21), cell(1, 21), "Difficulty sign"},
        {piece(0x02), cell(9, 21), cell(7, 21), "Sound Test sign"},
        {piece(0x19), cell(16, 21), cell(13, 21), "Control sign"},
        {piece(0x1A), cell(3, 5), cell(2, 5), "DIFFICULTY"},
        {piece(0x1B), cell(10, 5), cell(8, 5), "SOUND TEST"},
        {piece(0x1C), cell(19, 5), cell(16, 5), "CONTROL"},
        {piece(0x1D), cell(26, 7), cell(27, 7), "PRESS START BUTTON - EXIT"},
        {piece(0x15), cell(4, 22), cell(3, 22), "NORMAL"},
        {piece(0x16), cell(4, 22), cell(3, 22), "HARD"},
        {piece(0x17), cell(4, 22), cell(3, 22), "PRACTICE"},
        // Values the cursor writes straight to the VDP.
        {0x00441E, cell(11, 22), cell(9, 22), "sound number"},
        {0x004456, cell(18, 22), cell(15, 22), "control line 1"},
        {0x004476, cell(20, 22), cell(17, 22), "control line 2"},
        {0x00448A, cell(22, 22), cell(19, 22), "control line 3"},

        // GameState_08: draw BOSS RUSH and its sign after the labels.
        {0x009EEE, {0x20, 0x3C, 0x1D, 0x1C, 0x1B, 0x1A, 0x4E, 0xBA, 0x08, 0x6C}, jsr(kDrawOptions) + kNop + kNop,
         "options labels"},
        // The cursor: state dispatch, where it settles on a row, and the value redraw.
        {0x003FC2, {0x43, 0xFA, 0x00, 0x06, 0x4E, 0xFA, 0xDC, 0xAC}, jmp(kOptionsWrap) + kNop, "cursor states"},   // (wraps the top and bottom rows, then kCursorMain)
        {0x004158, {0x11, 0x78, 0xF4, 0x1E, 0x00, 0x03, 0x52, 0x28, 0x00, 0x03}, jsr(kSetRowState) + kNop + kNop,
         "cursor settles (up)"},
        {0x0041A8, {0x11, 0x78, 0xF4, 0x1E, 0x00, 0x03, 0x52, 0x28, 0x00, 0x03}, jsr(kSetRowState) + kNop + kNop,
         "cursor settles (down)"},
        {0x0041D2, {0x11, 0x78, 0xF4, 0x1E, 0x00, 0x03, 0x52, 0x28, 0x00, 0x03}, jsr(kSetRowState) + kNop + kNop,
         "cursor settles (after a change)"},
        // Hardcore: the difficulty row's B and C go round NORMAL, HARD, HARDCORE, PRACTICE.
        {0x00430A, {0x10, 0x38, 0xFF, 0x0D, 0x08, 0x05, 0x00, 0x04}, jmp(kHardcoreRow) + kNop, "difficulty row"},
        {0x0043EC, {0x10, 0x38, 0xFF, 0x0D, 0x0C, 0x00, 0x00, 0x01}, jmp(kHardcoreRedraw) + kNop, "value redraw"},
        // The rush: start at the first boss, one try at each level start, the clock and the
        // jump to the next boss every frame of play, and no extra-try items.
        {0x009FCC, {0x61, 0x22, 0x11, 0xF8, 0xFF, 0x0D, 0xFF, 0x0F}, jsr(kHardcoreStart) + kNop, "game start"},
        {0x009618, {0x4E, 0xBA, 0x00, 0xB2, 0x4E, 0xBA, 0x02, 0xD4}, jsr(kHardcoreLevel) + kNop, "level start"},
        {0x009808, {0x4E, 0xBA, 0x00, 0xD0, 0x61, 0x26}, jsr(kRushFrame), "frame of play"},
        {0x000C1C, {0x0C, 0x78, 0x00, 0x0A, 0xF3, 0x24}, jsr(kHardcoreTryItem), "extra-try item"},
        {0x009812, {0x4E, 0xBA, 0x04, 0x2E, 0x4E, 0xFA, 0x03, 0x32}, jmp(kHudFrame) + kNop, "score and HUD"},
        {0x009680, {0x4E, 0xF9, 0x00, 0x07, 0xC3, 0xD8}, jmp(kHudLevelStart), "level start's end"},
        // Mizrabel's last hit stops her clock; the ending's two GameStates show the results
        // instead in a Boss Rush.
        {0x010890, {0x11, 0xFC, 0x00, 0x9B, 0xF4, 0x0E}, jsr(kMizrabelHit), "Mizrabel's last hit"},
        {0x010E48, {0x70, 0x01, 0x4E, 0xB9, 0x00, 0x00, 0xA5, 0xC0}, jsr(kMizrabelFade) + kNop, "fade to white"},
        {0x00A110, {0x33, 0xF8, 0xFF, 0x08, 0x00, 0xC0, 0x00, 0x04}, jmp(kResultsStart) + kNop, "ending start"},
        {0x00A158, {0x31, 0xFC, 0x00, 0x04, 0xF3, 0x02}, jmp(kResultsFrame), "ending frame"},
        // Swing heights: start, row 0 down, row 1 up and down (row 2 is in the new code).
        {0x003FF4, {0x00, 0x38}, {0x00, 0x30}, "cursor start height"},
        {0x0040A0, {0x00, 0x70}, {0x00, 0x60}, "row 0 -> 1 height"},
        {0x0040C2, {0x00, 0x38}, {0x00, 0x30}, "row 1 -> 0 height"},
        {0x0040D8, {0x00, 0xB8}, {0x00, 0xA0}, "row 1 -> 2 height"},
        // The highlight objects: out of the longer chain's way, and moved with their rows.
        {0x004034, {0xD9, 0x00}, {0xDA, 0x00}, "highlight 1 slot"},
        {0x00403C, {0xD9, 0x10}, {0xDA, 0x10}, "highlight 1 X"},
        {0x004040, {0x00, 0x2C, 0x00, 0xFF, 0xD9, 0x14}, {0x00, 0x24, 0x00, 0xFF, 0xDA, 0x14}, "highlight 1 Y"},
        {0x00404C, {0xD9, 0x40}, {0xDA, 0x40}, "highlight 2 slot"},
        {0x004054, {0xD9, 0x50}, {0xDA, 0x50}, "highlight 2 X"},
        {0x004058, {0x00, 0x64, 0x00, 0xFF, 0xD9, 0x54}, {0x00, 0x54, 0x00, 0xFF, 0xDA, 0x54}, "highlight 2 Y"},
        {0x004064, {0xD9, 0x69}, {0xDA, 0x69}, "highlight 2 row"},
        {0x00406C, {0xD9, 0x80}, {0xDA, 0x80}, "highlight 3 slot"},
        {0x004074, {0xD9, 0x90}, {0xDA, 0x90}, "highlight 3 X"},
        {0x004078, {0x00, 0xBC, 0x00, 0xFF, 0xD9, 0x94}, {0x00, 0xA4, 0x00, 0xFF, 0xDA, 0x94}, "highlight 3 Y"},
    };
    return list;
}

// Co-op's patches (coop.asm): only in the image used while Player 2 has joined, so a game
// without Player 2 runs exactly the code it ran before.
// Stage 27's boss: cmpi.w #$1D0,($FFFFDC14).w; blt.s; move.b ($FFFFDC03).w,d0; cmpi.b #$13,d0; blt.s; cmpi.b #$1C,d0; bgt.s
const std::vector<uint8_t> kBossBlockBytes = {0x0C, 0x78, 0x01, 0xD0, 0xDC, 0x14, 0x6D, 0x10, 0x10, 0x38, 0xDC, 0x03,
                                              0x0C, 0x00, 0x00, 0x13, 0x6D, 0x34, 0x0C, 0x00, 0x00, 0x1C, 0x6E, 0x2E};
// Stage 13's boss: move.l ($FFFFDC10).w,d0; cmp.l $10(a0),d0; bcs.s +6; move.w #$FFF0,($FFFFDC18).w
const std::vector<uint8_t> kBossPushBytes = {0x20, 0x38, 0xDC, 0x10, 0xB0, 0xA8, 0x00, 0x10,
                                             0x65, 0x06, 0x31, 0xFC, 0xFF, 0xF0, 0xDC, 0x18};

const std::vector<Patch>& coop_patches()
{
    static const std::vector<Patch> list = {
        // Co-op: Player 2 runs after Mickey in RunAllObjects (only while one has joined).
        {0x0018E0, {0x41, 0xF8, 0xDC, 0x40, 0x7E, 0x0E}, jsr(kCoopObjects), "objects after Mickey"},
        {0x00F76E, {0x30, 0x28, 0x00, 0x10, 0x32, 0x28, 0x00, 0x14}, jsr(kCoopCameraTarget) + kNop, "camera target"},
        {0x00F6E4, {0x4E, 0xBA, 0x00, 0x34, 0x4E, 0xBA, 0x00, 0xBA}, jmp(kCoopScrolling) + kNop, "scrolling"},
        {0x00D486, {0x08, 0xB8, 0x00, 0x00, 0xF4, 0x04}, jmp(kCoopVIntUpload), "frame upload"},
        // Co-op: objects' tests against Mickey also try Player 2.
        {0x0092D2, {0x7E, 0x00, 0x4A, 0x38, 0xF4, 0x46}, jmp(kCoopTouchHurts), "TouchHurtsMickey"},
        {0x0092B2, {0x7E, 0x00, 0x4A, 0x38, 0xF4, 0x46}, jmp(kCoopTouchHurts2), "TouchHurtsMickey2"},
        {0x009318, {0x4A, 0x38, 0xF4, 0x0B, 0x67, 0x3E}, jmp(kCoopBouncedOn), "BouncedOn"},
        // Objects that touch Player 2 only have him in Mickey's place for the rest of their code.
        {0x009264, {0x61, 0x04, 0x4E, 0xFA, 0x00, 0xF6}, jmp(kCoopTouches), "TouchesMickey"},
        {0x009258, {0x43, 0xF8, 0xDC, 0x00, 0x61, 0x0C}, jmp(kCoopTouchesSlot), "TouchesMickeySlot"},
        {0x0018F4, {0x61, 0x0E, 0x4C, 0xDF, 0x01, 0x80}, jsr(kCoopAfterObject), "each object"},
        // Co-op: the bosses that face or go toward the player read his X from CoopTargetX
        // ($FFFD50: Mickey's or Player 2's, picked at random). Left alone: the reads before
        // a push of Mickey (they test his X, then set his speed).
        {0x011082, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $EB"},
        {0x0119B8, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $DD"},
        {0x01236C, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $D2"},
        {0x012510, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $D2 (toward)"},
        {0x01292C, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $D4"},
        {0x013960, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $C8"},
        {0x013A9E, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $C8"},
        {0x013D58, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $C8"},
        {0x013EBE, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $C8"},
        {0x013FBC, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $C8"},
        {0x014032, {0xDC, 0x10}, {0xFD, 0x50}, "boss target $C8 (jump)"},
        // Co-op: stage 13's boss pushes back both players (CoopBossPush).
        {0x0113E0, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x01140A, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x01145A, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x0114B2, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x01150A, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x011554, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x0115B0, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x0115F4, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x0116D0, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x0116E6, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x01170E, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x01173A, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        {0x011786, kBossPushBytes, jsr(kCoopBossPush) + kNop + kNop + kNop + kNop + kNop, "stage 13 push"},
        // Co-op: stage 27's boss raises its arm when a player jumps in its reach (CoopBossBlockL/R).
        {0x0139C8, kBossBlockBytes, jsr(kCoopBossBlockL) + std::vector<uint8_t>{0x67, 0x00, 0x00, 0x3E, 0x60, 0x00, 0x00, 0x0C} + kNop + kNop + kNop + kNop + kNop, "stage 27 block (left)"},
        {0x013B06, kBossBlockBytes, jsr(kCoopBossBlockR) + std::vector<uint8_t>{0x67, 0x00, 0x00, 0x3E, 0x60, 0x00, 0x00, 0x0C} + kNop + kNop + kNop + kNop + kNop, "stage 27 block (right)"},
        // Co-op: stage 27's raised arm reacts to each player as the original does to Mickey (CoopBossArm27).
        {0x014464, {0x4E, 0xB9, 0x00, 0x00, 0x92, 0xD2}, jmp(kCoopBossArm27), "stage 27 arm"},
        // Co-op: each player's own deaths, bubbles and tries (coop.asm, CoopPlayerEntry on).
        {0x00AEA2, {0x4E, 0xBA, 0x1F, 0x98, 0x53, 0x78, 0xF3, 0x16}, jsr(kCoopPlayerEntry) + kNop, "Mickey's code"},
        {0x00AEFE, {0x31, 0xFC, 0x00, 0x50, 0xF3, 0x46}, jsr(kCoopDieStart), "death start"},
        {0x00B192, {0x53, 0x78, 0xF3, 0x46, 0x67, 0x12}, jsr(kCoopDyingTick), "dying"},
        {0x00B1AA, {0x4A, 0x78, 0xF3, 0x24, 0x66, 0x06}, jmp(kCoopDeathEnd), "death end"},
        {0x009B4A, {0x45, 0xF9, 0x00, 0xC0, 0x00, 0x04}, jmp(kCoopDrawHud), "HUD"},
        {0x009652, {0x31, 0xFC, 0x00, 0x01, 0xDC, 0x00}, jsr(kCoopLevelInit), "level start"},
        // Co-op: each player marks the solid object he stands on with his own bit.
        {0x00C142, {0x08, 0xA9, 0x00, 0x00, 0x00, 0x0E}, jsr(kCoopStandClear), "stood on: clear"},
        {0x00C200, {0x08, 0xE9, 0x00, 0x00, 0x00, 0x0E}, jsr(kCoopStandSet), "stood on: set"},
        {0x00C33C, {0x08, 0xE9, 0x00, 0x00, 0x00, 0x0E}, jsr(kCoopStandSet), "stood on: set (2)"},
        // Co-op: no fireworks by a boss's gem (object $95, sub_001040): they draw from tiles $580
        // on (graphics set $2B, loaded at $B000 at every level start, also object $6C's in the
        // finale), where Player 2's frame and the bubble's tiles go; it drew Player 2's frame.
        {0x001040, {0x43, 0xF8, 0xDD, 0x80}, {0x4E, 0x75, 0x4E, 0x71}, "gem fireworks"},
        // Co-op: each player's own wait after letting go of a vine, for the next one's grab.
        {0x000AB2, {0x4A, 0x38, 0xF4, 0x48, 0x66, 0x2C}, jsr(kCoopVineGrab1), "vine grab"},
        // Co-op: both players can hold the same vine or rope.
        {0x000AD0, {0x61, 0x32, 0x4A, 0x38, 0xF4, 0x48}, jmp(kCoopVineHeld), "vine held"},
        // Co-op: stage 24's boulders roll back once beyond both players in play (the range
        // word after the call stays: $B8, or $E0 in the widened view).
        {0x005B28, {0x30, 0x38, 0xDC, 0x10, 0x06, 0x40}, jsr(kCoopBoulderR), "boulder: right turn"},
        {0x005B82, {0x30, 0x38, 0xDC, 0x10, 0x04, 0x40}, jsr(kCoopBoulderL), "boulder: left turn"},
        // Co-op: the final boss's script after the last hit, for both players (coop.asm).
        {0x00B494, {0x08, 0xB8, 0x00, 0x00, 0xF4, 0x5A}, jsr(kCoopFinaleCheck), "finale: start"},
        {0x00B412, {0x0C, 0x68, 0x00, 0xF0, 0x00, 0x10}, jsr(kCoopFinaleX), "finale: at his place"},
        {0x00B43A, {0x0C, 0x68, 0x00, 0xF0, 0x00, 0x10}, jsr(kCoopFinaleX), "finale: which way"},
        {0x00B4C8, {0x0C, 0x68, 0x00, 0xF0, 0x00, 0x10}, jsr(kCoopFinaleX), "finale: facing"},
        {0x00B422, {0x11, 0xFC, 0x00, 0x01, 0xF4, 0x20}, jsr(kCoopFinaleArrive), "finale: in place"},
        {0x00B460, {0x4A, 0x38, 0xF4, 0x20, 0x66, 0x12}, jsr(kCoopFinaleWaiting), "finale: waiting"},
        {0x00CDCA, {0x43, 0xF8, 0xDC, 0x40, 0x70, 0x06}, jsr(kCoopOrb), "finale: orb"},
        {0x00CDB0, {0x43, 0xF8, 0xDC, 0x80, 0x70, 0x08}, jsr(kCoopOrbBurst), "finale: burst"},
        {0x001B42, {0x08, 0x80, 0x00, 0x0F, 0xD2, 0xC0}, jsr(kCoopDrawFlip), "finale: Player 2 faces left"},
        {0x0109A8, {0x00, 0x38, 0x00, 0x01, 0xDC, 0x82}, jsr(kCoopOrbsGone), "finale: orbs gone"},
        {0x00CDF4, {0x33, 0x7C, 0xA5, 0x80, 0x00, 0x04}, jsr(kCoopOrbTiles), "finale: orbs' tiles"},
        {0x010AD8, {0x33, 0x7C, 0x00, 0xFC, 0x00, 0x10}, jmp(kCoopSparkleD1), "finale: sparkles from each orb"},
        {0x0006DC, {0x4A, 0x78, 0xF3, 0x22, 0x67, 0x40}, jsr(kCoopExitPower), "exit: either player's power"},
    };
    return list;
}

}  // namespace

// Nemesis decompression (the game's NemDec, $07FBAE): the tiles at `src`, 32 bytes each, or
// nothing if the data runs past the ROM or holds a code the table doesn't.
std::vector<uint8_t> nemesis_decode(const std::vector<uint8_t>& rom, size_t src)
{
    std::vector<uint8_t> out;
    if (src + 3 > rom.size())
        return out;
    const unsigned header = rom[src] << 8 | rom[src + 1];
    const bool xor_mode = header & 0x8000;
    const unsigned rows = (header & 0x7FFF) * 8;
    size_t p = src + 2;
    // The code table: a colour, then (count and length, code) pairs, until the next colour
    // (a byte of $80 or more) or $FF.
    struct Code { unsigned length, code, count, colour; };
    std::vector<Code> codes;
    unsigned byte = rom[p++];
    while (byte != 0xFF) {
        const unsigned colour = byte & 0x0F;
        if (p >= rom.size())
            return {};
        byte = rom[p++];
        while (byte < 0x80) {
            if (p + 1 >= rom.size())
                return {};
            codes.push_back({byte & 0x0F, rom[p++], ((byte >> 4) & 7) + 1, colour});
            byte = rom[p++];
        }
    }
    size_t bit = p * 8;
    auto peek = [&](unsigned n) -> int {
        unsigned v = 0;
        for (unsigned i = 0; i < n; i++) {
            const size_t b = bit + i;
            if (b / 8 >= rom.size())
                return -1;
            v = v << 1 | ((rom[b / 8] >> (7 - b % 8)) & 1);
        }
        return (int)v;
    };
    uint32_t row = 0, previous = 0;
    unsigned nibbles = 0, done = 0;
    while (done < rows) {
        unsigned count = 0, colour = 0;
        if (peek(6) == 0x3F) {                      // inline: 3 bits count - 1, 4 bits colour
            bit += 6;
            const int c = peek(3), v = (bit += 3, peek(4));
            if (c < 0 || v < 0)
                return {};
            bit += 4;
            count = (unsigned)c + 1;
            colour = (unsigned)v;
        } else {
            bool found = false;
            for (unsigned length = 1; length <= 8 && !found; length++) {
                const int v = peek(length);
                for (const Code& c : codes)
                    if (c.length == length && (int)c.code == v) {
                        bit += length;
                        count = c.count;
                        colour = c.colour;
                        found = true;
                        break;
                    }
            }
            if (!found)
                return {};
        }
        for (unsigned i = 0; i < count && done < rows; i++) {
            row = row << 4 | colour;
            if (++nibbles == 8) {
                if (xor_mode)
                    row = previous ^= row;
                for (int s = 24; s >= 0; s -= 8)
                    out.push_back((uint8_t)(row >> s));
                row = 0;
                nibbles = 0;
                done++;
            }
        }
    }
    return out;
}

std::string apply_coop(std::vector<uint8_t>& rom)
{
    // The bubble (CoopArt): the orb's tiles, its white and cyans (11-15) to palette line 0's
    // white (1), lavender (12), blue (11) and dark blue (10).
    std::vector<uint8_t> orb = nemesis_decode(rom, kOrbArt);
    if (orb.size() != kOrbTiles * 32)
        return "Co-op: the bubble's art isn't where it should be";
    static const uint8_t kOrbColours[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 1, 12, 12, 11, 10};
    for (uint8_t& b : orb)
        b = (uint8_t)(kOrbColours[b >> 4] << 4 | kOrbColours[b & 15]);
    if (rom.size() < kCoopArt + orb.size())
        rom.resize(kCoopArt + orb.size(), 0xFF);
    std::copy(orb.begin(), orb.end(), rom.begin() + kCoopArt);
    // The finale's orb and burst (FinaleArt, CoopOrbTiles): graphics set $2B's first tiles,
    // which co-op's own tiles overwrite in VRAM.
    std::vector<uint8_t> finale = nemesis_decode(rom, kFinaleSet);
    if (finale.size() < kFinaleTiles * 32)
        return "Co-op: the finale's orb art isn't where it should be";
    if (rom.size() < kFinaleArt + kFinaleTiles * 32)
        rom.resize(kFinaleArt + kFinaleTiles * 32, 0xFF);
    std::copy(finale.begin(), finale.begin() + kFinaleTiles * 32, rom.begin() + kFinaleArt);
    // The routine the game calls for a sound: PlaySound, or with an MD+ pack its own (the
    // pack's ROM changes the game's calls; the death's at $00AF10 tells which). Its start
    // jumps to co-op's (CoopPlaySound, CoopPlaySoundMd), which keeps the music asked for, to
    // ask for it again after a death's jingle; a pack whose routine starts otherwise is left
    // alone (the stage's music comes back instead).
    const uint32_t sound = (uint32_t)rom[0xAF12] << 24 | rom[0xAF13] << 16 | rom[0xAF14] << 8 | rom[0xAF15];
    static const std::vector<uint8_t> kPlaySound = {0x46, 0xFC, 0x27, 0x00, 0x48, 0xE7, 0xC0, 0x00};
    static const std::vector<uint8_t> kMdPlusSound = {0x61, 0x02, 0x4E, 0x75, 0x10, 0x38, 0xF4, 0x0E};
    for (int i = 0; i < 4; i++)
        rom[kCoopSoundRoutine + i] = (uint8_t)(sound >> (24 - 8 * i));
    if (sound + 8 <= rom.size()) {
        const auto start = rom.begin() + sound;
        if (std::equal(kPlaySound.begin(), kPlaySound.end(), start)) {
            const auto code = jmp(kCoopPlaySound) + kNop;
            std::copy(code.begin(), code.end(), start);
        } else if (std::equal(kMdPlusSound.begin(), kMdPlusSound.end(), start)) {
            const auto code = jmp(kCoopPlaySoundMd);
            std::copy(code.begin(), code.end(), start);
        }
    }
    // (Applied after the cheats.) TouchHurtsMickey2's patch may meet the Invincibility cheat's
    // rts at $0092B4; its wrapper replays the 6 bytes it replaces, whichever they are.
    static const std::vector<uint8_t> kInvincible = {0x7E, 0x00, 0x4E, 0x75, 0xF4, 0x46};
    const bool invincible = rom.size() > 0x0092B8 && std::equal(kInvincible.begin(), kInvincible.end(), rom.begin() + 0x0092B2);
    for (const Patch& p : coop_patches())
        for (size_t i = 0; i < p.original.size(); i++)
            if (p.address + i >= rom.size() || (rom[p.address + i] != p.original[i] && !(invincible && p.address == 0x0092B2))) {
                char message[160];
                std::snprintf(message, sizeof(message), "Co-op: unexpected bytes at $%06X (%s)",
                              p.address + (unsigned)i, p.what);
                return message;
            }
    std::copy(rom.begin() + 0x0092B2, rom.begin() + 0x0092B8, rom.begin() + kCoopHurts2Replay);
    for (const Patch& p : coop_patches())
        std::copy(p.replacement.begin(), p.replacement.end(), rom.begin() + p.address);
    return "";
}

std::string apply_boss_rush(std::vector<uint8_t>& rom)
{
    for (const Patch& p : patches())
        for (size_t i = 0; i < p.original.size(); i++)
            if (p.address + i >= rom.size() || rom[p.address + i] != p.original[i]) {
                char message[160];
                std::snprintf(message, sizeof(message), "Boss Rush: unexpected bytes at $%06X (%s)",
                              p.address + (unsigned)i, p.what);
                return message;
            }
    const size_t needed = kBossRushOrigin + sizeof(kBossRushCode);
    if (rom.size() < needed)
        rom.resize(needed, 0xFF);
    std::copy(std::begin(kBossRushCode), std::end(kBossRushCode), rom.begin() + kBossRushOrigin);
    for (const Patch& p : patches())
        std::copy(p.replacement.begin(), p.replacement.end(), rom.begin() + p.address);
    return "";
}
