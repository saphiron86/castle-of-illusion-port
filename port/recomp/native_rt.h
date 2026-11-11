/* The runtime that recompiled 68000 code (written by port/recomp into the build folder) runs
 * on: the register file, memory access through the port's bus (the same functions Musashi
 * calls), and each operation's result and flags, following Musashi's 68000 exactly, so
 * recompiled and interpreted code can be compared write for write. C, so the generated code
 * can be compiled as C. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CoiCpu {
    uint32_t d[8], a[8];    /* a[7] is the active stack pointer */
    uint8_t x, n, z, v, c;  /* condition codes, each 0 or 1 */
    uint16_t sr_high;       /* the status register's system byte (T, S, interrupt mask) */
    int64_t cycles;         /* 68000 clock cycles used, counted as Musashi counts them */
    int64_t cycle_limit;    /* stop before an instruction once this many cycles are used (0: no limit) */
    uint32_t fail;          /* nonzero: translated code stopped before the instruction here (the
                               budget ran out, or something it doesn't translate). Registers,
                               flags, memory and cycles are then exactly what the interpreter's
                               would be at that instruction, so it can carry on from there. */
    uint32_t missing;       /* when it stopped at a call to code not translated: that code */
    uint8_t jump;           /* set with the return value: a jmp out of the function, to there */
    uint32_t pc;            /* the instruction running, for the port's checks of where the
                               CPU is (Musashi's PPC, which translated code doesn't keep) */
    uint32_t usp;           /* the stack pointer not in use: the USP in supervisor mode, the SSP
                               in user mode (a[7] is the one in use, as Musashi's REG_SP) */
    uint8_t stopped;        /* a stop instruction is waiting for an interrupt */
} CoiCpu;

/* The port's bus (main.cpp), as Musashi uses it. */
unsigned int m68k_read_memory_8(unsigned int address);
unsigned int m68k_read_memory_16(unsigned int address);
unsigned int m68k_read_memory_32(unsigned int address);
void m68k_write_memory_8(unsigned int address, unsigned int value);
void m68k_write_memory_16(unsigned int address, unsigned int value);
void m68k_write_memory_32(unsigned int address, unsigned int value);

#define RT_INLINE static inline

/* Musashi's 68000 cycle table (each opcode with its addressing), filled by m68k_init. */
extern unsigned char m68ki_cycles[][0x10000];
#define RT_CYCLES(c, opcode) ((c)->cycles += m68ki_cycles[0][opcode])
/* Before each instruction: the interpreter runs an instruction only while cycles remain. */
#define RT_LIMIT(c, address) if ((c)->cycle_limit && (c)->cycles >= (c)->cycle_limit) { (c)->fail = (address); return 0; }
/* A busy-wait loop's branch back to its tst, taken, with the tst going to read `value` (of
   `size` bytes) every turn and loop again: n more turns of the loop at once, as if stepped
   (not while tracing, which sees every instruction). The branch began with x cycles used, x
   below the limit, and its own cycles are already counted; stepping would pass this branch
   again at x + n * (tst + branch) for as long as that stays below the limit. After any such
   turn, the flags are the tst's. */
#define RT_IDLE(c, again, value, size, tst_opcode, opcode) \
    if ((again) && (c)->cycle_limit && !coi_native_tracing) { \
        const int64_t turn_ = m68ki_cycles[0][tst_opcode] + m68ki_cycles[0][opcode]; \
        const int64_t n_ = ((c)->cycle_limit - ((c)->cycles - m68ki_cycles[0][opcode]) - 1) / turn_; \
        if (n_ > 0) { \
            (c)->cycles += n_ * turn_; \
            rt_logic(c, size, value); \
        } \
    }
/* A bra or jmp to itself, as Musashi's: USE_ALL_CYCLES, what's left of the slice before it modulo
   its own cycles, which are then counted on top; so the slice ends there. */
