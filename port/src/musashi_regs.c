/* Musashi's data and address registers, for code outside Musashi that copies them in bulk
   (translated code going in and out, main.cpp), without going through m68k_get_reg one by one. */
#include "m68kcpu.h"

unsigned int* coi_m68k_registers(void)
{
    return m68ki_cpu.dar;
}

/* Musashi's stack pointer not in use: the USP in supervisor mode, the SSP (the 68000's ISP) in
   user mode. Where it is depends on the mode, so it's asked again after an SR change. */
unsigned int* coi_m68k_usp(void)
{
    return FLAG_S ? &REG_SP_BASE[0] : &REG_SP_BASE[4];
}

/* For taking an interrupt as m68ki_exception_interrupt does (main.cpp, coi_native_interrupt):
   a non-maskable one is taken once; the cycles the exception takes; and whether Musashi is
   halted (a double fault), when it must go on itself. */
void coi_m68k_clear_nmi(void)
{
    m68ki_cpu.nmi_pending = 0;
}

unsigned int coi_m68k_exception_cycles(unsigned int vector)
{
    return CYC_EXCEPTION[vector];
}

int coi_m68k_halted(void)
{
    return (CPU_STOPPED & STOP_LEVEL_HALT) != 0;
}

/* Waiting at a stop instruction (STOP_LEVEL_STOP), for the port's own CPU state to follow. */
int coi_m68k_stopped(void)
{
    return (CPU_STOPPED & STOP_LEVEL_STOP) != 0;
}

void coi_m68k_set_stopped(int stopped)
{
    CPU_STOPPED = (CPU_STOPPED & ~STOP_LEVEL_STOP) | (stopped ? STOP_LEVEL_STOP : 0);
}

/* The cycles a reset takes, which m68k_execute eats first: taken (and cleared) by the native
   loop, which does the same. */
int coi_m68k_take_reset_cycles(void)
{
    const int n = RESET_CYCLES;
    RESET_CYCLES = 0;
    return n;
}

/* A reset done by the port itself (main.cpp, cpu_reset), as m68k_pulse_reset leaves what isn't
   a register: not stopped, no reset cycles of Musashi's own (the port's loop counts them). */
void coi_m68k_reset_state(void)
{
    CPU_STOPPED = 0;
    RESET_CYCLES = 0;
}

/* The interrupt Musashi would take at an SR write (m68ki_check_interrupts): the level waiting
   (0-7), or 8 for a non-maskable one, which is taken whatever the mask. */
unsigned int coi_m68k_irq_level(void)
{
    return m68ki_cpu.nmi_pending ? 8 : CPU_INT_LEVEL >> 8;
}

/* Save states (savestate.h): Musashi's data, field by field (its callbacks and cycle tables,
   which are pointers, stay as they are). COI_M68K_STATE_WORDS words. */
#define COI_M68K_STATE_FIELDS(X) \
    X(ppc) X(pc) X(vbr) X(sfc) X(dfc) X(ir) X(t1_flag) X(t0_flag) X(s_flag) X(m_flag) X(x_flag) \
    X(n_flag) X(not_z_flag) X(v_flag) X(c_flag) X(int_mask) X(int_level) X(stopped) X(pref_addr) \
    X(pref_data) X(instr_mode) X(run_mode) X(reset_cycles) X(virq_state) X(nmi_pending)

unsigned int coi_m68k_state_save(unsigned int* out)
{
    unsigned int n = 0, i;
    for (i = 0; i < 16; i++) out[n++] = m68ki_cpu.dar[i];
    for (i = 0; i < 7; i++) out[n++] = m68ki_cpu.sp[i];
#define COI_SAVE(f) out[n++] = (unsigned int)m68ki_cpu.f;
    COI_M68K_STATE_FIELDS(COI_SAVE)
#undef COI_SAVE
    return n;
}

void coi_m68k_state_load(const unsigned int* in)
{
    unsigned int n = 0, i;
    for (i = 0; i < 16; i++) m68ki_cpu.dar[i] = in[n++];
    for (i = 0; i < 7; i++) m68ki_cpu.sp[i] = in[n++];
#define COI_LOAD(f) m68ki_cpu.f = in[n++];
    COI_M68K_STATE_FIELDS(COI_LOAD)
#undef COI_LOAD
}
