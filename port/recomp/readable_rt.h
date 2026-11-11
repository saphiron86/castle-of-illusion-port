/* The names the readable C (coi_recomp --readable, docs/decompile.md) is written in: the 68000's
 * registers, memory, flags and cycles, over the translator's runtime (native_rt.h). Every
 * function has `CoiCpu* c`, the CPU it runs on. Part of the repository; the generated C that
 * uses it is game code and stays in the build folder. */
#pragma once

#include "native_rt.h"

/* Registers (32 bits; a sized part is a cast, or set_b/set_w to write it). */
#define D0 (c->d[0])
#define D1 (c->d[1])
#define D2 (c->d[2])
#define D3 (c->d[3])
#define D4 (c->d[4])
#define D5 (c->d[5])
#define D6 (c->d[6])
#define D7 (c->d[7])
#define A0 (c->a[0])
#define A1 (c->a[1])
#define A2 (c->a[2])
#define A3 (c->a[3])
#define A4 (c->a[4])
#define A5 (c->a[5])
#define A6 (c->a[6])
#define A7 (c->a[7])
#define SP (c->a[7])
#define set_b(r, v) ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(v) & 0xFFu))
#define set_w(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(v) & 0xFFFFu))
#define sx8(v) ((uint32_t)(int32_t)(int8_t)(v))
/* A RAM address as a .w operand loads it: sign-extended from 16 bits, so the top byte is set. */
#define WORD_ADDRESS(a) ((uint32_t)(a) | 0xFF000000u)
#define sx16(v) ((uint32_t)(int32_t)(int16_t)(v))

/* Memory (24-bit addresses, the bus's own decoding: RAM, ROM, VDP, I/O). */
#define read8(a) rt_read(1, (a))
#define read16(a) rt_read(2, (a))
#define read32(a) rt_read(4, (a))
#define write8(a, v) rt_write(1, (a), (v))
#define write16(a, v) rt_write(2, (a), (v))
#define write32(a, v) rt_write(4, (a), (v))
#define push32(v) rt_push32(c, (v))
#define pop32() rt_pop32(c)

/* Flags: set as the 68000 sets them (each returns its result), and tested by condition. */
#define add_flags(size, s, d) rt_add(c, (size), (s), (d))
#define sub_flags(size, s, d) rt_sub(c, (size), (s), (d))
#define addx_flags(size, s, d) rt_addx(c, (size), (s), (d))
#define subx_flags(size, s, d) rt_subx(c, (size), (s), (d))
#define cmp_flags(size, s, d) rt_cmp(c, (size), (s), (d))
#define test_flags(size, v) rt_logic(c, (size), (v))
#define shift_flags(kind, size, v, n) rt_shift(c, (kind), (size), (v), (int)(n))
#define CCR() rt_ccr(c)
#define set_ccr(v) rt_set_ccr(c, (v))
#define HI() rt_cond(c, 2)
#define LS() rt_cond(c, 3)
#define CC() rt_cond(c, 4)
#define CS() rt_cond(c, 5)
#define NE() rt_cond(c, 6)
#define EQ() rt_cond(c, 7)
#define VC() rt_cond(c, 8)
#define VS() rt_cond(c, 9)
#define PL() rt_cond(c, 10)
#define MI() rt_cond(c, 11)
#define GE() rt_cond(c, 12)
#define LT() rt_cond(c, 13)
#define GT() rt_cond(c, 14)
#define LE() rt_cond(c, 15)

/* dbcc: unless the condition holds, counts register r's low word down and says whether to
 * branch back (not at -1), with the cycles Musashi counts for each way out. */
RT_INLINE int rt_dbcc(CoiCpu* c, int cond, int r)
{
    if (rt_cond(c, cond))
        return 0;
    const uint32_t w = (c->d[r] - 1) & 0xFFFFu;
    rt_setd(c, r, 2, w);
    if (w != 0xFFFFu) {
        c->cycles -= 2;
        return 1;
    }
    c->cycles += 2;
    return 0;
}
#define dbra(r) rt_dbcc(c, 1, (r))
#define dbcc(cond, r) rt_dbcc(c, (cond), (r))

/* Cycles, and the 68000's PC for the port's hooks (each instruction's address, as it starts). */
#define CYCLES(n) (c->cycles += (n))
#define AT(address) RT_TRACE(c, (address))
/* Before each stretch of straight-line code: if the most it can use might not leave the slice's
 * cycles unspent, the rest of this call runs in the resumable copy, which stops exactly where
 * the 68000 would. */
#define BUDGET(most, address, resumable) \
    if (c->cycle_limit && c->cycles + (most) >= c->cycle_limit) return resumable(c, (address))
/* A call (jsr/bsr: the return address pushed, then the function run); it must come back where
 * the 68000's rts would. CALLED: the same with the return address pushed already (a call through
 * a RAM stub, whose jmp is checked between the two). */
#define CALLED(fn, ret) \
    do { const uint32_t r_ = coi_native_run(c, (fn), 0); if (c->fail) return 0; \
         if (r_ != (ret)) { c->fail = r_; return 0; } } while (0)
#define CALL(fn, ret) do { push32(ret); CALLED(fn, ret); } while (0)
/* The readable C's settings (docs/decompile.md): bit k set while setting k of the image family
 * is on (widescreen, smooth swinging, each cheat), each patched function's variant picked by it
 * (defined in main.cpp's native_dev.inc). */
extern uint32_t coi_native_settings;
/* One of the port's settings that patch the ROM is on (SETTING_<name>: its bit, which the
 * generated code defines from the image family): config/patches.txt's hand-written C runs. */
#define SETTING(name) (coi_native_settings & SETTING_##name)
/* In a patch of several instructions: the next starts (the resumable copy can stop there). */
#define STEP(address) AT(address)
/* The same after a call in the patch (a jsr to the setting's own routine, then a nop): the callee's
 * cycles weren't in the stretch's budget, so if they used up the slice, the 68000 stops here and so
 * does the C (the rest in the resumable copy). The generator writes it for a STEP after a CALL. */
#define STEP_AFTER_CALL(address, resumable) \
    AT(address); if (c->cycle_limit && c->cycles >= c->cycle_limit) return resumable(c, (address))
/* A jmp to another function: run by coi_native_run's loop. */
#define JUMP(address) do { c->jump = 1; return (address); } while (0)