RT_INLINE void rt_spin(CoiCpu* c, int opcode)
{
    if (!c->cycle_limit)
        return;
    const int64_t own = m68ki_cycles[0][opcode];
    const int64_t left = (c->cycle_limit - (c->cycles - own)) % own;
    c->cycles = c->cycle_limit - (left - own);
}
/* Stops at the instruction being run, before it does anything: its cycles are given back. */
#define RT_STOP(c, address, opcode) { (c)->cycles -= m68ki_cycles[0][opcode]; (c)->fail = (address); return 0; }

/* mulu/muls take longer the more 1 bits (mulu) or bit changes (muls) the source has. */
RT_INLINE int rt_mulu_cycles(uint32_t s)
{
    int n = 0;
    for (uint32_t y = s & 0xFFFF; y; y >>= 1) n += (y & 1) * 2;
    return n;
}
RT_INLINE int rt_muls_cycles(uint32_t s)
{
    int n = 0;
    uint32_t f = 0;
    for (uint32_t y = (uint32_t)(int32_t)(int16_t)s; y; y >>= 1)
        if ((y & 1) != f) { n += 2; f = 1 - f; }
    return n;
}

RT_INLINE uint32_t rt_mask(int size) { return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
RT_INLINE uint32_t rt_msb(int size) { return size == 1 ? 0x80u : size == 2 ? 0x8000u : 0x80000000u; }
RT_INLINE uint32_t rt_sext(int size, uint32_t v)
{
    return size == 1 ? (uint32_t)(int32_t)(int8_t)v : size == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
}

RT_INLINE uint32_t rt_read(int size, uint32_t a)
{
    a &= 0xFFFFFF;
    return size == 1 ? m68k_read_memory_8(a) : size == 2 ? m68k_read_memory_16(a) : m68k_read_memory_32(a);
}
RT_INLINE void rt_write(int size, uint32_t a, uint32_t v)
{
    a &= 0xFFFFFF;
    if (size == 1) m68k_write_memory_8(a, v & 0xFF);
    else if (size == 2) m68k_write_memory_16(a, v & 0xFFFF);
    else m68k_write_memory_32(a, v);
}

/* A data register written at a size keeps its other bits. */
RT_INLINE void rt_setd(CoiCpu* c, int r, int size, uint32_t v)
{
    const uint32_t m = rt_mask(size);
    c->d[r] = (c->d[r] & ~m) | (v & m);
}

RT_INLINE void rt_push32(CoiCpu* c, uint32_t v) { c->a[7] -= 4; rt_write(4, c->a[7], v); }
RT_INLINE uint32_t rt_pop32(CoiCpu* c) { uint32_t v = rt_read(4, c->a[7]); c->a[7] += 4; return v; }

RT_INLINE void rt_nz(CoiCpu* c, int size, uint32_t r)
{
    r &= rt_mask(size);
    c->n = (r & rt_msb(size)) != 0;
    c->z = r == 0;
}

/* move, and, or, eor, not, clr, tst, ext, swap, moveq: N and Z from the result, V and C clear. */
RT_INLINE uint32_t rt_logic(CoiCpu* c, int size, uint32_t r)
{
    rt_nz(c, size, r);
    c->v = c->c = 0;
    return r & rt_mask(size);
}

RT_INLINE uint32_t rt_add(CoiCpu* c, int size, uint32_t s, uint32_t d)
{
    const uint32_t m = rt_mask(size), h = rt_msb(size);
    s &= m; d &= m;
    const uint32_t r = (s + d) & m;
    rt_nz(c, size, r);
    c->v = (((s ^ r) & (d ^ r)) & h) != 0;
    c->c = c->x = (((s & d) | (~r & (s | d))) & h) != 0;
    return r;
}

RT_INLINE uint32_t rt_sub(CoiCpu* c, int size, uint32_t s, uint32_t d)  /* d - s */
{
    const uint32_t m = rt_mask(size), h = rt_msb(size);
    s &= m; d &= m;
    const uint32_t r = (d - s) & m;
    rt_nz(c, size, r);
    c->v = (((s ^ d) & (r ^ d)) & h) != 0;
    c->c = c->x = (((s & r) | (~d & (s | r))) & h) != 0;
    return r;
}

RT_INLINE void rt_cmp(CoiCpu* c, int size, uint32_t s, uint32_t d)     /* flags of d - s, X kept */
{
    const uint8_t x = c->x;
    rt_sub(c, size, s, d);
    c->x = x;
}

RT_INLINE uint32_t rt_addx(CoiCpu* c, int size, uint32_t s, uint32_t d)
{
    const uint32_t m = rt_mask(size), h = rt_msb(size);
    s &= m; d &= m;
    const uint32_t r = (s + d + c->x) & m;
    c->n = (r & h) != 0;
    if (r) c->z = 0;
    c->v = (((s ^ r) & (d ^ r)) & h) != 0;
    c->c = c->x = (((s & d) | (~r & (s | d))) & h) != 0;
    return r;
}

RT_INLINE uint32_t rt_subx(CoiCpu* c, int size, uint32_t s, uint32_t d)
{
    const uint32_t m = rt_mask(size), h = rt_msb(size);
    s &= m; d &= m;
    const uint32_t r = (d - s - c->x) & m;
    c->n = (r & h) != 0;
    if (r) c->z = 0;
    c->v = (((s ^ d) & (r ^ d)) & h) != 0;
    c->c = c->x = (((s & r) | (~d & (s | r))) & h) != 0;
    return r;
}

/* Shifts and rotates by `count` (0-63), as the 68000 does them one bit at a time. */
RT_INLINE uint32_t rt_shift(CoiCpu* c, int kind, int size, uint32_t v, int count)
{
    /* kind: 0 asl, 1 asr, 2 lsl, 3 lsr, 4 rol, 5 ror, 6 roxl, 7 roxr */
    const uint32_t m = rt_mask(size), h = rt_msb(size);
    v &= m;
    uint8_t carry = 0, overflow = 0;
    for (int i = 0; i < count; i++) {
        uint32_t out;
        switch (kind) {
        case 0: case 2: case 4: case 6:
            out = (v & h) != 0;
            v = (v << 1) & m;
            if (kind == 4) v |= out;
            if (kind == 6) v |= c->x;
            if (kind == 0 && ((v & h) != 0) != (out != 0)) overflow = 1;
            break;
        default:
            out = v & 1;
            if (kind == 1) v = (v >> 1) | (v & h);
            else if (kind == 3) v >>= 1;
            else if (kind == 5) v = (v >> 1) | (out ? h : 0);
            else v = (v >> 1) | (c->x ? h : 0);
            break;
        }
        carry = (uint8_t)out;
        if (kind != 4 && kind != 5) c->x = carry;
    }
    rt_nz(c, size, v);
    c->v = kind == 0 ? overflow : 0;
    if (count == 0)
        c->c = (kind == 6 || kind == 7) ? c->x : 0;
    else
        c->c = carry;
    return v;
}

/* Conditions 0-15: t f hi ls cc cs ne eq vc vs pl mi ge lt gt le. */
RT_INLINE int rt_cond(const CoiCpu* c, int cond)
{
    switch (cond) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !c->c && !c->z;
    case 3: return c->c || c->z;
    case 4: return !c->c;
    case 5: return c->c;
    case 6: return !c->z;
    case 7: return c->z;
    case 8: return !c->v;
    case 9: return c->v;
    case 10: return !c->n;
    case 11: return c->n;
    case 12: return c->n == c->v;
    case 13: return c->n != c->v;
    case 14: return !c->z && c->n == c->v;
    default: return c->z || c->n != c->v;
    }
}

