/* The runtime's operations (native_rt.h) as functions of one shape, for the machine code the
 * app's translator makes (x64.cpp) to call: uint32_t (CoiCpu*, x, y, z). */
#include "native_rt.h"

uint32_t coi_h_read(CoiCpu* c, uint32_t size, uint32_t a, uint32_t z) { (void)c; (void)z; return rt_read((int)size, a); }
uint32_t coi_h_write(CoiCpu* c, uint32_t size, uint32_t a, uint32_t v) { (void)c; rt_write((int)size, a, v); return 0; }
uint32_t coi_h_add(CoiCpu* c, uint32_t size, uint32_t s, uint32_t d) { return rt_add(c, (int)size, s, d); }
uint32_t coi_h_sub(CoiCpu* c, uint32_t size, uint32_t s, uint32_t d) { return rt_sub(c, (int)size, s, d); }
uint32_t coi_h_addx(CoiCpu* c, uint32_t size, uint32_t s, uint32_t d) { return rt_addx(c, (int)size, s, d); }
uint32_t coi_h_subx(CoiCpu* c, uint32_t size, uint32_t s, uint32_t d) { return rt_subx(c, (int)size, s, d); }
uint32_t coi_h_cmp(CoiCpu* c, uint32_t size, uint32_t s, uint32_t d) { rt_cmp(c, (int)size, s, d); return 0; }
uint32_t coi_h_irq_level(CoiCpu* c, uint32_t x, uint32_t y, uint32_t z) { (void)c; (void)x; (void)y; (void)z; return coi_native_irq_level(); }
uint32_t coi_h_logic(CoiCpu* c, uint32_t size, uint32_t v, uint32_t z) { (void)z; return rt_logic(c, (int)size, v); }
uint32_t coi_h_shift(CoiCpu* c, uint32_t kind_size, uint32_t v, uint32_t count)
{
    return rt_shift(c, (int)(kind_size & 0xFF), (int)(kind_size >> 8), v, (int)count);
}
uint32_t coi_h_cond(CoiCpu* c, uint32_t cond, uint32_t y, uint32_t z) { (void)y; (void)z; return (uint32_t)rt_cond(c, (int)cond); }
uint32_t coi_h_ccr(CoiCpu* c, uint32_t x, uint32_t y, uint32_t z) { (void)x; (void)y; (void)z; return rt_ccr(c); }
uint32_t coi_h_set_ccr(CoiCpu* c, uint32_t v, uint32_t y, uint32_t z) { (void)y; (void)z; rt_set_ccr(c, v); return 0; }
uint32_t coi_h_mulu_cycles(CoiCpu* c, uint32_t s, uint32_t y, uint32_t z) { (void)c; (void)y; (void)z; return (uint32_t)rt_mulu_cycles(s); }
uint32_t coi_h_muls_cycles(CoiCpu* c, uint32_t s, uint32_t y, uint32_t z) { (void)c; (void)y; (void)z; return (uint32_t)rt_muls_cycles(s); }
uint32_t coi_h_divu(CoiCpu* c, uint32_t r, uint32_t s, uint32_t z) { (void)z; return (uint32_t)rt_divu(c, (int)r, s); }
uint32_t coi_h_divs(CoiCpu* c, uint32_t r, uint32_t s, uint32_t z) { (void)z; return (uint32_t)rt_divs(c, (int)r, s); }
uint32_t coi_h_push32(CoiCpu* c, uint32_t v, uint32_t y, uint32_t z) { (void)y; (void)z; rt_push32(c, v); return 0; }
uint32_t coi_h_pop32(CoiCpu* c, uint32_t x, uint32_t y, uint32_t z) { (void)x; (void)y; (void)z; return rt_pop32(c); }
uint32_t coi_h_interrupt(CoiCpu* c, uint32_t pc, uint32_t y, uint32_t z) { (void)y; (void)z; return coi_native_interrupt(c, pc); }
uint32_t coi_h_exception(CoiCpu* c, uint32_t vector, uint32_t pc, uint32_t opcode) { return coi_native_exception(c, vector, pc, opcode); }
uint32_t coi_h_stop(CoiCpu* c, uint32_t sr, uint32_t next, uint32_t z) { (void)z; return coi_native_stop(c, sr, next); }
uint32_t coi_h_spin(CoiCpu* c, uint32_t opcode, uint32_t y, uint32_t z) { (void)y; (void)z; rt_spin(c, (int)opcode); return 0; }
uint32_t coi_h_set_sr(CoiCpu* c, uint32_t v, uint32_t y, uint32_t z) { (void)y; (void)z; rt_set_sr(c, v); return 0; }
uint32_t coi_h_trace(CoiCpu* c, uint32_t address, uint32_t y, uint32_t z)
{
    (void)y; (void)z;
#ifdef COI_NATIVE_TRACE
    coi_native_trace(c, address);
#else
    (void)c; (void)address;
#endif
    return 0;
}