RT_INLINE uint32_t rt_ccr(const CoiCpu* c)
{
    return (uint32_t)(c->x << 4 | c->n << 3 | c->z << 2 | c->v << 1 | c->c);
}
RT_INLINE void rt_set_ccr(CoiCpu* c, uint32_t v)
{
    c->x = (v >> 4) & 1; c->n = (v >> 3) & 1; c->z = (v >> 2) & 1; c->v = (v >> 1) & 1; c->c = v & 1;
}
/* An SR write, as Musashi's m68ki_set_sr_noint: the system byte (T, S, the mask; with trace not
   emulated, T is only kept) and the CCR; going into or out of supervisor mode swaps the stack
   pointers. */
RT_INLINE void rt_set_sr(CoiCpu* c, uint32_t v)
{
    const uint16_t was = c->sr_high & 0x20;
    c->sr_high = (uint16_t)((v >> 8) & 0xA7);
    rt_set_ccr(c, v);
    if ((c->sr_high & 0x20) != was) {
        const uint32_t other = c->usp;
        c->usp = c->a[7];
        c->a[7] = other;
    }
}

/* divu/divs of data register r by the word s: 0 (nothing changed) when s is 0, which traps. */
RT_INLINE int rt_divu(CoiCpu* c, int r, uint32_t s)
{
    if (s == 0)
        return 0;
    const uint32_t q = c->d[r] / s, m = c->d[r] % s;
    if (q < 0x10000u) {
        c->d[r] = (q & 0xFFFFu) | (m << 16);
        c->z = q == 0; c->n = (q & 0x8000u) != 0; c->v = c->c = 0;
    } else {
        c->v = 1;
    }
    return 1;
}
RT_INLINE int rt_divs(CoiCpu* c, int r, uint32_t s)
{
    if (s == 0)
        return 0;
    const int32_t ds = (int16_t)s;
    if (c->d[r] == 0x80000000u && ds == -1) {
        c->d[r] = 0;
        c->z = 0; c->n = c->v = c->c = 0;
        return 1;
    }
    const int32_t q = (int32_t)c->d[r] / ds, m = (int32_t)c->d[r] % ds;
    if (q == (int16_t)q) {
        c->d[r] = ((uint32_t)q & 0xFFFFu) | ((uint32_t)m << 16);
        c->z = (q & 0xFFFF) == 0; c->n = (q & 0x8000) != 0; c->v = c->c = 0;
    } else {
        c->v = 1;
    }
    return 1;
}

/* As each translated instruction starts: its address, and for the test harness (builds with
 * COI_NATIVE_TRACE) a trace of the address and registers. */
#ifdef COI_NATIVE_TRACE
extern int coi_native_tracing;          /* nonzero while the harness wants the trace */
void coi_native_trace(const CoiCpu* c, uint32_t address);
#define RT_TRACE(c, address) ((c)->pc = (address), coi_native_tracing ? coi_native_trace(c, address) : (void)0)
#else
#define RT_TRACE(c, address) ((c)->pc = (address))
#endif

/* Entry points: the recompiled function for a 68000 address (0 if there is none), called
 * with the return address on the stack as a jsr leaves it; it returns the address its rts
 * went to. */
/* A translated function, run from its entry (at = 0) or resumed at any of its instructions. */
typedef uint32_t (*CoiNativeFn)(CoiCpu*, uint32_t at);
/* Every translated instruction, sorted by address, with a function that can resume there. */
typedef struct CoiResume { uint32_t address; CoiNativeFn fn; } CoiResume;
extern const CoiResume coi_native_resume[];
extern const int coi_native_resume_count;
/* The ROM image the code was translated from: its size and FNV-1a hash (coi_native_hash).
 * Translated code is only right for exactly that image. */
extern const uint32_t coi_native_rom_size;
extern const uint64_t coi_native_rom_hash;
RT_INLINE uint64_t coi_native_hash(const uint8_t* p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
/* Runs `fn` from `at` (0: its entry) and on through any jmps out of it, to the address the
 * last one's rts returns to (or until it stops: cpu->fail). */
uint32_t coi_native_run(CoiCpu* cpu, CoiNativeFn fn, uint32_t at);
CoiNativeFn coi_native_lookup(uint32_t address);
extern const uint32_t coi_native_entries[];
extern const int coi_native_entry_count;

/* A set of translated code, as the port runs it: the machine code the app makes itself
 * (port/recomp/x64.cpp, arm64.cpp), or a translation a development build links in. */
typedef struct CoiNativeCode {
    CoiNativeFn (*lookup)(uint32_t address);
    uint32_t (*run)(CoiCpu* cpu, CoiNativeFn fn, uint32_t at);
    const CoiResume* resume;
    int resume_count;
    const uint32_t* entries;    /* the functions asked for (the harness saves calls of these) */
    int entry_count;
    uint32_t rom_size;
    uint64_t rom_hash;
} CoiNativeCode;

/* The runtime's operations with one shape, for machine code to call (native_helpers.c):
 * uint32_t (CoiCpu*, x, y, z), unused arguments ignored. */
typedef uint32_t (*CoiHelper)(CoiCpu* c, uint32_t x, uint32_t y, uint32_t z);
uint32_t coi_h_read(CoiCpu* c, uint32_t size, uint32_t a, uint32_t z);
uint32_t coi_h_write(CoiCpu* c, uint32_t size, uint32_t a, uint32_t v);
uint32_t coi_h_add(CoiCpu* c, uint32_t size, uint32_t s, uint32_t d);
uint32_t coi_h_sub(CoiCpu* c, uint32_t size, uint32_t s, uint32_t d);
uint32_t coi_h_addx(CoiCpu* c, uint32_t size, uint32_t s, uint32_t d);
uint32_t coi_h_subx(CoiCpu* c, uint32_t size, uint32_t s, uint32_t d);
uint32_t coi_h_cmp(CoiCpu* c, uint32_t size, uint32_t s, uint32_t d);
uint32_t coi_h_logic(CoiCpu* c, uint32_t size, uint32_t v, uint32_t z);
uint32_t coi_h_irq_level(CoiCpu* c, uint32_t x, uint32_t y, uint32_t z);
/* The interrupt waiting: its level 0-7, or 8 for a non-maskable one (main.cpp, kept by the port
   as Musashi's m68k_set_irq keeps its own). */
unsigned int coi_native_irq_level(void);
uint32_t coi_h_shift(CoiCpu* c, uint32_t kind_size, uint32_t v, uint32_t count);   /* kind | size << 8 */
uint32_t coi_h_cond(CoiCpu* c, uint32_t cond, uint32_t y, uint32_t z);
uint32_t coi_h_ccr(CoiCpu* c, uint32_t x, uint32_t y, uint32_t z);
uint32_t coi_h_set_ccr(CoiCpu* c, uint32_t v, uint32_t y, uint32_t z);
uint32_t coi_h_mulu_cycles(CoiCpu* c, uint32_t s, uint32_t y, uint32_t z);
uint32_t coi_h_muls_cycles(CoiCpu* c, uint32_t s, uint32_t y, uint32_t z);
uint32_t coi_h_divu(CoiCpu* c, uint32_t r, uint32_t s, uint32_t z);
uint32_t coi_h_divs(CoiCpu* c, uint32_t r, uint32_t s, uint32_t z);
uint32_t coi_h_push32(CoiCpu* c, uint32_t v, uint32_t y, uint32_t z);
uint32_t coi_h_pop32(CoiCpu* c, uint32_t x, uint32_t y, uint32_t z);
uint32_t coi_h_trace(CoiCpu* c, uint32_t address, uint32_t y, uint32_t z);
uint32_t coi_h_spin(CoiCpu* c, uint32_t opcode, uint32_t y, uint32_t z);
uint32_t coi_h_set_sr(CoiCpu* c, uint32_t v, uint32_t y, uint32_t z);
uint32_t coi_h_interrupt(CoiCpu* c, uint32_t pc, uint32_t y, uint32_t z);
/* Takes the interrupt waiting, as Musashi's 68000 does (main.cpp): `pc` is the address it
   returns to; returns the handler's. */
uint32_t coi_native_interrupt(CoiCpu* c, uint32_t pc);
uint32_t coi_h_exception(CoiCpu* c, uint32_t vector, uint32_t pc, uint32_t opcode);
uint32_t coi_h_stop(CoiCpu* c, uint32_t sr, uint32_t next, uint32_t z);
/* An exception other than an interrupt, as Musashi's 68000 takes it (main.cpp): its frame's PC,
   the instruction's opcode (its cycles are given back); returns the handler's address. */
uint32_t coi_native_exception(CoiCpu* c, uint32_t vector, uint32_t pc, uint32_t opcode);
/* stop #sr, the next instruction at `next`: returns where the CPU goes on (main.cpp). */
uint32_t coi_native_stop(CoiCpu* c, uint32_t sr, uint32_t next);

#ifdef __cplusplus
}
#endif
